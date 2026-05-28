// c2_client.cpp -- EFM 2.x compatible heartbeat client.
//
// Heartbeat loop:
//   POST /efm/api/c2-protocol/heartbeat  with HEARTBEAT envelope
//   Parse requestedOperations from response
//   Handle DESCRIBE/manifest  -> re-include manifest on next heartbeat
//   Handle UPDATE/configuration -> fetch flow def, parse, apply to engine
//
// Flow-update path (Slice B):
//   1. EFM heartbeat response contains UPDATE/configuration op
//   2. Extract configContent URL from args.configuration or args.url;
//      if absent, construct from heartbeat base URL + op identifier.
//   3. GET the configContent URL -> raw JSON stored in s_flow_buf
//   4. flow_parse() -> FlowDef
//   5. FlowEngine::instance().apply(def) -> queued for next engine tick
//   6. Subsequent heartbeats advertise the new flowInfo.flowId so EFM
//      can confirm the update landed.
//
// Note: No explicit ACKNOWLEDGE POST is sent.  EFM 2.x considers the
// operation acknowledged when the next heartbeat's flowInfo.flowId matches
// the flow UUID it pushed.

#include "microfi/c2_client.h"

#include "microfi/agent_id.h"
#include "microfi/flow_def.h"
#include "microfi/flow_engine.h"
#include "microfi/flow_parser.h"
#include "microfi/manifest.h"
#include "microfi/registry.h"
#include "microfi/wifi.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>

