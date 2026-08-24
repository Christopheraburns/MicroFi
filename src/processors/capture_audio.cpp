// processors/capture_audio.cpp
//
// CaptureAudio (#191, rung 6): the microphone as a source. Records a short
// clip from the board's ES8311 mic path and publishes it as a 16 kHz /
// 16-bit / mono WAV to an MQTT topic, emitting a small metadata FlowFile
// (JSON: seq/bytes/ms/rate/topic) into the flow chain. Same egress shape as
// CaptureImage: a MicroFi FlowFile carries kInlineContentBytes (256 B) of
// content, and one second of mono 16 kHz PCM is 32 KB, so the audio goes
// broker-direct from this processor while the capture *event* stays a
// normal FlowFile any sink can consume.
//
// The mic is not opened here: Brookesia's AudioEncoder0 service owns the
// codec, the AFE and the GMF recorder pipeline (shared with the wake-word /
// AI-agent path). This processor only SUBSCRIBES to the encoder's raw
// recorder-data signal (`AudioEncoder::connect_recorder_data`) -- the PCM
// as it comes off the codec, before AFE/opus -- so it neither takes the
// exclusive DataFlow capture lease an AI agent holds nor re-tunes the
// encoder. The board recorder is 16 kHz / 16-bit / 2 ch with mic layout
// "MR": channel 0 is the microphone, channel 1 the playback reference the
// AEC uses. Only channel 0 is kept, which is what makes the clip mono.
//
// The AudioEncoder0 *service* is initialized at boot but only started on
// demand (the launcher binds AudioPlayback for its sounds; an AI-agent
// session is what normally binds the encoder). The processor therefore
// holds its own ServiceManager binding -- the launcher's exact pattern --
// which starts the service and its dependencies, and releases it in
// on_stop. The recorder callbacks then only fire while the encoder is
// *started*: if nothing else has started it, the processor starts it
// itself through the helper with a plain PCM config and stops it again in
// on_stop; if data is already flowing when we subscribe, the encoder is
// somebody else's and is left alone.
//
// Threading: the recorder signal fires on the GMF recorder task. The slot
// reserves a slice of the clip buffer under a spinlock and copies outside
// it; the engine task never touches the buffer while `recording` is set.
// The clip buffer is heap PSRAM (task-context only -- the codec DMA never
// sees it), never the engine slab and never the agent's extram_bss statics.
//
// Trigger model: an incoming FlowFile starts a clip (ListenHTTP /record,
// or a GetTouch tap); with no incoming connection the node is a source and
// `Capture Every N Ticks` records on a timer. Either way one on_trigger
// does the whole job synchronously -- record, publish, emit -- because the
// engine only calls a connected node when a FlowFile is queued for it, so
// nothing can be deferred "to the next tick". The engine task blocks for
// Clip Seconds while the recorder fills the buffer; the other nodes' ticks
// slip by that much (ListenHTTP's server task keeps accepting meanwhile).
//
// Whole file conditional on MICROFI_BOARD_CAPTURE_AUDIO, defined only by
// the AMOLED overlay's CMakeLists.txt (XIAO PlatformIO builds compile it as
// an empty translation unit, same as get_imu.cpp / play_audio.cpp).

#ifdef MICROFI_BOARD_CAPTURE_AUDIO

#include "microfi/agent_id.h"
#include "microfi/flowfile.h"
#include "microfi/flow_engine.h"
#include "microfi/processor.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/types.h"

