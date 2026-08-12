// processors/capture_image.cpp
//
// Camera ingress for the XIAO ESP32-S3 Sense (OV2640): captures a JPEG frame
// and publishes the image bytes to an MQTT topic, emitting a small metadata
// FlowFile (JSON: seq/bytes/dims/topic) into the flow chain.
//
// Why the image bytes bypass the FlowFile chain: FlowFile content is a fixed
// kInlineContentBytes (256 B) inline buffer copied by value through queues
// and the engine task stack -- a 5-30 KB JPEG categorically cannot ride it.
// The frame therefore goes broker-direct from this processor (the same "one
// thing the engine can't hold natively" carve-out as a persistent socket),
// while everything downstream-routable -- the capture event -- stays a normal
// FlowFile any sink can consume.
//
// Camera + MQTT client are created lazily on the first trigger and torn down
// in on_stop (#150), so a C2 republish never orphans the camera driver or
// leaves a stale MQTT session fighting the new one. NOTE: leave "Client ID"
// distinct from any PublishMQTT node on the same device -- esp-mqtt's default
// id is MAC-derived and identical for every client on one unit.

#include "microfi/agent_id.h"
#include "microfi/flowfile.h"
#include "microfi/flow_engine.h"
#include "microfi/processor.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/types.h"

#include "esp_camera.h"
#include "esp_log.h"
#include "mqtt_client.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace microfi {
namespace captureimage {

namespace {

static const char* TAG = "microfi.proc.camera";

// XIAO ESP32-S3 Sense OV2640 pin map (Seeed schematic).
constexpr int kPwdn = -1, kReset = -1, kXclk = 10, kSiod = 40, kSioc = 39;
constexpr int kY9 = 48, kY8 = 11, kY7 = 12, kY6 = 14, kY5 = 16, kY4 = 18,
              kY3 = 17, kY2 = 15, kVsync = 38, kHref = 47, kPclk = 13;

struct State {
    esp_mqtt_client_handle_t client;
    bool     camera_ok;
    bool     camera_failed;   // init failed once -- stop retrying/spamming
    bool     mqtt_started;
    bool     connected;
    uint8_t  jpeg_quality;
    uint8_t  frame_size;      // framesize_t
    uint32_t interval_ticks;
    uint32_t tick_count;
    uint32_t seq;
    char     broker_uri[48];
    char     image_topic[32];
    char     client_id[24];
};
static_assert(sizeof(State) <= 256, "State larger than engine slab");

static const AllowableValue kFrameSizes[] = {
    { "QVGA",  "QVGA 320x240"  },
    { "VGA",   "VGA 640x480"   },
    { "SVGA",  "SVGA 800x600"  },
    { "XGA",   "XGA 1024x768"  },
};

static const PropertyDescriptor kProperties[] = {
    {
        /* name          */ "Broker URI",
        /* description   */ "The URI of the MQTT broker the JPEG frames are published to.",
        /* default_value */ nullptr,
        /* required      */ true,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Image Topic",
        /* description   */ "MQTT topic the raw JPEG bytes are published to.",
        /* default_value */ nullptr,
        /* required      */ true,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Client ID",
        /* description   */ "MQTT client id. Blank derives <agent-id>-cam. Keep distinct "
                            "from any PublishMQTT client on the same device.",
        /* default_value */ nullptr,
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Frame Size",
        /* description   */ "Camera resolution for the JPEG capture.",
        /* default_value */ "QVGA",
        /* required      */ false,
        /* allowable     */ kFrameSizes, 4,
    },
    {
        /* name          */ "JPEG Quality",
        /* description   */ "JPEG quality 10-63; lower is better/larger.",
        /* default_value */ "12",
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Capture Every N Ticks",
        /* description   */ "Capture one frame every N engine ticks (1 tick = 1 s).",
        /* default_value */ "10",
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
};
static constexpr size_t kPropertyCount =
    sizeof(kProperties) / sizeof(kProperties[0]);

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s = static_cast<State*>(state);
    s->jpeg_quality   = 12;
    s->frame_size     = FRAMESIZE_QVGA;
    s->interval_ticks = 10;

    for (size_t i = 0; i < count; ++i) {
        const NodeProperty& p = props[i];
        if (std::strcmp(p.key, "Broker URI") == 0 && p.value[0] != '\0') {
            std::strncpy(s->broker_uri, p.value, sizeof(s->broker_uri) - 1);
            s->broker_uri[sizeof(s->broker_uri) - 1] = '\0';
        }
        else if (std::strcmp(p.key, "Image Topic") == 0 && p.value[0] != '\0') {
            std::strncpy(s->image_topic, p.value, sizeof(s->image_topic) - 1);
            s->image_topic[sizeof(s->image_topic) - 1] = '\0';
        }
        else if (std::strcmp(p.key, "Client ID") == 0 && p.value[0] != '\0') {
            std::strncpy(s->client_id, p.value, sizeof(s->client_id) - 1);
            s->client_id[sizeof(s->client_id) - 1] = '\0';
        }
        else if (std::strcmp(p.key, "Frame Size") == 0 && p.value[0] != '\0') {
            if      (std::strcmp(p.value, "QVGA") == 0) s->frame_size = FRAMESIZE_QVGA;
            else if (std::strcmp(p.value, "VGA")  == 0) s->frame_size = FRAMESIZE_VGA;
            else if (std::strcmp(p.value, "SVGA") == 0) s->frame_size = FRAMESIZE_SVGA;
            else if (std::strcmp(p.value, "XGA")  == 0) s->frame_size = FRAMESIZE_XGA;
        }
        else if (std::strcmp(p.key, "JPEG Quality") == 0 && p.value[0] != '\0') {
            const int v = atoi(p.value);
            s->jpeg_quality = (v >= 10 && v <= 63) ? static_cast<uint8_t>(v) : 12;
        }
        else if (std::strcmp(p.key, "Capture Every N Ticks") == 0 && p.value[0] != '\0') {
            const int v = atoi(p.value);
            s->interval_ticks = (v >= 1) ? static_cast<uint32_t>(v) : 10;
        }
    }

    if (s->client_id[0] == '\0') {
        std::snprintf(s->client_id, sizeof(s->client_id), "%.19s-cam", agent_id());
    }
}

bool init_camera(State* s) {
    camera_config_t cfg = {};
    cfg.pin_pwdn     = kPwdn;   cfg.pin_reset = kReset;
    cfg.pin_xclk     = kXclk;
    cfg.pin_sccb_sda = kSiod;   cfg.pin_sccb_scl = kSioc;
    cfg.pin_d7 = kY9; cfg.pin_d6 = kY8; cfg.pin_d5 = kY7; cfg.pin_d4 = kY6;
    cfg.pin_d3 = kY5; cfg.pin_d2 = kY4; cfg.pin_d1 = kY3; cfg.pin_d0 = kY2;
    cfg.pin_vsync = kVsync; cfg.pin_href = kHref; cfg.pin_pclk = kPclk;
    cfg.xclk_freq_hz = 20000000;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size   = static_cast<framesize_t>(s->frame_size);
    cfg.jpeg_quality = s->jpeg_quality;
    cfg.fb_count     = 1;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

    const esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s -- CaptureImage disabled "
                 "until the next flow apply", esp_err_to_name(err));
        s->camera_failed = true;
        return false;
    }
    s->camera_ok = true;
    ESP_LOGI(TAG, "camera up (frame_size=%u, quality=%u)",
             static_cast<unsigned>(s->frame_size),
             static_cast<unsigned>(s->jpeg_quality));
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
    cfg.broker.address.uri     = s->broker_uri;
    cfg.credentials.client_id  = s->client_id;

