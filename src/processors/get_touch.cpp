// processors/get_touch.cpp
//
// Source processor: one FlowFile per completed touch gesture on the board's
// display -- tap / hold / swipe_{up,down,left,right} with the start/stop
// coordinates, duration, distance and speed the Display service already
// computes. MicroFi-original (#191, rung 4 "GetTouch"), built for the
// Waveshare AMOLED 1.8 V2 as an ESP-Brookesia guest.
//
// The sense is not read from the CST820 directly: Brookesia's Display
// service owns the controller, polls it, and synthesizes gestures for the
// launcher shell (which enables gesture detection on the GUI output at
// start-up with its own edge thresholds). This processor only SUBSCRIBES --
// `service::Display::get_instance().connect_touch_gesture()` -- and never
// calls set_touch_gesture_config(), because that config is per-output,
// last-writer-wins, and overwriting it would silently re-tune the shell's
// swipe-to-home behaviour. First real Brookesia *service* dependency in the
// agent (REQUIRES brookesia_service_display).
//
// Threading: the gesture signal fires synchronously on the Display service's
// touch task (the TouchGesture event has require_scheduler = false), so the
// callback must not block and must not touch the flow engine. It copies a
// compact record into a spinlock-guarded ring; the engine task drains the
// ring in on_trigger (up to kDrainPerTick per 1 s tick) and emits FlowFiles.
// Same seam shape as the DisplayMessage mailbox, in the other direction.
//
// Gesture events: the service emits Press on first contact, Pressing on
// every 20 ms detect tick, and exactly one Release with the complete
// metrics (duration_ms / distance_px / speed_px_per_ms / locked direction).
// Release is the one that means "a gesture happened", so it is the default;
// "Press and Release" is offered for a touch-down trigger, Pressing is never
// forwarded (it would flood the 1 s engine tick).
//
// This file is entirely conditional on MICROFI_BOARD_TOUCH_GESTURE, defined
// only by the AMOLED overlay's CMakeLists.txt -- the XIAO PlatformIO builds
// compile it as an empty translation unit (same pattern as get_imu.cpp).

#ifdef MICROFI_BOARD_TOUCH_GESTURE

#include "microfi/flowfile.h"
#include "microfi/flow_engine.h"
#include "microfi/processor.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/types.h"

