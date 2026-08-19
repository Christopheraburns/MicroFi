// processors/publish_sparkplug.cpp
//
// Sparkplug B egress: publishes NBIRTH/NDATA to the broker with the full
// session semantics plain MQTT doesn't give you -- bdSeq, per-message seq,
// rebirth/scan-rate node tags, and the spBv1.0/<group>/<type>/<node> topic
// namespace. This is the unification of the previously separate Arduino
// Sparkplug sketch (xiao-telemetry-sparkplug, issue #126) into the MicroFi
// image: the same field-proven EmbeddedSparkplugNode/nanopb path, now vendored
// under vendor/sparkplug/ and driven as an EFM-pushed flow node.
//
// Shape mirrors MicroFi-1's telemetry leg: GenerateFlowFile (cadence) ->
// PublishSparkplug (this). Each incoming FlowFile is one tick of the node's
// state machine; the library rate-limits with its own Scan Rate, so a faster
// upstream cadence just means SCAN_NOT_DUE ticks in between. NBIRTH goes out
// on the first due scan after the broker connects; NDATA follows on scans
// whose tag values changed (report-by-exception, per the Sparkplug spec).
//
// The metric is either the S3's internal temperature sensor (default -- the
// proven sketch's shape, and its natural jitter keeps NDATA flowing) or the
// FlowFile content parsed as a float.
//
// Timestamps must be real epoch millis for the birth/data payloads, and this
// firmware has no other NTP consumer -- so the processor owns SNTP: started
// lazily once, and until the clock is sane every tick returns Again rather
// than emit a 1970 NBIRTH.
//
// Client lifecycle matches PublishMQTT: lazy esp-mqtt client on first trigger,
// connect tracked by event handler, and on_stop (#150) tears down the client
// AND the Sparkplug node + tag so a C2 republish can't leak the old session
// or double-register the metric tag.

#include "microfi/flowfile.h"
#include "microfi/processor.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/types.h"

#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

#include "SparkplugNode.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

