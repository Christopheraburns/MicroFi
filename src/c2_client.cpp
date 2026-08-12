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
// Operation acknowledge:
//   After the UPDATE/configuration op is fetched+parsed+applied (or fails),
//   POST an explicit acknowledge to CONFIG_MICROFI_C2_ACK_URL.  EFM 2.x does
//   NOT treat a matching heartbeat flowInfo.flowId as an implicit ack -- an
//   unacknowledged operation row times out to FAILED on the server.  EFM maps
//   the ack's operationState.state to the operation row:
//     FULLY_APPLIED -> DONE, NO_OPERATION -> NOOP, anything else -> FAILED.

#include "microfi/c2_client.h"

#include "microfi/agent_id.h"
#include "microfi/flow_def.h"
#include "microfi/flow_engine.h"
#include "microfi/flow_parser.h"
#include "microfi/manifest.h"
#include "microfi/registry.h"
#include "microfi/flow_store.h"
#include "microfi/storage.h"
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

#if defined(CONFIG_MICROFI_STORAGE_METRICS)
    // Durable-storage metrics. Reported as 0/0 (with fill_percent=0) when
    // the repository failed to mount; the agent stays operational in
    // volatile-only mode but operators can see the failure via the
    // capacity_bytes=0 sentinel.
    IRepository* repo = repository();
    if (repo != nullptr) {
        const RepositoryStats st = repo->stats();
        cJSON_AddNumberToObject(mf, "littleFsUsedBytes",
                                static_cast<double>(st.used_bytes));
        cJSON_AddNumberToObject(mf, "littleFsCapacityBytes",
                                static_cast<double>(st.capacity_bytes));
        cJSON_AddNumberToObject(mf, "littleFsFillPercent",
                                static_cast<double>(st.fill_percent));
        cJSON_AddNumberToObject(mf, "evictionCount",
                                static_cast<double>(st.eviction_count));
        cJSON_AddNumberToObject(mf, "failedWrites",
                                static_cast<double>(st.failed_writes));
        cJSON_AddNumberToObject(mf, "storedRecords",
                                static_cast<double>(st.record_count));
    } else {
        cJSON_AddNumberToObject(mf, "littleFsUsedBytes",     0);
        cJSON_AddNumberToObject(mf, "littleFsCapacityBytes", 0);
        cJSON_AddStringToObject(mf, "storageStatus",         "unmounted");
    }
#endif

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
        for (int i = 0; i < 36 && ok; ++i) {
            char c = p[i];
            bool is_dash = (i == 8 || i == 13 || i == 18 || i == 23);
            if (is_dash) {
                if (c != '-') ok = false;
            } else {
                if (!((c >= '0' && c <= '9') ||
                      (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F'))) ok = false;
            }
        }
        if (ok) {
            std::strncpy(dst, p, 36);
            dst[36] = '\0';
            return true;
        }
    }
    (void)kDashPos;  // suppress unused-variable warning
    return false;
}

// ---- Operation acknowledge -------------------------------------------------

// POST an operation acknowledge to EFM.  Body is deliberately minimal --
// {operationId, operationState} only.  Including agentInfo/deviceInfo/flowInfo
// makes EFM additionally process the ack as a heartbeat (its
// containsAdditionalInfo path); the plain body keeps the ack side-effect-free
// and EFM resolves the agent from the operation row itself.
void post_operation_ack(const char* op_id, bool applied, const char* details) {
    if (op_id == nullptr || op_id[0] == '\0') {
        ESP_LOGW(TAG, "ack skipped: operation has no identifier");
        return;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "operationId", op_id);
    cJSON* st = cJSON_CreateObject();
    cJSON_AddStringToObject(st, "state", applied ? "FULLY_APPLIED" : "NOT_APPLIED");
    if (details != nullptr && details[0] != '\0') {
        cJSON_AddStringToObject(st, "details", details);
    }
    cJSON_AddItemToObject(root, "operationState", st);

    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        ESP_LOGE(TAG, "ack build: OOM");
        return;
    }

    esp_http_client_config_t cfg = {};
    cfg.url               = CONFIG_MICROFI_C2_ACK_URL;
    cfg.method            = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms        = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        cJSON_free(body);
        ESP_LOGE(TAG, "ack: esp_http_client_init failed");
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, std::strlen(body));

    const esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ack POST failed: %s", esp_err_to_name(err));
    } else {
        const int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "ack op=%s state=%s -> HTTP %d",
                 op_id, applied ? "FULLY_APPLIED" : "NOT_APPLIED", status);
    }
    esp_http_client_cleanup(client);
    cJSON_free(body);
}