#include "brookesia/service_display.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace microfi {
namespace gettouch {

namespace {

static const char* TAG = "microfi.proc.touch";

using DisplayService = esp_brookesia::service::Display;
using DisplayHelper  = esp_brookesia::service::helper::Display;

constexpr uint8_t kOutputJson       = 0;
constexpr uint8_t kOutputAttributes = 1;

constexpr uint8_t kEventsRelease         = 0;  // one FlowFile per completed gesture
constexpr uint8_t kEventsPressAndRelease = 1;  // plus one on touch-down

constexpr size_t kRingSize     = 8;   // gestures buffered between engine ticks
constexpr size_t kDrainPerTick = 4;   // FlowFiles emitted per on_trigger at most

// One buffered gesture. Plain data only -- copied under a critical section
// on the Display touch task, read back on the engine task.
struct Event {
    uint8_t  type;         // 0 = press, 1 = release
    uint8_t  direction;    // 0 none, 1 up, 2 down, 3 left, 4 right
    bool     short_duration;
    int16_t  x, y;         // start point (output-normalized px)
    int16_t  x2, y2;       // stop point
    uint32_t duration_ms;
    float    distance_px;
    float    speed_px_per_ms;
    int64_t  ts_us;        // esp_timer_get_time() at the callback
};

struct State {
    bool     subscribed;
    bool     subscribe_failed;   // logged once; retried on next flow apply
    uint8_t  output_format;
    uint8_t  events;
    uint32_t tick;
};
static_assert(sizeof(State) <= 256, "State larger than engine slab");

// The ring and the signal connection are file-scope (one display, one
// subscriber), not per-node: the connection outlives a node slab rebuild
// and is torn down explicitly in on_stop. Lands in PSRAM via the agent's
// extram_bss mapping; the critical section is task-to-task, never ISR.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static Event    s_ring[kRingSize];
static size_t   s_head    = 0;   // next write
static size_t   s_count   = 0;   // buffered events
static uint32_t s_dropped = 0;   // oldest-evicted count, for the log
static uint8_t  s_events_mode = kEventsRelease;  // mirrored from State for the callback
static esp_brookesia::lib_utils::connection s_connection;

static const AllowableValue kOutputFormatValues[] = {
    { "JSON",       nullptr },
    { "Attributes", nullptr },
};

static const AllowableValue kEventsValues[] = {
    { "Release",           nullptr },
    { "Press and Release", nullptr },
};

static const PropertyDescriptor kProperties[] = {
    {
        /* name          */ "Events",
        /* description   */ "Release: one FlowFile per completed gesture (tap, hold, "
                            "swipe) with its final duration/distance/speed. Press and "
                            "Release: additionally one FlowFile on touch-down "
                            "(gesture=press, no metrics yet).",
        /* default_value */ "Release",
        /* required      */ false,
        /* allowable     */ kEventsValues, 2,
    },
    {
        /* name          */ "Output Format",
        /* description   */ "JSON: gesture record as FlowFile content. Attributes: "
                            "touch.gesture/x/y/x2/y2/duration_ms as attributes and "
                            "empty content (distance/speed are JSON-only -- FlowFile "
                            "attribute cap).",
        /* default_value */ "JSON",
        /* required      */ false,
        /* allowable     */ kOutputFormatValues, 2,
    },
};
static constexpr size_t kPropertyCount =
    sizeof(kProperties) / sizeof(kProperties[0]);

// ---- Display-task side ----------------------------------------------------

uint8_t direction_code(DisplayService::TouchGestureDirection d) {
    switch (d) {
        case DisplayService::TouchGestureDirection::Up:    return 1;
        case DisplayService::TouchGestureDirection::Down:  return 2;
        case DisplayService::TouchGestureDirection::Left:  return 3;
        case DisplayService::TouchGestureDirection::Right: return 4;
        default:                                          return 0;
    }
}

void on_gesture(const std::string& /*output_name*/,
                const DisplayService::TouchGestureInfo& info) {
    uint8_t type;
    if (info.event_type == DisplayService::TouchGestureEventType::Release) {
        type = 1;
    } else if (info.event_type == DisplayService::TouchGestureEventType::Press &&
               s_events_mode == kEventsPressAndRelease) {
        type = 0;
    } else {
        return;  // Pressing (20 ms stream) is never forwarded
    }

    Event e;
    e.type            = type;
    e.direction       = direction_code(info.direction);
    e.short_duration  = info.flags_short_duration;
    e.x               = static_cast<int16_t>(info.start_x);
    e.y               = static_cast<int16_t>(info.start_y);
    e.x2              = static_cast<int16_t>(info.stop_x);
    e.y2              = static_cast<int16_t>(info.stop_y);
    e.duration_ms     = info.duration_ms;
    e.distance_px     = info.distance_px;
    e.speed_px_per_ms = info.speed_px_per_ms;
    e.ts_us           = esp_timer_get_time();

    taskENTER_CRITICAL(&s_lock);
    s_ring[s_head] = e;
    s_head = (s_head + 1) % kRingSize;
    if (s_count < kRingSize) ++s_count;
    else                     ++s_dropped;  // ring full: oldest evicted
    taskEXIT_CRITICAL(&s_lock);
}

// Pop the oldest buffered event. Returns false when the ring is empty.
bool pop_event(Event* out, uint32_t* dropped) {
    bool ok = false;
    taskENTER_CRITICAL(&s_lock);
    if (s_count > 0) {
        const size_t tail = (s_head + kRingSize - s_count) % kRingSize;
        *out = s_ring[tail];
        --s_count;
        ok = true;
    }
    *dropped  = s_dropped;
    s_dropped = 0;
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}

// ---- engine side ----------------------------------------------------------

Status on_init(void* state) {
    auto* s = static_cast<State*>(state);
    s->subscribed       = false;
    s->subscribe_failed = false;
    s->output_format    = kOutputJson;
    s->events           = kEventsRelease;
    s->tick             = 0;
    return Status::Ok;
}

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s = static_cast<State*>(state);
    for (size_t i = 0; i < count; ++i) {
        const NodeProperty& p = props[i];
        if (std::strcmp(p.key, "Output Format") == 0 && p.value[0] != '\0') {
            s->output_format = (std::strcmp(p.value, "Attributes") == 0)
                                   ? kOutputAttributes : kOutputJson;
        } else if (std::strcmp(p.key, "Events") == 0 && p.value[0] != '\0') {
            s->events = (std::strcmp(p.value, "Press and Release") == 0)
                            ? kEventsPressAndRelease : kEventsRelease;
        }
    }
    s_events_mode = s->events;
}

// Lazy subscribe on first trigger (the Display service is up long before
// the agent's engine task runs its first tick). An empty output name means
// "the first registered output" -- the panel.
bool ensure_subscribed(State* s) {
    if (s_connection.connected()) return true;

    if (!DisplayHelper::is_available()) {
        ESP_LOGW(TAG, "Display service not available -- no touch source");
        return false;
    }
    if (!DisplayHelper::is_running()) {
        ESP_LOGW(TAG, "Display service not running -- no touch source");
        return false;
    }

    taskENTER_CRITICAL(&s_lock);
    s_head = 0; s_count = 0; s_dropped = 0;
    taskEXIT_CRITICAL(&s_lock);

    s_connection = DisplayService::get_instance().connect_touch_gesture("", &on_gesture);
    if (!s_connection.connected()) {
        ESP_LOGW(TAG, "connect_touch_gesture failed");
        return false;
    }
    ESP_LOGI(TAG, "subscribed to Display touch gestures (events=%s)",
             s->events == kEventsPressAndRelease ? "press+release" : "release");
    return true;
}

const char* gesture_name(const Event& e) {
    if (e.type == 0) return "press";
    switch (e.direction) {
        case 1: return "swipe_up";
        case 2: return "swipe_down";
        case 3: return "swipe_left";
        case 4: return "swipe_right";
        default: return e.short_duration ? "tap" : "hold";
    }
}

Status emit(Session& session, State* s, const Event& e) {
    FlowFile f;
    f.assign_id(FlowEngine::instance().next_id());

    char tick_buf[16];
    std::snprintf(tick_buf, sizeof(tick_buf), "%u", static_cast<unsigned>(s->tick));

    Status rc = f.set_attribute("source", "GetTouch");
    if (rc != Status::Ok) return rc;
    rc = f.set_attribute("tickIndex", tick_buf);
    if (rc != Status::Ok) return rc;

    const char* name = gesture_name(e);

    if (s->output_format == kOutputAttributes) {
        // source + tickIndex + 6 = kMaxAttributes exactly (microfi/flowfile.h).
        char buf[16];
        rc = f.set_attribute("touch.gesture", name); if (rc != Status::Ok) return rc;
        std::snprintf(buf, sizeof(buf), "%d", e.x);
        rc = f.set_attribute("touch.x", buf); if (rc != Status::Ok) return rc;
        std::snprintf(buf, sizeof(buf), "%d", e.y);
        rc = f.set_attribute("touch.y", buf); if (rc != Status::Ok) return rc;
        std::snprintf(buf, sizeof(buf), "%d", e.x2);
        rc = f.set_attribute("touch.x2", buf); if (rc != Status::Ok) return rc;
        std::snprintf(buf, sizeof(buf), "%d", e.y2);
        rc = f.set_attribute("touch.y2", buf); if (rc != Status::Ok) return rc;
        std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(e.duration_ms));
        rc = f.set_attribute("touch.duration_ms", buf); if (rc != Status::Ok) return rc;
        rc = f.set_content(nullptr, 0);
        if (rc != Status::Ok) return rc;
    } else {
        // ts: microseconds since boot -- no adopted wall clock on this board
        // (same as GetIMU).
        char content[200];
        const int len = std::snprintf(content, sizeof(content),
            "{\"gesture\":\"%s\",\"event\":\"%s\","
            "\"x\":%d,\"y\":%d,\"x2\":%d,\"y2\":%d,"
            "\"duration_ms\":%u,\"distance_px\":%.1f,\"speed\":%.2f,\"ts\":%lld}",
            name, e.type == 0 ? "press" : "release",
            e.x, e.y, e.x2, e.y2,
            static_cast<unsigned>(e.duration_ms),
            static_cast<double>(e.distance_px),
            static_cast<double>(e.speed_px_per_ms),
            static_cast<long long>(e.ts_us));
        const size_t content_len =
            (len > 0) ? ((static_cast<size_t>(len) < sizeof(content))
                             ? static_cast<size_t>(len)
                             : sizeof(content) - 1)
                      : 0;
        rc = f.set_content(reinterpret_cast<const uint8_t*>(content), content_len);
        if (rc != Status::Ok) return rc;
    }