namespace microfi {
namespace pubsparkplug {

namespace {

static const char* TAG = "microfi.proc.spb";

// NBIRTH carries the node's own bdSeq/rebirth/scan-rate tags plus our metric;
// 512 bytes is the sketch-proven size (256 truncates an NBIRTH).
constexpr size_t kPayloadBufferSize = 512;

// time() below this is a pre-SNTP clock; don't stamp Sparkplug payloads with it.
constexpr time_t kSaneEpochFloor = 1600000000;  // 2020-09-13

struct State {
    esp_mqtt_client_handle_t    client;
    SparkplugNodeConfig*        node;
    FunctionalBasicTag*         metric_tag;
    temperature_sensor_handle_t temp_sensor;
    float   metric_value;
    int32_t scan_rate_ms;
    bool    started;
    bool    connected;
    bool    was_connected;
    bool    from_content;
    bool    ntp_wait_logged;
    char    broker_uri[48];
    char    group_id[24];
    char    node_id[24];
    char    client_id[24];
    char    metric_name[32];
};
static_assert(sizeof(State) <= 256, "State larger than engine slab");

// SNTP is global to the firmware; guard so a graph rebuild never double-inits.
static bool s_sntp_started = false;

uint64_t timestamp_ms() {
    return static_cast<uint64_t>(time(nullptr)) * 1000ULL;
}

void ensure_sntp() {
    if (s_sntp_started) return;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
}

// ---- Property declarations ----------------------------------------------

static const AllowableValue kMetricSourceValues[] = {
    { "Internal Temperature", nullptr },
    { "FlowFile Content", nullptr },
};

static const PropertyDescriptor kProperties[] = {
    {
        /* name          */ "Broker URI",
        /* description   */ "The URI of the MQTT broker, e.g. mqtt://host:1883.",
        /* default_value */ nullptr,
        /* required      */ true,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Group ID",
        /* description   */ "Sparkplug group id -- the <group> segment of spBv1.0/<group>/... topics.",
        /* default_value */ "MicroFi",
        /* required      */ true,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Edge Node ID",
        /* description   */ "Sparkplug edge node id. Blank derives it from the agent class.",
        /* default_value */ nullptr,
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Client ID",
        /* description   */ "MQTT client identifier. If blank, esp-mqtt derives one. Must be distinct per MQTT-owning processor on one device.",
        /* default_value */ nullptr,
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Metric Name",
        /* description   */ "Name of the single float metric declared in NBIRTH and reported in NDATA.",
        /* default_value */ "Sensors/Temperature",
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Metric Source",
        /* description   */ "Where the metric value comes from: the S3's internal temperature sensor, or the FlowFile content parsed as a float.",
        /* default_value */ "Internal Temperature",
        /* required      */ false,
        /* allowable     */ kMetricSourceValues, 2,
    },
    {
        /* name          */ "Scan Rate",
        /* description   */ "Sparkplug scan rate in milliseconds -- the node samples tags and considers an NDATA at most this often.",
        /* default_value */ "5000",
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
};
static constexpr size_t kPropertyCount =
    sizeof(kProperties) / sizeof(kProperties[0]);

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s = static_cast<State*>(state);
    s->scan_rate_ms = 5000;
    std::strncpy(s->group_id, "MicroFi", sizeof(s->group_id) - 1);
    std::strncpy(s->node_id, CONFIG_MICROFI_AGENT_CLASS, sizeof(s->node_id) - 1);
    std::strncpy(s->metric_name, "Sensors/Temperature", sizeof(s->metric_name) - 1);

    for (size_t i = 0; i < count; ++i) {
        const NodeProperty& p = props[i];

        if (std::strcmp(p.key, "Broker URI") == 0 && p.value[0] != '\0') {
            std::strncpy(s->broker_uri, p.value, sizeof(s->broker_uri) - 1);
            s->broker_uri[sizeof(s->broker_uri) - 1] = '\0';
        }
        else if (std::strcmp(p.key, "Group ID") == 0 && p.value[0] != '\0') {
            std::strncpy(s->group_id, p.value, sizeof(s->group_id) - 1);
            s->group_id[sizeof(s->group_id) - 1] = '\0';
        }
        else if (std::strcmp(p.key, "Edge Node ID") == 0 && p.value[0] != '\0') {
            std::strncpy(s->node_id, p.value, sizeof(s->node_id) - 1);
            s->node_id[sizeof(s->node_id) - 1] = '\0';
        }
        else if (std::strcmp(p.key, "Client ID") == 0 && p.value[0] != '\0') {
            std::strncpy(s->client_id, p.value, sizeof(s->client_id) - 1);
            s->client_id[sizeof(s->client_id) - 1] = '\0';
        }
        else if (std::strcmp(p.key, "Metric Name") == 0 && p.value[0] != '\0') {
            std::strncpy(s->metric_name, p.value, sizeof(s->metric_name) - 1);
            s->metric_name[sizeof(s->metric_name) - 1] = '\0';
        }
        else if (std::strcmp(p.key, "Metric Source") == 0 && p.value[0] != '\0') {
            s->from_content = (std::strcmp(p.value, "FlowFile Content") == 0);
        }
        else if (std::strcmp(p.key, "Scan Rate") == 0 && p.value[0] != '\0') {
            const int v = atoi(p.value);
            if (v > 0) s->scan_rate_ms = v;
        }
    }
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
    cfg.broker.address.uri = s->broker_uri;
    if (s->client_id[0] != '\0') cfg.credentials.client_id = s->client_id;

    s->client = esp_mqtt_client_init(&cfg);
    if (s->client == nullptr) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed (broker=%s)", s->broker_uri);
        return;
    }
    esp_mqtt_client_register_event(s->client, MQTT_EVENT_ANY, &mqtt_event_handler, s);
    esp_mqtt_client_start(s->client);
    s->started = true;
    ESP_LOGI(TAG, "client starting (broker=%s, node=%s/%s)",
             s->broker_uri, s->group_id, s->node_id);
}

bool ensure_node(State* s) {
    if (s->node != nullptr) return true;

    s->node = createSparkplugNode(s->group_id, s->node_id,
                                  kPayloadBufferSize, &timestamp_ms);
    if (s->node == nullptr) {
        ESP_LOGE(TAG, "createSparkplugNode failed (%s/%s)", s->group_id, s->node_id);
        return false;
    }
    *(s->node->vars.scan_rate_tag_value) = s->scan_rate_ms;

    s->metric_tag = createFloatTag(s->metric_name, &s->metric_value,
                                   getNextAlias(), false, false);
    if (s->metric_tag == nullptr) {
        ESP_LOGE(TAG, "createFloatTag failed (%s)", s->metric_name);
        deleteSparkplugNode(s->node);
        s->node = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "node ready: NBIRTH=%s NDATA=%s scan=%ldms",
             s->node->topics.NBIRTH, s->node->topics.NDATA,
             static_cast<long>(s->scan_rate_ms));
    return true;
}

void update_metric(State* s, const FlowFile* in) {
    if (s->from_content) {
        char buf[32] = {0};
        const size_t n = in->content_size() < sizeof(buf) - 1
                             ? in->content_size() : sizeof(buf) - 1;
        std::memcpy(buf, in->content(), n);
        char* end = nullptr;
        const float v = strtof(buf, &end);
        if (end != buf) s->metric_value = v;
        return;
    }

    if (s->temp_sensor == nullptr) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s->temp_sensor) != ESP_OK ||
            temperature_sensor_enable(s->temp_sensor) != ESP_OK) {
            ESP_LOGE(TAG, "internal temperature sensor unavailable");
            s->temp_sensor = nullptr;
            return;
        }
    }
    float celsius = 0.0f;
    if (temperature_sensor_get_celsius(s->temp_sensor, &celsius) == ESP_OK) {
        s->metric_value = celsius;
    }
}