#include "brookesia/hal_interface/interfaces/audio/processor.hpp"
#include "brookesia/service_audio/service_audio.hpp"
#include "brookesia/service_helper/media/audio.hpp"
#include "brookesia/service_manager/service/manager.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mqtt_client.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace microfi {
namespace captureaudio {

namespace {

static const char* TAG = "microfi.proc.mic";

using EncoderService = esp_brookesia::service::AudioEncoder;
using EncoderHelper  = esp_brookesia::service::helper::AudioEncoder<0>;
using RawBuffer      = esp_brookesia::service::RawBuffer;

constexpr uint32_t kSampleRate     = 16000;  // board recorder (sdkconfig)
constexpr uint32_t kRecorderChans  = 2;      // "MR": mic + AEC reference
constexpr uint32_t kBytesPerSample = 2;      // 16-bit
constexpr uint32_t kMonoBytesPerSec = kSampleRate * kBytesPerSample;
constexpr uint32_t kMinClipSeconds = 1;
constexpr uint32_t kMaxClipSeconds = 10;     // 320 KB of PSRAM at the cap
constexpr uint32_t kCallTimeoutMs  = 2000;   // service RPC
constexpr size_t   kWavHeaderBytes = 44;

struct State {
    esp_mqtt_client_handle_t client;
    bool     mqtt_started;
    bool     connected;
    bool     subscribed;
    uint8_t  clip_seconds;
    uint32_t interval_ticks;   // source mode: record every N ticks (0 = never)
    uint32_t tick_count;
    uint32_t seq;
    char     broker_uri[48];
    char     topic[32];
    char     client_id[24];
    char     username[24];
    char     password[24];
};
static_assert(sizeof(State) <= 256, "State larger than engine slab");

// Clip buffer + recorder connection are file-scope (one mic, one
// subscriber), like GetTouch's ring: they outlive a node slab rebuild and
// are torn down explicitly in on_stop. The buffer itself is heap PSRAM.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t* s_buf       = nullptr;  // [WAV header][mono PCM]
static size_t   s_buf_cap   = 0;        // bytes allocated
static size_t   s_pcm_cap   = 0;        // PCM bytes wanted for this clip
static size_t   s_pcm_len   = 0;        // PCM bytes written so far
static bool     s_recording = false;    // slot may write
static bool     s_clip_ready = false;   // clip complete, engine to publish
static bool     s_data_seen = false;    // any recorder data since subscribe
static bool     s_started_by_us = false;
static int64_t  s_clip_start_us = 0;
static esp_brookesia::lib_utils::connection s_connection;
static esp_brookesia::service::ServiceBinding s_binding;   // keeps AudioEncoder0 running

static const PropertyDescriptor kProperties[] = {
    {
        /* name          */ "Broker URI",
        /* description   */ "The URI of the MQTT broker the WAV clips are published to.",
        /* default_value */ nullptr,
        /* required      */ true,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Audio Topic",
        /* description   */ "MQTT topic the WAV bytes (16 kHz, 16-bit, mono) are published to.",
        /* default_value */ nullptr,
        /* required      */ true,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Client ID",
        /* description   */ "MQTT client id. Blank derives <agent-id>-mic. Keep distinct "
                            "from any PublishMQTT client on the same device.",
        /* default_value */ nullptr,
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Username",
        /* description   */ "Username for an authenticated broker. Leave blank if none.",
        /* default_value */ nullptr,
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Password",
        /* description   */ "Password for an authenticated broker. Leave blank if none.",
        /* default_value */ nullptr,
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Clip Seconds",
        /* description   */ "Length of each recorded clip in seconds (1-10).",
        /* default_value */ "3",
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Capture Every N Ticks",
        /* description   */ "Record a clip every N engine ticks (1 tick = 1 s). 0 = only "
                            "when an incoming FlowFile triggers a capture.",
        /* default_value */ "0",
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
};
static constexpr size_t kPropertyCount =
    sizeof(kProperties) / sizeof(kProperties[0]);

// ---- recorder slot (GMF recorder task) -------------------------------------

void on_recorder_data(const RawBuffer& data) {
    s_data_seen = true;
    if (data.data_ptr == nullptr || data.data_size < kRecorderChans * kBytesPerSample) return;

    const size_t frames   = data.data_size / (kRecorderChans * kBytesPerSample);
    size_t       want     = frames * kBytesPerSample;   // mono bytes this slice yields
    size_t       offset   = 0;
    bool         complete = false;

    taskENTER_CRITICAL(&s_lock);
    if (!s_recording || s_buf == nullptr) {
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    const size_t room = s_pcm_cap - s_pcm_len;
    if (want > room) want = room;
    offset     = s_pcm_len;
    s_pcm_len += want;
    if (s_pcm_len >= s_pcm_cap) {
        s_recording  = false;
        complete     = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    // Copy outside the critical section: the engine leaves the buffer alone
    // while a clip is recording, and the slice [offset, offset+want) is ours.
    const int16_t* in  = reinterpret_cast<const int16_t*>(data.data_ptr);
    int16_t*       out = reinterpret_cast<int16_t*>(s_buf + kWavHeaderBytes + offset);
    const size_t   n   = want / kBytesPerSample;
    for (size_t i = 0; i < n; ++i) out[i] = in[i * kRecorderChans];   // channel 0 = mic

    if (complete) {
        taskENTER_CRITICAL(&s_lock);
        s_clip_ready = true;
        taskEXIT_CRITICAL(&s_lock);
    }
}

// ---- helpers (engine task) -------------------------------------------------

void write_wav_header(uint8_t* h, uint32_t pcm_bytes) {
    auto put32 = [](uint8_t* p, uint32_t v) {
        p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
    };
    auto put16 = [](uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; };
    std::memcpy(h + 0, "RIFF", 4);
    put32(h + 4, 36 + pcm_bytes);
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16);                       // PCM fmt chunk size
    put16(h + 20, 1);                        // PCM
    put16(h + 22, 1);                        // mono
    put32(h + 24, kSampleRate);
    put32(h + 28, kSampleRate * kBytesPerSample);   // byte rate
    put16(h + 32, kBytesPerSample);          // block align
    put16(h + 34, 16);                       // bits per sample
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, pcm_bytes);
}

bool ensure_buffer(size_t pcm_bytes) {
    const size_t need = kWavHeaderBytes + pcm_bytes;
    if (s_buf != nullptr && s_buf_cap >= need) return true;
    if (s_buf != nullptr) { heap_caps_free(s_buf); s_buf = nullptr; s_buf_cap = 0; }
    s_buf = static_cast<uint8_t*>(heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_buf == nullptr) {
        ESP_LOGE(TAG, "heap_caps_malloc(%u) in PSRAM failed", static_cast<unsigned>(need));
        return false;
    }
    s_buf_cap = need;
    return true;
}

void mqtt_event_handler(void* handler_args, esp_event_base_t /*base*/,
                        int32_t event_id, void* /*event_data*/) {
    auto* s = static_cast<State*>(handler_args);
    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected to broker %s", s->broker_uri);
            s->connected = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected from broker %s", s->broker_uri);
            s->connected = false;
            break;
        default:
            break;
    }
}

void start_client(State* s) {
    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri    = s->broker_uri;
    cfg.credentials.client_id = s->client_id;
    if (s->username[0] != '\0') cfg.credentials.username = s->username;
    if (s->password[0] != '\0') cfg.credentials.authentication.password = s->password;
    s->client = esp_mqtt_client_init(&cfg);
    if (s->client == nullptr) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed (broker=%s)", s->broker_uri);
        return;
    }
    esp_mqtt_client_register_event(s->client, MQTT_EVENT_ANY, &mqtt_event_handler, s);
    esp_mqtt_client_start(s->client);
    s->mqtt_started = true;
    ESP_LOGI(TAG, "mqtt client starting (broker=%s, topic=%s, id=%s)",
             s->broker_uri, s->topic, s->client_id);
}

bool ensure_subscribed(State* s) {
    if (s_connection.connected()) return true;
    EncoderService* enc = EncoderService::get_instance(0);
    if (enc == nullptr) {
        ESP_LOGW(TAG, "AudioEncoder0 service not available -- no mic source");
        return false;
    }
    taskENTER_CRITICAL(&s_lock);
    s_recording = false; s_clip_ready = false; s_pcm_len = 0;
    taskEXIT_CRITICAL(&s_lock);
    s_data_seen = false;
    s_connection = enc->connect_recorder_data(&on_recorder_data);
    if (!s_connection.connected()) {
        ESP_LOGW(TAG, "connect_recorder_data failed");
        return false;
    }
    ESP_LOGI(TAG, "subscribed to AudioEncoder0 recorder data");
    return true;
}

bool wait_for_data(uint32_t timeout_ms) {
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (!s_data_seen && esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(20));
    return s_data_seen;
}

// The recorder only runs while the encoder is started. No data shortly
// after subscribing means nobody else runs it -- start it ourselves.
bool ensure_encoder_running() {
    if (s_data_seen || s_started_by_us) return true;
    if (!EncoderHelper::is_available()) {
        ESP_LOGW(TAG, "AudioEncoder0 service not available -- no mic source");
        return false;
    }
    if (!EncoderHelper::is_running() && !s_binding.is_valid()) {
        s_binding = esp_brookesia::service::ServiceManager::get_instance().bind(
            std::string(EncoderHelper::get_name()));
        if (!s_binding.is_valid()) {
            ESP_LOGW(TAG, "bind(AudioEncoder0) failed -- service would not start");
            return false;
        }
        ESP_LOGI(TAG, "bound AudioEncoder0 service (it was stopped; started it)");
    }
    if (wait_for_data(300)) {
        ESP_LOGI(TAG, "AudioEncoder0 already running (someone else's) -- tapping it");
        return true;
    }
    if (!EncoderHelper::is_available() || !EncoderHelper::is_running()) {
        ESP_LOGW(TAG, "AudioEncoder0 service not running -- cannot start the mic");
        return false;
    }
    esp_brookesia::hal::audio::EncoderDynamicConfig cfg{};
    cfg.type                   = esp_brookesia::hal::audio::CodecFormat::PCM;
    cfg.general.channels       = 1;
    cfg.general.sample_bits    = 16;
    cfg.general.sample_rate    = kSampleRate;
    cfg.general.frame_duration = 20;
    cfg.fetch_interval_ms      = 20;
    cfg.fetch_data_size        = 640;
    cfg.enable_afe             = false;
    auto cfg_json = BROOKESIA_DESCRIBE_TO_JSON(cfg).as_object();
    auto r = EncoderHelper::call_function_sync(
        EncoderHelper::FunctionId::Start, cfg_json,
        esp_brookesia::service::helper::Timeout(kCallTimeoutMs));
    if (!r) {
        ESP_LOGW(TAG, "AudioEncoder0 Start failed: %s", r.error().c_str());
        return false;
    }
    s_started_by_us = true;
    ESP_LOGI(TAG, "started AudioEncoder0 (PCM 16 kHz mono, no AFE) -- nobody else had it running");
    if (!wait_for_data(1500)) {
        ESP_LOGW(TAG, "encoder started but no recorder data within 1.5 s");
        return false;
    }
    return true;
}

void begin_clip(State* s) {
    const size_t pcm_bytes = static_cast<size_t>(s->clip_seconds) * kMonoBytesPerSec;
    if (!ensure_buffer(pcm_bytes)) return;
    taskENTER_CRITICAL(&s_lock);
    s_pcm_cap    = pcm_bytes;
    s_pcm_len    = 0;
    s_clip_ready = false;
    s_recording  = true;
    taskEXIT_CRITICAL(&s_lock);
    s_clip_start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "recording %u s clip (%u PCM bytes)",
             static_cast<unsigned>(s->clip_seconds), static_cast<unsigned>(pcm_bytes));
}

