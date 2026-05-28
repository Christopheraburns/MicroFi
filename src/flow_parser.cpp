// flow_parser.cpp -- parse an EFM C2 configContent JSON into a FlowDef.
//
// The EFM configContent endpoint returns a NiFi versioned-flow-snapshot.
// We care about two arrays inside "flowContents":
//
//   processors[]:
//     { "identifier": "...", "type": "org.apache.nifi...GenerateFlowFile",
//       "properties": { "File Size": "1 kB", ... } }
//
//   connections[]:
//     { "source":      { "id": "<proc-uuid>" },
//       "destination": { "id": "<proc-uuid>" },
//       "selectedRelationships": ["success"] }
//
// Processor type names are fully qualified; we strip the package prefix so
// "org.apache.nifi.processors.standard.GenerateFlowFile" -> "GenerateFlowFile".
// That matches the name registered by MICROFI_REGISTER_PROCESSOR.

#include "microfi/flow_parser.h"
#include "microfi/flow_def.h"

#include "cJSON.h"
#include "esp_log.h"

#include <cstring>

namespace microfi {

namespace {

const char* TAG = "microfi.flow_parser";

// Strip everything up to (and including) the last '.' from a fully-qualified
// NiFi type name.  If there is no '.', returns the input string unchanged.
const char* short_type_name(const char* fqn) {
    const char* last_dot = std::strrchr(fqn, '.');
    return last_dot ? last_dot + 1 : fqn;
}

void safe_copy(char* dst, size_t dst_size, const char* src) {
    if (src == nullptr || dst_size == 0) {
        if (dst_size > 0) dst[0] = '\0';
        return;
    }
    std::strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

void parse_processor_list(cJSON* arr, FlowDef& out) {
    const int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; ++i) {
        if (out.node_count >= kMaxFlowNodes) {
            ESP_LOGW(TAG, "reached kMaxFlowNodes (%u); dropping remaining processors",
                     (unsigned)kMaxFlowNodes);
            break;
        }
        cJSON* item = cJSON_GetArrayItem(arr, i);
        if (item == nullptr) continue;

        // "identifier" is the UUID; some snapshots use "id" instead
        const char* id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "identifier"));
        if (id == nullptr) id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        const char* fq_type = cJSON_GetStringValue(cJSON_GetObjectItem(item, "type"));

        if (id == nullptr || fq_type == nullptr) {
            ESP_LOGW(TAG, "processor[%d]: missing identifier or type -- skipped", i);
            continue;
        }

        FlowNode& node = out.nodes[out.node_count];
        std::memset(&node, 0, sizeof(node));
        safe_copy(node.id,   sizeof(node.id),   id);
        safe_copy(node.type, sizeof(node.type),  short_type_name(fq_type));

        // Properties: flat object { "key": "value" }
        cJSON* props = cJSON_GetObjectItem(item, "properties");
        if (props != nullptr && cJSON_IsObject(props)) {
            for (cJSON* kv = props->child;
                 kv != nullptr && node.property_count < kMaxNodeProperties;
                 kv = kv->next) {
                if (kv->string == nullptr) continue;
                // Skip null values (property cleared in EFM UI)
                if (!cJSON_IsString(kv)) continue;
                NodeProperty& p = node.properties[node.property_count];
                safe_copy(p.key,   sizeof(p.key),   kv->string);
                safe_copy(p.value, sizeof(p.value),
                          kv->valuestring ? kv->valuestring : "");
                ++node.property_count;
            }
        }

        ESP_LOGI(TAG, "  node[%u] type=%-20s id=%.8s...",
                 (unsigned)out.node_count, node.type, node.id);
        ++out.node_count;
    }
}