// ---- Response processor ----------------------------------------------------

void process_response(const char* body, size_t /*len*/) {
    if (body == nullptr || body[0] == '\0') return;
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) {
        ESP_LOGW(TAG, "response not JSON (first 80 bytes): %.80s", body);
        return;
    }

    cJSON* ops = cJSON_GetObjectItem(root, "requestedOperations");
    if (ops == nullptr || !cJSON_IsArray(ops)) {
        ESP_LOGD(TAG, "no requestedOperations in response");
        cJSON_Delete(root);
        return;
    }

    const int n = cJSON_GetArraySize(ops);
    if (n == 0) { cJSON_Delete(root); return; }

    ESP_LOGI(TAG, "EFM requested %d operation(s):", n);

    for (int i = 0; i < n; ++i) {
        cJSON* op = cJSON_GetArrayItem(ops, i);
        if (op == nullptr) continue;

        const char* id      = cJSON_GetStringValue(cJSON_GetObjectItem(op, "identifier"));
        const char* type    = cJSON_GetStringValue(cJSON_GetObjectItem(op, "operation"));
        const char* operand = cJSON_GetStringValue(cJSON_GetObjectItem(op, "operand"));

        ESP_LOGI(TAG, "  [%d] op=%-15s operand=%-15s id=%s",
                 i,
                 type    ? type    : "?",
                 operand ? operand : "?",
                 id      ? id      : "?");

        if (type == nullptr || operand == nullptr) continue;

        // ---- DESCRIBE/manifest: re-include manifest on next heartbeat ----
        if (std::strcmp(type, "DESCRIBE") == 0 &&
            std::strcmp(operand, "manifest") == 0) {
            ESP_LOGI(TAG, "  -> manifest re-request; next heartbeat will carry it");
            s_need_send_manifest = true;
            continue;
        }

        // ---- UPDATE/configuration: fetch, parse, apply -------------------
        if (std::strcmp(type, "UPDATE") == 0 &&
            std::strcmp(operand, "configuration") == 0) {
            ESP_LOGI(TAG, "  -> UPDATE_CONFIGURATION: resolving flow config URL");

            char config_url[256] = {0};
            resolve_config_url(op, config_url, sizeof(config_url));

            if (config_url[0] == '\0') {
                ESP_LOGE(TAG, "  -> could not resolve config URL; ignoring op");
                post_operation_ack(id, false, "could not resolve flow config URL");
                continue;
            }

            if (fetch_flow_config(config_url) != Status::Ok) {
                ESP_LOGE(TAG, "  -> flow fetch failed");
                post_operation_ack(id, false, "flow config fetch failed");
                continue;
            }
            // Log full YAML in 400-char chunks so we can find the flow identifier field.
            ESP_LOGI(TAG, "  -> flow body (%u bytes):", static_cast<unsigned>(s_flow_len));
            for (size_t _off = 0; _off < s_flow_len; _off += 400)
                ESP_LOGI(TAG, "  [%u]: %.400s", static_cast<unsigned>(_off), s_flow_buf + _off);

            FlowDef def = {};
            const Status parse_rc = flow_parse(s_flow_buf, def);
            if (parse_rc != Status::Ok) {
                ESP_LOGE(TAG, "  -> flow parse failed (%s); body[0..79]: %.80s",
                         to_string(parse_rc), s_flow_buf);
                char details[64];
                std::snprintf(details, sizeof(details),
                              "flow parse failed: %s", to_string(parse_rc));
                post_operation_ack(id, false, details);
                continue;
            }
            ESP_LOGI(TAG, "  -> parsed %u node(s), %u conn(s)",
                     static_cast<unsigned>(def.node_count),
                     static_cast<unsigned>(def.connection_count));

            // MiNiFi YAML v3 carries no flow UUID; recover it from the URL.
            // The EFM flows/ endpoint always embeds the UUID in the path:
            //   .../flows/<uuid>/...
            // Without this, flowInfo.flowId stays all-zeros and EFM never
            // marks the agent as "synchronized".
            if (def.flow_id[0] == '\0') {
                if (extract_uuid_from_url(config_url, def.flow_id,
                                          sizeof(def.flow_id))) {
                    ESP_LOGI(TAG, "  -> flow_id recovered from URL: %.36s",
                             def.flow_id);
                } else {
                    ESP_LOGW(TAG, "  -> could not recover flow_id from URL;"
                             " EFM may not show 'synchronized'");
                }
            }

            const Status apply_rc = FlowEngine::instance().apply(def);
            if (apply_rc != Status::Ok) {
                ESP_LOGE(TAG, "  -> engine apply rejected (%s)", to_string(apply_rc));
                char details[64];
                std::snprintf(details, sizeof(details),
                              "engine apply rejected: %s", to_string(apply_rc));
                post_operation_ack(id, false, details);
                continue;
            }
            s_dump_next_heartbeat = true;   // log the next heartbeat so we can inspect monitoring fields
            // Persist the raw flow def so it survives a power cycle.
            // Non-fatal: if the save fails we still apply the flow in RAM.
            const Status sv = flow_def_save(s_flow_buf, s_flow_len);
            if (sv != Status::Ok) {
                ESP_LOGW(TAG, "  -> flow_def_save failed (%s) -- volatile only",
                         to_string(sv));
            }
            // Persist the flow UUID separately (37 bytes).  The YAML body has no
            // UUID, so without this the engine would always boot with flow_id=zeros
            // and EFM would re-push UPDATE_CONFIGURATION on every power cycle.
            if (def.flow_id[0] != '\0') {
                const Status sid = flow_id_save(def.flow_id);
                if (sid != Status::Ok) {
                    ESP_LOGW(TAG, "  -> flow_id_save failed (%s)", to_string(sid));
                }
            }
            ESP_LOGI(TAG, "  -> flow queued for apply (flow_id=%.36s)", def.flow_id);
            post_operation_ack(id, true, "flow applied");
            continue;
        }
    }
    cJSON_Delete(root);
}

