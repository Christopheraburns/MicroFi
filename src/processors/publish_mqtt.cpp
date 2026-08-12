// processors/publish_mqtt.cpp
//
// The egress gap closer: publishes the input FlowFile's content to an MQTT
// broker topic. This is what turns a MicroFi XIAO from a loopback demo into
// a real publisher -- XIAO -> Mosquitto -> ConsumeMQTT -> Kafka.
//
// Minimal ESP32 subset of MiNiFi C++'s PublishMQTT (see efm-xiao-microfi.md
// "Processor design specs" -> "1. PublishMQTT"): Broker URI, Client ID,
// Topic, Quality of Service, Username, Password. TLS props are deferred --
// the SparkPlug PG's Mosquitto is plaintext on the LAN.
//
// Client lifecycle: the esp-mqtt client is created lazily on the first
// on_trigger call (once Broker URI / Topic are known from on_configure) and
// connects asynchronously in the background. A FlowFile that arrives before
// the CONNECTED event lands is dropped -- acceptable for a periodic ingress
// source, which just re-publishes on the next tick. Note the client handle
// is not torn down on a flow re-push (the per-node state slab is zeroed by
// the engine's rebuild, silently orphaning any previously-started client);
// fine for a single-flow verification pass, worth revisiting before this is
// treated as production-grade.

#include "microfi/flowfile.h"
#include "microfi/processor.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/types.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include <cstdlib>
#include <cstring>

namespace microfi {
namespace pubmqtt {

namespace {

static const char* TAG = "microfi.proc.mqtt";

struct State {
    esp_mqtt_client_handle_t client;
    bool    started;
    bool    connected;
    uint8_t qos;
    char    broker_uri[48];
    char    client_id[24];
    char    topic[32];
    char    username[24];
    char    password[24];
};
static_assert(sizeof(State) <= 256, "State larger than engine slab");

// ---- Property declarations (MiNiFi C++ compatible names) ----------------

static const AllowableValue kQosValues[] = {
    { "0", nullptr },
    { "1", nullptr },
    { "2", nullptr },
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
        /* name          */ "Client ID",
        /* description   */ "MQTT client identifier. If blank, esp-mqtt derives one.",
        /* default_value */ nullptr,
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Topic",
        /* description   */ "The topic to publish the FlowFile content to.",
        /* default_value */ nullptr,
        /* required      */ true,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Quality of Service",
        /* description   */ "MQTT QoS level (0, 1, or 2) for the publish.",
        /* default_value */ "0",
        /* required      */ false,
        /* allowable     */ kQosValues, 3,
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
};
static constexpr size_t kPropertyCount =
    sizeof(kProperties) / sizeof(kProperties[0]);

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s = static_cast<State*>(state);
    s->qos = 0;

    for (size_t i = 0; i < count; ++i) {
        const NodeProperty& p = props[i];

        if (std::strcmp(p.key, "Broker URI") == 0 && p.value[0] != '\0') {
            std::strncpy(s->broker_uri, p.value, sizeof(s->broker_uri) - 1);
            s->broker_uri[sizeof(s->broker_uri) - 1] = '\0';
        }
        else if (std::strcmp(p.key, "Client ID") == 0 && p.value[0] != '\0') {
            std::strncpy(s->client_id, p.value, sizeof(s->client_id) - 1);
            s->client_id[sizeof(s->client_id) - 1] = '\0';
        }
        else if (std::strcmp(p.key, "Topic") == 0 && p.value[0] != '\0') {
            std::strncpy(s->topic, p.value, sizeof(s->topic) - 1);
            s->topic[sizeof(s->topic) - 1] = '\0';
        }
        else if (std::strcmp(p.key, "Quality of Service") == 0 && p.value[0] != '\0') {
            const int v = atoi(p.value);
            s->qos = (v >= 0 && v <= 2) ? static_cast<uint8_t>(v) : 0;
        }
        else if (std::strcmp(p.key, "Username") == 0 && p.value[0] != '\0') {
            std::strncpy(s->username, p.value, sizeof(s->username) - 1);
            s->username[sizeof(s->username) - 1] = '\0';
        }
        else if (std::strcmp(p.key, "Password") == 0 && p.value[0] != '\0') {
            std::strncpy(s->password, p.value, sizeof(s->password) - 1);
            s->password[sizeof(s->password) - 1] = '\0';
        }
    }
}

// esp-mqtt event handler -- tracks connect/disconnect so on_trigger knows
// whether a publish attempt is worth making.
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
    if (s->username[0] != '\0')  cfg.credentials.username  = s->username;
    if (s->password[0] != '\0') cfg.credentials.authentication.password = s->password;

    s->client = esp_mqtt_client_init(&cfg);
    if (s->client == nullptr) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed (broker=%s)", s->broker_uri);
        return;
    }
    esp_mqtt_client_register_event(s->client, MQTT_EVENT_ANY, &mqtt_event_handler, s);
    esp_mqtt_client_start(s->client);
    s->started = true;
    ESP_LOGI(TAG, "client starting (broker=%s, topic=%s, qos=%u)",
             s->broker_uri, s->topic, s->qos);
}

Status on_trigger(Session& session, void* state) {
    auto* s = static_cast<State*>(state);
    const FlowFile* in = session.input();
    if (in == nullptr) return Status::Again;

    if (s->broker_uri[0] == '\0' || s->topic[0] == '\0') {
        ESP_LOGE(TAG, "not configured (missing Broker URI or Topic); dropping FlowFile id=%llu",
                 static_cast<unsigned long long>(in->id()));
        return Status::InvalidArg;
    }

    if (!s->started) start_client(s);

    if (!s->connected) {
        ESP_LOGW(TAG, "broker not yet connected; dropping FlowFile id=%llu",
                 static_cast<unsigned long long>(in->id()));
        return Status::Again;
    }

    const int msg_id = esp_mqtt_client_publish(
        s->client, s->topic,
        reinterpret_cast<const char*>(in->content()),
        static_cast<int>(in->content_size()),
        s->qos, /*retain=*/0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "publish failed (topic=%s)", s->topic);
        return Status::IoError;
    }

    ESP_LOGI(TAG, "published %u bytes to '%s' (qos=%u, msg_id=%d)",
             static_cast<unsigned>(in->content_size()), s->topic, s->qos, msg_id);

    session.transfer(*in, "success");
    return Status::Ok;
}

ProcessorDescriptor descriptor = {
    "PublishMQTT",
    "Publishes the input FlowFile's content to an MQTT broker topic.",
    &on_trigger,
    nullptr,          // no on_init -- on_configure sets defaults, matching LogAttribute
    &on_configure,
    sizeof(State),
    "INPUT_REQUIRED", // sink: must have an incoming connection
    kProperties,
    kPropertyCount,
};

}  // namespace
}  // namespace pubmqtt
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::pubmqtt::descriptor)