Status publish_clip(Session& session, State* s) {
    const size_t pcm_bytes = s_pcm_len;
    const uint32_t ms = static_cast<uint32_t>((esp_timer_get_time() - s_clip_start_us) / 1000);
    write_wav_header(s_buf, static_cast<uint32_t>(pcm_bytes));
    const size_t total = kWavHeaderBytes + pcm_bytes;

    if (!s->connected) {
        ESP_LOGW(TAG, "broker not connected; clip %lu (%u bytes) dropped",
                 static_cast<unsigned long>(s->seq), static_cast<unsigned>(total));
        s_clip_ready = false;
        return Status::IoError;
    }
    const int msg_id = esp_mqtt_client_publish(
        s->client, s->topic, reinterpret_cast<const char*>(s_buf), static_cast<int>(total),
        /*qos=*/0, /*retain=*/0);
    s_clip_ready = false;
    if (msg_id < 0) {
        ESP_LOGE(TAG, "clip publish failed (topic=%s)", s->topic);
        return Status::IoError;
    }
    const uint32_t audio_ms = static_cast<uint32_t>(pcm_bytes * 1000 / kMonoBytesPerSec);
    ESP_LOGI(TAG, "clip %lu: %u bytes WAV, %u ms audio (%u ms wall) -> '%s' (msg_id=%d)",
             static_cast<unsigned long>(s->seq), static_cast<unsigned>(total),
             static_cast<unsigned>(audio_ms), static_cast<unsigned>(ms), s->topic, msg_id);

    char meta[192];
    const int meta_len = std::snprintf(
        meta, sizeof(meta),
        "{\"seq\":%lu,\"bytes\":%u,\"ms\":%u,\"rate\":%u,\"channels\":1,\"bits\":16,\"topic\":\"%s\"}",
        static_cast<unsigned long>(s->seq), static_cast<unsigned>(total),
        static_cast<unsigned>(audio_ms), static_cast<unsigned>(kSampleRate), s->topic);
    ++s->seq;

    FlowFile f;
    f.assign_id(FlowEngine::instance().next_id());
    Status rc = f.set_attribute("source", "CaptureAudio");
    if (rc != Status::Ok) return rc;
    rc = f.set_attribute("mime.type", "application/json");
    if (rc != Status::Ok) return rc;
    rc = f.set_content(reinterpret_cast<const uint8_t*>(meta),
                       (meta_len > 0) ? static_cast<size_t>(meta_len) : 0);
    if (rc != Status::Ok) return rc;
    return session.transfer(f, "success");
}