// ---- Heartbeat POST --------------------------------------------------------

// Dump the full heartbeat body to the serial log in 500-char chunks.
// Called once on the first heartbeat after a flow apply so we can inspect
// what MicroFi is sending for the monitoring fields.
static void dump_heartbeat(const char* body, size_t len) {
    ESP_LOGI(TAG, "=== HEARTBEAT DUMP (%u bytes) ===", static_cast<unsigned>(len));
    for (size_t off = 0; off < len; off += 500)
        ESP_LOGI(TAG, "[hb %u]: %.500s", static_cast<unsigned>(off), body + off);
    ESP_LOGI(TAG, "=== END HEARTBEAT DUMP ===");
}

Status post_heartbeat() {
    const bool include_manifest = s_need_send_manifest;
    size_t body_len = 0;
    char* body = build_heartbeat(include_manifest, &body_len);
    if (body == nullptr) {
        ESP_LOGE(TAG, "build_heartbeat: OOM");
        return Status::OutOfMemory;
    }

    if (s_dump_next_heartbeat) {
        dump_heartbeat(body, body_len);
        s_dump_next_heartbeat = false;
    }

    s_resp_len    = 0;
    s_resp_buf[0] = '\0';

    esp_http_client_config_t cfg = {};
    cfg.url               = CONFIG_MICROFI_C2_HEARTBEAT_URL;
    cfg.method            = HTTP_METHOD_POST;
    cfg.event_handler     = http_event;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms        = 10000;
    cfg.buffer_size_tx    = 8192;  // heartbeat with manifest is ~6.5 KB; default 512 B causes
                                   // 13 small send() calls that stall on TCP slow-start and
                                   // trip Jetty's 30-second idle timeout mid-body.

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        cJSON_free(body);
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return Status::OutOfMemory;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, body_len);

    const esp_err_t err = esp_http_client_perform(client);
    Status rc = Status::Ok;
    int    status = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "heartbeat POST failed: %s", esp_err_to_name(err));
        rc = Status::IoError;
    } else {
        status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG,
                 "heartbeat #%llu -> %d (sent %u bytes, manifest=%s, recv %u bytes)",
                 static_cast<unsigned long long>(s_heartbeat_counter),
                 status,
                 static_cast<unsigned>(body_len),
                 include_manifest ? "yes" : "no",
                 static_cast<unsigned>(s_resp_len));
        if (status >= 400) rc = Status::IoError;
    }
    esp_http_client_cleanup(client);
    cJSON_free(body);

    if (rc == Status::Ok) {
        if (include_manifest) s_need_send_manifest = false;
        process_response(s_resp_buf, s_resp_len);
        ++s_heartbeat_counter;
    }
    return rc;
}