namespace microfi {

namespace {

const char* TAG = "microfi.c2";

constexpr size_t kResponseBufBytes = 4096;
constexpr size_t kFlowBufBytes     = 16384;  // flow defs can be 5-10 KB

// State -- single C2 task, no mutex needed for these.
char     s_resp_buf[kResponseBufBytes];
size_t   s_resp_len           = 0;
uint64_t s_heartbeat_counter  = 0;
bool     s_need_send_manifest = true;   // first heartbeat carries full manifest
bool     s_dump_next_heartbeat = true;  // dump first heartbeat + one after each flow apply

// Buffer for the raw flow-definition JSON fetched from EFM.
char   s_flow_buf[kFlowBufBytes];
size_t s_flow_len = 0;

// ---- HTTP event handlers ---------------------------------------------------

esp_err_t http_event(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        const size_t remaining = sizeof(s_resp_buf) - 1 - s_resp_len;
        const size_t take = static_cast<size_t>(evt->data_len) < remaining
                              ? static_cast<size_t>(evt->data_len) : remaining;
        if (take > 0) {
            std::memcpy(s_resp_buf + s_resp_len, evt->data, take);
            s_resp_len += take;
            s_resp_buf[s_resp_len] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t flow_http_event(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        const size_t remaining = sizeof(s_flow_buf) - 1 - s_flow_len;
        const size_t take = static_cast<size_t>(evt->data_len) < remaining
                              ? static_cast<size_t>(evt->data_len) : remaining;
        if (take > 0) {
            std::memcpy(s_flow_buf + s_flow_len, evt->data, take);
            s_flow_len += take;
            s_flow_buf[s_flow_len] = '\0';
        }
    }
    return ESP_OK;
}

// ---- Helpers ---------------------------------------------------------------

void read_ip_address(char* out, size_t out_size) {
    out[0] = '\0';
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) return;
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return;
    std::snprintf(out, out_size, IPSTR, IP2STR(&info.ip));
}

void build_heartbeat_id(char* out, size_t out_size) {
    std::snprintf(out, out_size, "%s-%llu-%lld",
                  agent_id(),
                  static_cast<unsigned long long>(s_heartbeat_counter),
                  static_cast<long long>(esp_timer_get_time() / 1000));
}

// Build the configContent URL by replacing the last path segment of the
// heartbeat URL with "configContent/<op_id>".
//
//   heartbeat URL: "http://192.168.243.226:10090/efm/api/c2-protocol/heartbeat"
//   result:        "http://192.168.243.226:10090/efm/api/c2-protocol/configContent/<op_id>"
void build_config_content_url(char* out, size_t out_size, const char* op_id) {
    const char* base      = CONFIG_MICROFI_C2_HEARTBEAT_URL;
    const char* last_slash = std::strrchr(base, '/');
    if (last_slash == nullptr) {
        // Fallback: just append
        std::snprintf(out, out_size, "%s/configContent/%s", base, op_id);
    } else {
        // prefix_len includes the trailing slash
        const int prefix_len = (int)(last_slash - base + 1);
        std::snprintf(out, out_size, "%.*sconfigContent/%s",
                      prefix_len, base, op_id);
    }
}

// ---- JSON builders ---------------------------------------------------------

cJSON* build_agent_info(bool include_manifest) {
    auto&    engine    = FlowEngine::instance();
    uint32_t free_heap = esp_get_free_heap_size();
    int64_t  uptime_ms = esp_timer_get_time() / 1000;

    cJSON* ai = cJSON_CreateObject();
    cJSON_AddStringToObject(ai, "identifier",        agent_id());
    cJSON_AddStringToObject(ai, "agentClass",        CONFIG_MICROFI_AGENT_CLASS);
    cJSON_AddStringToObject(ai, "agentManifestHash", manifest_hash());
    cJSON_AddNumberToObject(ai, "heartbeatPeriod",
                            static_cast<double>(CONFIG_MICROFI_HEARTBEAT_INTERVAL_MS));

    if (include_manifest) {
        cJSON* parsed = cJSON_Parse(manifest_json());
        if (parsed != nullptr) {
            cJSON_AddItemToObject(ai, "agentManifest", parsed);
        } else {
            ESP_LOGW(TAG, "manifest re-parse failed; sending hash only");
        }
    }

    cJSON* status = cJSON_CreateObject();
    cJSON_AddNumberToObject(status, "uptime", static_cast<double>(uptime_ms));

    // components: running/stopped badge per processor plus top-level FlowController.
    // MiNiFi C++ always includes "FlowController" as a component representing the
    // flow engine itself -- EFM uses its presence to enable the Monitor Activity view.
    //
    // Each entry MUST include a `uuid` field; EFM correlates these UUIDs against
    // canvas processors and against processorStatuses[].groupId.  Omitting `uuid`
    // (the previous behaviour) silently disables the live-counter overlay.
    cJSON* components = cJSON_CreateObject();
    {
        cJSON* fc = cJSON_CreateObject();
        cJSON_AddBoolToObject  (fc, "running", 1);
        cJSON_AddStringToObject(fc, "uuid",    process_group_id());
        cJSON_AddItemToObject(components, "FlowController", fc);
    }
    for (size_t i = 0; i < engine.node_count(); ++i) {
        const FlowEngine::Node& n = engine.node(i);
        if (n.desc == nullptr) continue;
        cJSON* comp = cJSON_CreateObject();
        cJSON_AddBoolToObject  (comp, "running", n.active ? 1 : 0);
        cJSON_AddStringToObject(comp, "uuid",    n.id);
        cJSON_AddItemToObject(components, n.desc->name, comp);
    }
    cJSON_AddItemToObject(status, "components", components);

    // repositories: send zero values so EFM doesn't treat them as null/offline.
    cJSON* repos = cJSON_CreateObject();
    {
        cJSON* ff_repo = cJSON_CreateObject();
        cJSON_AddNumberToObject(ff_repo, "size",        0);
        cJSON_AddNullToObject  (ff_repo, "sizeMax");
        cJSON_AddNullToObject  (ff_repo, "dataSize");
        cJSON_AddNullToObject  (ff_repo, "dataSizeMax");
        cJSON_AddItemToObject(repos, "flowFile", ff_repo);
    }
    cJSON_AddItemToObject(status, "repositories", repos);

    cJSON* rc = cJSON_CreateObject();
    cJSON_AddNumberToObject(rc, "memoryUsage",    static_cast<double>(free_heap));
    cJSON_AddNumberToObject(rc, "cpuUtilization", 0.0);
    cJSON_AddItemToObject  (status, "resourceConsumption", rc);

    // Custom microfi metrics -- EFM tolerates unknown fields.
    cJSON* mf = cJSON_CreateObject();
    cJSON_AddNumberToObject(mf, "queueDepth",
                            static_cast<double>(engine.queue_depth()));
    cJSON_AddNumberToObject(mf, "produced",
                            static_cast<double>(engine.flowfiles_produced()));
    cJSON_AddNumberToObject(mf, "consumed",
                            static_cast<double>(engine.flowfiles_consumed()));
    cJSON_AddItemToObject(status, "microfi", mf);

    cJSON_AddItemToObject(ai, "status", status);
    return ai;
}

cJSON* build_device_info() {
    char ip[16] = {0};
    read_ip_address(ip, sizeof(ip));

    cJSON* di = cJSON_CreateObject();
    cJSON_AddStringToObject(di, "identifier", device_id());

    cJSON* sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "machineArch",     "esp32");
    cJSON_AddStringToObject(sys, "operatingSystem", "FreeRTOS / ESP-IDF");
    cJSON_AddNumberToObject(sys, "physicalMem",
                            static_cast<double>(esp_get_free_heap_size()));
    cJSON_AddNumberToObject(sys, "vCores", 2);
    cJSON_AddItemToObject  (di, "systemInfo", sys);