void parse_connection_list(cJSON* arr, FlowDef& out) {
    const int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; ++i) {
        if (out.connection_count >= kMaxFlowConnections) {
            ESP_LOGW(TAG, "reached kMaxFlowConnections (%u); dropping rest",
                     (unsigned)kMaxFlowConnections);
            break;
        }
        cJSON* item = cJSON_GetArrayItem(arr, i);
        if (item == nullptr) continue;

        cJSON* src_obj = cJSON_GetObjectItem(item, "source");
        cJSON* dst_obj = cJSON_GetObjectItem(item, "destination");
        if (src_obj == nullptr || dst_obj == nullptr) continue;

        // Source and destination UUIDs may be under "id" or "identifier"
        const char* src_id = cJSON_GetStringValue(cJSON_GetObjectItem(src_obj, "id"));
        if (src_id == nullptr)
            src_id = cJSON_GetStringValue(cJSON_GetObjectItem(src_obj, "identifier"));
        const char* dst_id = cJSON_GetStringValue(cJSON_GetObjectItem(dst_obj, "id"));
        if (dst_id == nullptr)
            dst_id = cJSON_GetStringValue(cJSON_GetObjectItem(dst_obj, "identifier"));

        if (src_id == nullptr || dst_id == nullptr) {
            ESP_LOGW(TAG, "connection[%d]: missing source/destination id -- skipped", i);
            continue;
        }

        FlowConnection& conn = out.connections[out.connection_count];
        std::memset(&conn, 0, sizeof(conn));
        safe_copy(conn.src_id, sizeof(conn.src_id), src_id);
        safe_copy(conn.dst_id, sizeof(conn.dst_id), dst_id);

        // selectedRelationships: first entry wins; default to "success"
        cJSON* rels = cJSON_GetObjectItem(item, "selectedRelationships");
        if (rels != nullptr && cJSON_IsArray(rels) && cJSON_GetArraySize(rels) > 0) {
            safe_copy(conn.relationship, sizeof(conn.relationship),
                      cJSON_GetStringValue(cJSON_GetArrayItem(rels, 0)));
        } else {
            safe_copy(conn.relationship, sizeof(conn.relationship), "success");
        }

        ESP_LOGI(TAG, "  conn[%u]: %.8s->%.8s  rel=%s",
                 (unsigned)out.connection_count,
                 conn.src_id, conn.dst_id, conn.relationship);
        ++out.connection_count;
    }
}

}  // namespace

Status flow_parse(const char* body, FlowDef& out) {
    std::memset(&out, 0, sizeof(out));

    if (body == nullptr || body[0] == '\0') {
        ESP_LOGE(TAG, "empty flow body");
        return Status::InvalidArg;
    }

    // Skip leading whitespace to find the first meaningful character.
    const char* p = body;
    while (*p == ' ' || *p == '\n' || *p == '\r') ++p;

    // EFM returns MiNiFi YAML v3 when it treats the agent as MiNiFi C++.
    // Detect by the well-known header line.
    if (std::strncmp(p, "MiNiFi Config Version:", 22) == 0) {
        ESP_LOGI(TAG, "detected MiNiFi YAML v3 format -- delegating to YAML parser");
        return flow_yaml_parse(body, out);
    }

    // JSON: NiFi versioned-flow-snapshot starting with '{'.
    if (*p != '{') {
        ESP_LOGE(TAG, "unrecognised flow body format (first 40 chars): %.40s", p);
        return Status::ParseError;
    }

    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) {
        const char* near = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "JSON parse error near: %.60s", near ? near : "?");
        return Status::ParseError;
    }

    // Locate the flow-contents node.  EFM wraps it in "flowContents"; some
    // older versions use "content"; if neither exists we try root directly.
    cJSON* contents = cJSON_GetObjectItem(root, "flowContents");
    if (contents == nullptr) contents = cJSON_GetObjectItem(root, "content");
    if (contents == nullptr) contents = root;  // bare flow object

    // Flow identifier
    const char* fid = cJSON_GetStringValue(cJSON_GetObjectItem(contents, "identifier"));
    if (fid == nullptr)
        fid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "identifier"));
    if (fid != nullptr)
        std::strncpy(out.flow_id, fid, sizeof(out.flow_id) - 1);

    cJSON* procs = cJSON_GetObjectItem(contents, "processors");
    cJSON* conns = cJSON_GetObjectItem(contents, "connections");

    if (procs == nullptr || !cJSON_IsArray(procs)) {
        ESP_LOGE(TAG, "no 'processors' array found in flow definition");
        cJSON_Delete(root);
        return Status::ParseError;
    }

    ESP_LOGI(TAG, "parsing flow id=%.36s  (%d processor(s), %d connection(s))",
             out.flow_id[0] ? out.flow_id : "(none)",
             cJSON_GetArraySize(procs),
             conns ? cJSON_GetArraySize(conns) : 0);

    parse_processor_list(procs, out);
    if (conns != nullptr && cJSON_IsArray(conns))
        parse_connection_list(conns, out);

    cJSON_Delete(root);

    if (out.node_count == 0) {
        ESP_LOGE(TAG, "flow definition contains no usable processor nodes");
        return Status::ParseError;
    }

    ESP_LOGI(TAG, "flow parse OK: %u node(s), %u connection(s)",
             (unsigned)out.node_count, (unsigned)out.connection_count);
    return Status::Ok;
}

}  // namespace microfi