void on_stop(void* state) {
    auto* s = static_cast<State*>(state);
    if (s->client != nullptr) {
        esp_mqtt_client_stop(s->client);
        esp_mqtt_client_destroy(s->client);
        s->client = nullptr;
        ESP_LOGI(TAG, "client for %s stopped", s->broker_uri);
    }
    if (s->metric_tag != nullptr) {
        deleteTag(s->metric_tag);
        s->metric_tag = nullptr;
    }
    if (s->node != nullptr) {
        deleteSparkplugNode(s->node);
        s->node = nullptr;
    }
    if (s->temp_sensor != nullptr) {
        temperature_sensor_disable(s->temp_sensor);
        temperature_sensor_uninstall(s->temp_sensor);
        s->temp_sensor = nullptr;
    }
    s->started       = false;
    s->connected     = false;
    s->was_connected = false;
}

Status on_trigger(Session& session, void* state) {
    auto* s = static_cast<State*>(state);
    const FlowFile* in = session.input();
    if (in == nullptr) return Status::Again;

    if (s->broker_uri[0] == '\0') {
        ESP_LOGE(TAG, "not configured (missing Broker URI); dropping FlowFile id=%llu",
                 static_cast<unsigned long long>(in->id()));
        return Status::InvalidArg;
    }

    ensure_sntp();
    if (!ensure_node(s)) return Status::IoError;
    if (!s->started) start_client(s);

    // Connect/disconnect edges belong to the engine task, not the esp-mqtt
    // event task -- the library is not thread-safe against tick().
    const bool now_connected = s->connected;
    if (now_connected && !s->was_connected) {
        spnOnMQTTConnected(s->node);
    } else if (!now_connected && s->was_connected) {
        spnOnMQTTDisconnected(s->node);
    }
    s->was_connected = now_connected;

    if (!now_connected) {
        ESP_LOGW(TAG, "broker not yet connected; dropping FlowFile id=%llu",
                 static_cast<unsigned long long>(in->id()));
        return Status::Again;
    }

    if (time(nullptr) < kSaneEpochFloor) {
        if (!s->ntp_wait_logged) {
            ESP_LOGW(TAG, "waiting for SNTP before first Sparkplug payload");
            s->ntp_wait_logged = true;
        }
        return Status::Again;
    }

    update_metric(s, in);

    const SparkplugNodeState st = tickSparkplugNode(s->node);
    switch (st) {
        case spn_NBIRTH_PL_READY:
            if (esp_mqtt_client_publish(s->client, s->node->mqtt_message.topic,
                    reinterpret_cast<const char*>(s->node->mqtt_message.payload->buffer),
                    static_cast<int>(s->node->mqtt_message.payload->written_length),
                    /*qos=*/0, /*retain=*/0) >= 0) {
                ESP_LOGI(TAG, "published NBIRTH (%u bytes) -> %s",
                         static_cast<unsigned>(s->node->mqtt_message.payload->written_length),
                         s->node->mqtt_message.topic);
                spnOnPublishNBIRTH(s->node);
            } else {
                ESP_LOGE(TAG, "NBIRTH publish failed");
                return Status::IoError;
            }
            break;
        case spn_NDATA_PL_READY:
            if (esp_mqtt_client_publish(s->client, s->node->mqtt_message.topic,
                    reinterpret_cast<const char*>(s->node->mqtt_message.payload->buffer),
                    static_cast<int>(s->node->mqtt_message.payload->written_length),
                    /*qos=*/0, /*retain=*/0) >= 0) {
                ESP_LOGI(TAG, "published NDATA (%u bytes, %s=%.2f) -> %s",
                         static_cast<unsigned>(s->node->mqtt_message.payload->written_length),
                         s->metric_name, static_cast<double>(s->metric_value),
                         s->node->mqtt_message.topic);
                spnOnPublishNDATA(s->node);
            } else {
                ESP_LOGE(TAG, "NDATA publish failed");
                return Status::IoError;
            }
            break;
        case spn_MAKE_NBIRTH_FAILED:
            ESP_LOGE(TAG, "error making NBIRTH payload");
            return Status::IoError;
        case spn_MAKE_NDATA_FAILED:
            ESP_LOGE(TAG, "error making NDATA payload");
            return Status::IoError;
        case spn_SCAN_NOT_DUE:
        case spn_VALUES_UNCHANGED:
        default:
            break;
    }

    session.transfer(*in, "success");
    return Status::Ok;
}

ProcessorDescriptor descriptor = {
    "PublishSparkplug",
    "Publishes Sparkplug B NBIRTH/NDATA for a single float metric to an MQTT broker.",
    &on_trigger,
    nullptr,          // no on_init -- on_configure sets defaults
    &on_configure,
    sizeof(State),
    "INPUT_REQUIRED", // sink: an upstream tick source drives the scan cadence
    kProperties,
    kPropertyCount,
    &on_stop,
};

}  // namespace
}  // namespace pubsparkplug
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::pubsparkplug::descriptor)