    cJSON* net = cJSON_CreateObject();
    cJSON_AddStringToObject(net, "deviceId",  device_id());
    cJSON_AddStringToObject(net, "hostname",  agent_id());
    cJSON_AddStringToObject(net, "ipAddress", ip);
    cJSON_AddItemToObject  (di, "networkInfo", net);
    return di;
}

cJSON* build_flow_info() {
    auto& engine = FlowEngine::instance();

    cJSON* fi = cJSON_CreateObject();
    cJSON_AddStringToObject(fi, "flowId",    engine.flow_id());
    cJSON_AddStringToObject(fi, "runStatus", "RUNNING");

    // ---- versionedFlowSnapshotURI: required for EFM Monitor Activity ---------
    // EFM Flow Designer uses this to confirm the agent is running the flow
    // currently open on the canvas before enabling the live-counter overlay.
    // bucketId="default" matches what EFM expects for non-registry flows.
    {
        cJSON* uri = cJSON_CreateObject();
        cJSON_AddStringToObject(uri, "registryUrl", "");
        cJSON_AddStringToObject(uri, "bucketId",    "default");
        cJSON_AddStringToObject(uri, "flowId",      engine.flow_id());
        cJSON_AddItemToObject(fi, "versionedFlowSnapshotURI", uri);
    }

    // ---- queues: one entry per connection, keyed by connection UUID ---------
    // Despite MiNiFi C++ source keying queues by `getName()`, the EFM C2
    // protocol documentation example keys them by the connection UUID, and
    // empirically EFM's Flow Designer Monitor view needs the UUID key to
    // bind queue stats to the corresponding connector on the canvas (i.e.
    // to light up the green active-connection badge).  The human-readable
    // name is preserved as a `name` child field.
    cJSON* queues = cJSON_CreateObject();
    for (size_t i = 0; i < engine.conn_count(); ++i) {
        const FlowEngine::Connection& c = engine.conn(i);
        if (c.id[0] == '\0') continue;   // boot-default connection has no UUID

        const char* display_name = c.name[0] ? c.name : c.rel;

        cJSON* q = cJSON_CreateObject();
        cJSON_AddNumberToObject(q, "dataSize",    0);   // byte tracking NYI
        cJSON_AddNumberToObject(q, "dataSizeMax", 104857600);  // 100 MB default
        cJSON_AddStringToObject(q, "name",        display_name);
        cJSON_AddNumberToObject(q, "size",
                                static_cast<double>(engine.conn_queue_size(i)));
        cJSON_AddNumberToObject(q, "sizeMax",     2000);
        cJSON_AddStringToObject(q, "uuid",        c.id);
        cJSON_AddItemToObject(queues, c.id, q);
    }
    cJSON_AddItemToObject(fi, "queues", queues);

    // NOTE: MiNiFi C++ does NOT emit `flowInfo.components` -- that map lives
    // only under `agentInfo.status.components`.  The previous duplicate here
    // was tolerated but not part of the schema, so it has been removed.

    // ---- processorStatuses: one entry per node -------------------------------
    // `groupId` MUST be the root process-group UUID, NOT the flow UUID --
    // EFM indexes monitor metrics by (processGroupId, processorId).  Using
    // engine.flow_id() here meant the lookup always missed and the canvas
    // never lit up the counter overlay.
    // activeThreadCount/terminatedThreadCount use -1 as MiNiFi's "unknown"
    // sentinel; 0 means "definitely zero" which trips Monitor's liveness check.
    cJSON* statuses = cJSON_CreateArray();
    for (size_t i = 0; i < engine.node_count(); ++i) {
        const FlowEngine::Node& n = engine.node(i);
        if (!n.active || n.desc == nullptr) continue;

        const FlowEngine::NodeStats& st = n.stats;
        cJSON* ps = cJSON_CreateObject();
        cJSON_AddStringToObject(ps, "id",          n.id);
        cJSON_AddStringToObject(ps, "groupId",     process_group_id());
        cJSON_AddStringToObject(ps, "runStatus",   "RUNNING");
        cJSON_AddNumberToObject(ps, "bytesRead",   0);
        cJSON_AddNumberToObject(ps, "bytesWritten",
                                static_cast<double>(st.bytes_out));
        cJSON_AddNumberToObject(ps, "flowFilesIn",
                                static_cast<double>(st.ff_in));
        cJSON_AddNumberToObject(ps, "flowFilesOut",
                                static_cast<double>(st.ff_out));
        cJSON_AddNumberToObject(ps, "bytesIn",     0);
        cJSON_AddNumberToObject(ps, "bytesOut",
                                static_cast<double>(st.bytes_out));
        cJSON_AddNumberToObject(ps, "invocations",
                                static_cast<double>(st.invocations));
        cJSON_AddNumberToObject(ps, "processingNanos",       0);
        cJSON_AddNumberToObject(ps, "activeThreadCount",     -1);
        cJSON_AddNumberToObject(ps, "terminatedThreadCount", -1);
        cJSON_AddItemToArray(statuses, ps);
    }
    cJSON_AddItemToObject(fi, "processorStatuses", statuses);

    return fi;
}

char* build_heartbeat(bool include_manifest, size_t* out_len) {
    cJSON* root = cJSON_CreateObject();

    char hb_id[80];
    build_heartbeat_id(hb_id, sizeof(hb_id));
    cJSON_AddStringToObject(root, "identifier", hb_id);
    cJSON_AddStringToObject(root, "operation",  "HEARTBEAT");

    cJSON_AddItemToObject(root, "agentInfo",  build_agent_info(include_manifest));
    cJSON_AddItemToObject(root, "deviceInfo", build_device_info());
    cJSON_AddItemToObject(root, "flowInfo",   build_flow_info());

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) return nullptr;
    if (out_len != nullptr) *out_len = std::strlen(json);
    return json;
}