// ---- processor hooks --------------------------------------------------------

Status on_init(void* state) {
    auto* s = static_cast<State*>(state);
    std::memset(s, 0, sizeof(*s));
    s->clip_seconds   = 3;
    s->interval_ticks = 0;
    return Status::Ok;
}

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s = static_cast<State*>(state);
    for (size_t i = 0; i < count; ++i) {
        const NodeProperty& p = props[i];
        if (std::strcmp(p.key, "Broker URI") == 0 && p.value[0] != '\0') {
            std::strncpy(s->broker_uri, p.value, sizeof(s->broker_uri) - 1);
            s->broker_uri[sizeof(s->broker_uri) - 1] = '\0';
        } else if (std::strcmp(p.key, "Audio Topic") == 0 && p.value[0] != '\0') {
            std::strncpy(s->topic, p.value, sizeof(s->topic) - 1);
            s->topic[sizeof(s->topic) - 1] = '\0';
        } else if (std::strcmp(p.key, "Client ID") == 0 && p.value[0] != '\0') {
            std::strncpy(s->client_id, p.value, sizeof(s->client_id) - 1);
            s->client_id[sizeof(s->client_id) - 1] = '\0';
        } else if (std::strcmp(p.key, "Username") == 0 && p.value[0] != '\0') {
            std::strncpy(s->username, p.value, sizeof(s->username) - 1);
            s->username[sizeof(s->username) - 1] = '\0';
        } else if (std::strcmp(p.key, "Password") == 0 && p.value[0] != '\0') {
            std::strncpy(s->password, p.value, sizeof(s->password) - 1);
            s->password[sizeof(s->password) - 1] = '\0';
        } else if (std::strcmp(p.key, "Clip Seconds") == 0 && p.value[0] != '\0') {
            const int v = std::atoi(p.value);
            s->clip_seconds = static_cast<uint8_t>(
                (v < static_cast<int>(kMinClipSeconds)) ? kMinClipSeconds :
                (v > static_cast<int>(kMaxClipSeconds)) ? kMaxClipSeconds : v);
        } else if (std::strcmp(p.key, "Capture Every N Ticks") == 0 && p.value[0] != '\0') {
            const int v = std::atoi(p.value);
            s->interval_ticks = (v > 0) ? static_cast<uint32_t>(v) : 0;
        }
    }
    if (s->client_id[0] == '\0') {
        std::snprintf(s->client_id, sizeof(s->client_id), "%.19s-mic", agent_id());
    }
}