    s->client = esp_mqtt_client_init(&cfg);
    if (s->client == nullptr) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed (broker=%s)", s->broker_uri);
        return;
    }
    esp_mqtt_client_register_event(s->client, MQTT_EVENT_ANY, &mqtt_event_handler, s);
    esp_mqtt_client_start(s->client);
    s->mqtt_started = true;
    ESP_LOGI(TAG, "mqtt client starting (broker=%s, topic=%s, id=%s)",
             s->broker_uri, s->image_topic, s->client_id);
}

// Engine task, on graph rebuild (#150).
void on_stop(void* state) {
    auto* s = static_cast<State*>(state);
    if (s->client != nullptr) {
        esp_mqtt_client_stop(s->client);
        esp_mqtt_client_destroy(s->client);
        s->client = nullptr;
        ESP_LOGI(TAG, "mqtt client stopped");
    }
    if (s->camera_ok) {
        esp_camera_deinit();
        ESP_LOGI(TAG, "camera deinitialized");
    }
    s->camera_ok = s->camera_failed = s->mqtt_started = s->connected = false;
}

Status on_trigger(Session& session, void* state) {
    auto* s = static_cast<State*>(state);

    if (s->broker_uri[0] == '\0' || s->image_topic[0] == '\0') {
        return Status::InvalidArg;
    }
    if (s->camera_failed) return Status::Again;   // logged once at init
    if (!s->camera_ok && !init_camera(s)) return Status::Again;
    if (!s->mqtt_started) start_client(s);

    // Tick divider: capture every interval_ticks engine ticks.
    if ((s->tick_count++ % s->interval_ticks) != 0) return Status::Again;

    if (!s->connected) {
        ESP_LOGW(TAG, "broker not yet connected; skipping capture");
        return Status::Again;
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
        ESP_LOGE(TAG, "esp_camera_fb_get failed");
        return Status::IoError;
    }

    const int msg_id = esp_mqtt_client_publish(
        s->client, s->image_topic,
        reinterpret_cast<const char*>(fb->buf), static_cast<int>(fb->len),
        /*qos=*/0, /*retain=*/0);

    char meta[192];
    const int meta_len = std::snprintf(
        meta, sizeof(meta),
        "{\"seq\":%lu,\"bytes\":%u,\"width\":%u,\"height\":%u,\"topic\":\"%s\"}",
        static_cast<unsigned long>(s->seq),
        static_cast<unsigned>(fb->len),
        static_cast<unsigned>(fb->width),
        static_cast<unsigned>(fb->height),
        s->image_topic);

    ESP_LOGI(TAG, "frame %lu: %u bytes %ux%u -> '%s' (msg_id=%d)",
             static_cast<unsigned long>(s->seq),
             static_cast<unsigned>(fb->len),
             static_cast<unsigned>(fb->width),
             static_cast<unsigned>(fb->height),
             s->image_topic, msg_id);

    esp_camera_fb_return(fb);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "image publish failed (topic=%s)", s->image_topic);
        return Status::IoError;
    }
    ++s->seq;

    FlowFile f;
    f.assign_id(FlowEngine::instance().next_id());
    Status rc = f.set_attribute("source", "CaptureImage");
    if (rc != Status::Ok) return rc;
    rc = f.set_attribute("mime.type", "application/json");
    if (rc != Status::Ok) return rc;
    rc = f.set_content(reinterpret_cast<const uint8_t*>(meta),
                       (meta_len > 0) ? static_cast<size_t>(meta_len) : 0);
    if (rc != Status::Ok) return rc;

    return session.transfer(f, "success");
}

ProcessorDescriptor descriptor = {
    "CaptureImage",
    "Captures a JPEG frame from the onboard OV2640 camera, publishes the "
    "image bytes to an MQTT topic, and emits a metadata FlowFile "
    "(seq/bytes/dimensions/topic as JSON) on success.",
    &on_trigger,
    nullptr,          // no on_init -- on_configure sets defaults
    &on_configure,
    sizeof(State),
    "INPUT_FORBIDDEN", // source: no incoming connections
    kProperties,
    kPropertyCount,
    &on_stop,
};

}  // namespace
}  // namespace captureimage
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::captureimage::descriptor)