    rc = session.transfer(f, "success");
    if (rc != Status::Ok) return rc;
    ++s->tick;
    return Status::Ok;
}

Status on_trigger(Session& session, void* state) {
    auto* s = static_cast<State*>(state);

    if (!s->subscribed) {
        if (!ensure_subscribed(s)) {
            if (!s->subscribe_failed) {
                s->subscribe_failed = true;
                ESP_LOGW(TAG, "GetTouch idle until the Display service is up (retrying each tick)");
            }
            return Status::Again;
        }
        s->subscribed = true;
        s->subscribe_failed = false;
    }

    size_t emitted = 0;
    while (emitted < kDrainPerTick) {
        Event e;
        uint32_t dropped = 0;
        if (!pop_event(&e, &dropped)) break;
        if (dropped > 0) {
            ESP_LOGW(TAG, "ring overflow: %u gesture(s) dropped before this tick",
                     static_cast<unsigned>(dropped));
        }
        const Status rc = emit(session, s, e);
        if (rc == Status::Full) return Status::Full;  // downstream back-pressure: event lost
        if (rc != Status::Ok) return rc;
        ++emitted;
    }
    return emitted > 0 ? Status::Ok : Status::Again;
}

void on_stop(void* /*state*/) {
    if (s_connection.connected()) {
        s_connection.disconnect();
        ESP_LOGI(TAG, "unsubscribed from Display touch gestures");
    }
    taskENTER_CRITICAL(&s_lock);
    s_head = 0; s_count = 0; s_dropped = 0;
    taskEXIT_CRITICAL(&s_lock);
}

ProcessorDescriptor descriptor = {
    "GetTouch",
    "Emits one FlowFile per touch gesture on the device's display (tap, "
    "hold, swipe_up/down/left/right) with start/stop coordinates, duration, "
    "distance and speed, as JSON content or touch.* attributes.",
    &on_trigger,
    &on_init,
    &on_configure,
    sizeof(State),
    "INPUT_FORBIDDEN",  // source: no incoming connections
    kProperties,
    kPropertyCount,
    &on_stop,           // releases the Display signal connection on republish
};

}  // namespace
}  // namespace gettouch
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::gettouch::descriptor)

#endif  // MICROFI_BOARD_TOUCH_GESTURE