Status on_trigger(Session& session, void* state) {
    auto* s = static_cast<State*>(state);
    const FlowFile* in = session.input();
    if (s->broker_uri[0] == '\0' || s->topic[0] == '\0') return Status::InvalidArg;

    // Source mode (no incoming connection): the timer decides.
    if (in == nullptr) {
        if (s->interval_ticks == 0) return Status::Again;
        if ((s->tick_count++ % s->interval_ticks) != 0) return Status::Again;
    } else {
        ESP_LOGI(TAG, "FlowFile id=%llu: capture requested",
                 static_cast<unsigned long long>(in->id()));
    }

    if (!s->mqtt_started) start_client(s);
    for (int i = 0; i < 100 && !s->connected; ++i) vTaskDelay(pdMS_TO_TICKS(20));  // <= 2 s
    if (!s->connected) {
        ESP_LOGW(TAG, "broker not connected -- capture skipped");
        return Status::IoError;
    }
    if (!s->subscribed) {
        if (!ensure_subscribed(s)) return Status::IoError;
        s->subscribed = true;
    }
    if (!ensure_encoder_running()) return Status::IoError;

    begin_clip(s);
    if (s_buf == nullptr) return Status::OutOfMemory;
    const int64_t deadline = esp_timer_get_time() +
        (static_cast<int64_t>(s->clip_seconds) * 1000 + 2000) * 1000;
    bool ready = false;
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
        taskENTER_CRITICAL(&s_lock);
        ready = s_clip_ready;
        taskEXIT_CRITICAL(&s_lock);
        if (ready) break;
    }
    if (!ready) {
        size_t got;
        taskENTER_CRITICAL(&s_lock);
        s_recording = false; got = s_pcm_len; s_pcm_len = 0;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "clip timed out with %u of %u PCM bytes -- recorder data stopped?",
                 static_cast<unsigned>(got), static_cast<unsigned>(s_pcm_cap));
        s_data_seen = false;  // re-probe the encoder on the next trigger
        return Status::IoError;
    }
    return publish_clip(session, s);
}