// ---- Heartbeat task --------------------------------------------------------

void heartbeat_task(void* /*arg*/) {
    const TickType_t period      = pdMS_TO_TICKS(CONFIG_MICROFI_HEARTBEAT_INTERVAL_MS);
    // After a connect failure, retry after a short back-off rather than
    // waiting the full heartbeat interval.  Cap at the normal period.
    const TickType_t retry_delay = pdMS_TO_TICKS(5000);   // 5 s
    bool logged_manifest = false;

    while (true) {
        if (wifi_connected()) {
            if (!logged_manifest) {
                // Log manifest in 500-char chunks for diagnostics.
                // Remove once property rendering is confirmed stable.
                const char* mj   = manifest_json();
                size_t      mlen = manifest_json_len();
                ESP_LOGI(TAG, "manifest: %u bytes, hash=%s",
                         (unsigned)mlen, manifest_hash());
                for (size_t off = 0; off < mlen; off += 500) {
                    ESP_LOGI(TAG, "manifest[%u]: %.500s", (unsigned)off, mj + off);
                }
                logged_manifest = true;
            }

            const Status rc = post_heartbeat();
            if (rc != Status::Ok) {
                // Short retry delay: the WiFi link may have just recovered or
                // EFM may be momentarily unreachable.  Don't wait a full period.
                ESP_LOGW(TAG, "heartbeat failed; will retry in 5 s");
                vTaskDelay(retry_delay);
                continue;
            }
        } else {
            ESP_LOGW(TAG, "wifi down; skipping heartbeat");
        }
        vTaskDelay(period);
    }
}

}  // namespace

// ---- Loopback guard --------------------------------------------------------

namespace {
void warn_if_url_loopback() {
    const char* url = CONFIG_MICROFI_C2_HEARTBEAT_URL;
    const bool loopback =
        std::strstr(url, "localhost")  != nullptr ||
        std::strstr(url, "127.0.0.1") != nullptr;
    if (loopback) {
        ESP_LOGW(TAG, "================================================================");
        ESP_LOGW(TAG, "C2 heartbeat URL contains 'localhost' or '127.0.0.1':");
        ESP_LOGW(TAG, "  %s", url);
        ESP_LOGW(TAG, "Inside the device these resolve to the ESP32, not your dev box.");
        ESP_LOGW(TAG, "Fix: use the LAN IP of the EFM host in sdkconfig.");
        ESP_LOGW(TAG, "================================================================");
    }
}
}  // namespace

// ---- Public API ------------------------------------------------------------

const char* flow_config_json() { return s_flow_buf; }
size_t      flow_config_len()  { return s_flow_len;  }

Status c2_client_start() {
    warn_if_url_loopback();
    const BaseType_t ok = xTaskCreate(
        &heartbeat_task,
        "microfi-c2",
        /*stack_depth=*/10240,
        nullptr,
        tskIDLE_PRIORITY + 1,
        nullptr);
    return (ok == pdPASS) ? Status::Ok : Status::OutOfMemory;
}

}  // namespace microfi