// ---- Flow-config fetch and apply -------------------------------------------

// GET the flow-definition JSON from `url` into s_flow_buf.
Status fetch_flow_config(const char* url) {
    ESP_LOGI(TAG, "fetching flow config from: %s", url);

    s_flow_len    = 0;
    s_flow_buf[0] = '\0';

    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.method            = HTTP_METHOD_GET;
    cfg.event_handler     = flow_http_event;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms        = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGE(TAG, "flow fetch: esp_http_client_init failed");
        return Status::OutOfMemory;
    }

    const esp_err_t err = esp_http_client_perform(client);
    Status rc = Status::Ok;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "flow fetch failed: %s", esp_err_to_name(err));
        rc = Status::IoError;
    } else {
        const int http_status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "flow fetch -> HTTP %d (%u bytes)",
                 http_status, static_cast<unsigned>(s_flow_len));
        if (http_status >= 400) {
            rc = Status::IoError;
        }
    }

    esp_http_client_cleanup(client);
    return rc;
}

// Resolve the flow-config URL for an UPDATE_CONFIGURATION op.
//
// EFM 2.x puts the URL in args.location (primary) or args.flowUrl (alias).
// Older EFM / MiNiFi protocol variants used args.configuration or args.url.
// If none of those keys are present we fall back to constructing the
// configContent URL from the heartbeat base + op identifier.
//
// Key precedence (in order):
//   args.location       -- observed in Cloudera EFM 2.x
//   args.flowUrl        -- alias seen in some builds
//   args.configuration  -- legacy MiNiFi C++ C2 protocol
//   args.url            -- legacy alternate
//   constructed fallback
void resolve_config_url(cJSON* op, char* out, size_t out_size) {
    out[0] = '\0';

    cJSON* args = cJSON_GetObjectItem(op, "args");
    if (args != nullptr) {
        // Always log the full args so we can see what EFM actually sent.
        char* args_str = cJSON_PrintUnformatted(args);
        if (args_str) {
            ESP_LOGI(TAG, "  UPDATE args: %.300s", args_str);
            cJSON_free(args_str);
        }

        static const char* kKeys[] = {
            "location", "flowUrl", "configuration", "url", nullptr
        };
        for (int k = 0; kKeys[k] != nullptr; ++k) {
            const char* url = cJSON_GetStringValue(
                cJSON_GetObjectItem(args, kKeys[k]));
            if (url != nullptr && url[0] != '\0') {
                ESP_LOGI(TAG, "  using args.%s: %s", kKeys[k], url);
                std::strncpy(out, url, out_size - 1);
                out[out_size - 1] = '\0';
                return;
            }
        }
        ESP_LOGW(TAG, "  no recognised URL key in args; falling back to constructed URL");
    } else {
        ESP_LOGW(TAG, "  UPDATE op has no 'args'; falling back to constructed URL");
    }

    const char* op_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(op, "identifier"));
    if (op_id == nullptr || op_id[0] == '\0') {
        ESP_LOGE(TAG, "  op has no identifier; cannot resolve config URL");
        return;
    }
    build_config_content_url(out, out_size, op_id);
    ESP_LOGW(TAG, "  constructed fallback configContent URL: %s", out);
}

// Scan `url` left-to-right for the first UUID-shaped token
// (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx, 36 chars).
// Writes into dst[0..36] and returns true when found.
// Used to recover the flow UUID from the EFM flows/ URL when the payload
// format (MiNiFi YAML v3) doesn't carry an explicit flow identifier.
static bool extract_uuid_from_url(const char* url, char* dst, size_t dst_size) {
    if (dst_size < 37) return false;
    static const int kDashPos[] = {8, 13, 18, 23, -1};
    for (const char* p = url; *p != '\0'; ++p) {
        // Fast pre-screen: dashes must be at positions 8, 13, 18, 23.
        if (p[8]  != '-') continue;
        if (p[13] != '-') { p += 8;  continue; }
        if (p[18] != '-') { p += 13; continue; }
        if (p[23] != '-') { p += 18; continue; }

        bool ok = true;
        for (int i = 0; i < 36 && ok; +