// Engine task, on graph rebuild (#150 pattern): drop the signal, stop the
// encoder only if we were the ones who started it, keep the PSRAM buffer.
void on_stop(void* state) {
    auto* s = static_cast<State*>(state);
    taskENTER_CRITICAL(&s_lock);
    s_recording = false; s_clip_ready = false; s_pcm_len = 0;
    taskEXIT_CRITICAL(&s_lock);
    if (s_connection.connected()) {
        s_connection.disconnect();
        ESP_LOGI(TAG, "unsubscribed from AudioEncoder0 recorder data");
    }
    if (s_started_by_us) {
        auto r = EncoderHelper::call_function_sync(
            EncoderHelper::FunctionId::Stop,
            esp_brookesia::service::helper::Timeout(kCallTimeoutMs));
        if (!r) ESP_LOGW(TAG, "AudioEncoder0 Stop failed: %s", r.error().c_str());
        else    ESP_LOGI(TAG, "stopped AudioEncoder0 (we started it)");
        s_started_by_us = false;
    }
    s_data_seen = false;
    if (s_binding.is_valid()) {
        s_binding.release();
        ESP_LOGI(TAG, "released AudioEncoder0 service binding");
    }
    if (s->client != nullptr) {
        esp_mqtt_client_stop(s->client);
        esp_mqtt_client_destroy(s->client);
        s->client = nullptr;
        ESP_LOGI(TAG, "mqtt client stopped");
    }
    s->mqtt_started = s->connected = s->subscribed = false;
}

ProcessorDescriptor descriptor = {
    "CaptureAudio",
    "Records a short clip from the device microphone (16 kHz, 16-bit, mono "
    "WAV), publishes the bytes to an MQTT topic, and emits a metadata "
    "FlowFile (seq/bytes/ms/rate/topic as JSON). An incoming FlowFile "
    "triggers a clip; Capture Every N Ticks records on a timer.",
    &on_trigger,
    &on_init,
    &on_configure,
    sizeof(State),
    "INPUT_ALLOWED",   // trigger on input, or timer, or both
    kProperties,
    kPropertyCount,
    &on_stop,
};

}  // namespace
}  // namespace captureaudio
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::captureaudio::descriptor)

#endif  // MICROFI_BOARD_CAPTURE_AUDIO
