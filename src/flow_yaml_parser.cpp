// flow_yaml_parser.cpp -- parse MiNiFi Config Version 3 YAML into a FlowDef.
//
// EFM serves this format to agents that implement the MiNiFi C2 protocol,
// because MiNiFi C++ reads exactly this format.  MicroFi uses the same
// protocol, so EFM hands us the same YAML.  The "MiNiFi Config" label in the
// file is EFM's serialisation header -- no MiNiFi code runs on the device.
//
// Relevant schema excerpt:
//
//   MiNiFi Config Version: 3
//   Processors:
//   - id: <uuid>
//     name: <display name>
//     class: org.apache.nifi.processors.standard.GenerateFlowFile
//     max concurrent tasks: 1
//     scheduling strategy: TIMER_DRIVEN
//     scheduling period: 1 sec
//     ...
//     auto-terminated relationships list:
//     Properties:
//       File Size: 1 kB
//       Batch Size: 1
//       Data Format: Binary
//       Unique FlowFiles: 'true'
//       Custom Text: ''
//   Connections:
//   - id: <uuid>
//     name: <display name>
//     source id: <uuid>
//     source relationship names:
//     - success
//     destination id: <uuid>
//     max work queue size: 10000
//     ...
//
// The parser is a line-by-line state machine.  It uses a 512-byte line
// buffer on the C2 task stack (which has 10 KB -- plenty of room).

#include "microfi/flow_parser.h"
#include "microfi/flow_def.h"

#include "esp_log.h"

#include <cstring>

namespace microfi {

namespace {

const char* TAG = "microfi.flow_parser";

// ---- Mini utilities --------------------------------------------------------

// Count leading spaces (= YAML indent level).
static int indent_of(const char* line) {
    int n = 0;
    while (line[n] == ' ') ++n;
    return n;
}

// Return pointer past leading spaces (does NOT skip dashes).
static const char* ltrim(const char* s) {
    while (*s == ' ') ++s;
    return s;
}

// If `trimmed` starts with `key` followed by ':', return a pointer to the
// value string (after the ': ').  Otherwise return nullptr.
// Works for keys that may contain spaces (e.g. "source id").
static const char* value_of(const char* trimmed, const char* key) {
    const size_t klen = std::strlen(key);
    if (std::strncmp(trimmed, key, klen) != 0) return nullptr;
    const char* p = trimmed + klen;
    while (*p == ' ') ++p;
    if (*p != ':') return nullptr;
    ++p;
    while (*p == ' ') ++p;
    return p;  // pointer into the line buffer
}

// strncpy into `dst` (size `n`) then strip trailing whitespace + single quotes.
static void store(char* dst, size_t n, const char* src) {
    if (src == nullptr || n == 0) return;
    std::strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';

    // Strip trailing CR / LF / spaces
    size_t len = std::strlen(dst);
    while (len > 0 && (dst[len-1] == '\r' || dst[len-1] == '\n' ||
                       dst[len-1] == ' '  || dst[len-1] == '\t'))
        dst[--len] = '\0';

    // Strip surrounding single quotes ('value') or double quotes ("value")
    if (len >= 2 &&
        ((dst[0] == '\'' && dst[len-1] == '\'') ||
         (dst[0] == '"'  && dst[len-1] == '"'))) {
        std::memmove(dst, dst + 1, len - 1);
        dst[len - 2] = '\0';
    }
}

// Strip FQN prefix: "org.apache.nifi.processors.standard.GFF" → "GFF"
static const char* short_class(const char* fqn) {
    const char* dot = std::strrchr(fqn, '.');
    return dot ? dot + 1 : fqn;
}

// ---- State machine ---------------------------------------------------------

enum class Sec  { Other, Procs, Conns };
enum class Sub  { None, Proc, ProcProps, Conn, ConnRels };

}  // namespace

// ---------------------------------------------------------------------------

Status flow_yaml_parse(const char* yaml, FlowDef& out) {
    std::memset(&out, 0, sizeof(out));
    if (yaml == nullptr || yaml[0] == '\0') return Status::InvalidArg;

    Sec sec      = Sec::Other;
    Sub sub      = Sub::None;
    int cur_proc = -1;   // current node index in out.nodes[]
    int cur_conn = -1;   // current connection index in out.connections[]

    const char* p = yaml;
    char        line[512];

    while (*p != '\0') {
        // ---- Read one line ------------------------------------------------
        size_t len = 0;
        while (*p != '\0' && *p != '\n' && len < sizeof(line) - 1)
            line[len++] = *p++;
        if (*p == '\n') ++p;
        line[len] = '\0';
        if (len == 0) continue;   // blank line

        const int   ind = indent_of(line);
        const char* tri = ltrim(line);   // trimmed (no leading spaces)

        // ---- Top-level section headers (indent 0, not a list item) --------
        if (ind == 0 && tri[0] != '-') {
            if (std::strncmp(tri, "Processors:", 11) == 0) {
                sec = Sec::Procs; sub = Sub::None;
            } else if (std::strncmp(tri, "Connections:", 12) == 0) {
                sec = Sec::Conns; sub = Sub::None;
            } else {
                sec = Sec::Other; sub = Sub::None;
            }
            continue;
        }

        // ---- New list items at indent 0 (Processor or Connection) ---------
        if (ind == 0 && tri[0] == '-') {
            if (sec == Sec::Procs) {
                if (out.node_count < kMaxFlowNodes) {
                    cur_proc = (int)out.node_count++;
                    std::memset(&out.nodes[cur_proc], 0, sizeof(FlowNode));
                } else {
                    cur_proc = -1;
                    ESP_LOGW(TAG, "YAML: exceeded kMaxFlowNodes (%u); extra processors ignored",
                             static_cast<unsigned>(kMaxFlowNodes));
                }
                sub = Sub::Proc;
                // "- id: <uuid>" is sometimes on the dash line itself.
                const char* after_dash = tri + 1;
                while (*after_dash == ' ') ++after_dash;
                const char* val = value_of(after_dash, "id");
                if (val && cur_proc >= 0)
                    store(out.nodes[cur_proc].id, sizeof(out.nodes[cur_proc].id), val);
            } else if (sec == Sec::Conns) {
                if (out.connection_count < kMaxFlowConnections) {
                    cur_conn = (int)out.connection_count++;
                    std::memset(&out.connections[cur_conn], 0, sizeof(FlowConnection));
                    // default relationship in case the list is empty
                    store(out.connections[cur_conn].relationship,
                          sizeof(out.connections[cur_conn].relationship), "success");
                    // EFM emits connections with "- id: <uuid>" on the dash line.
                    // Without this, flowInfo.queues stays empty and EFM never
                    // lights up the active-connection badge on the canvas.
                    const char* after_dash = tri + 1;
                    while (*after_dash == ' ') ++after_dash;
                    const char* val = value_of(after_dash, "id");
                    if (val != nullptr)
                        store(out.connections[cur_conn].id,
                              sizeof(out.connections[cur_conn].id), val);
                } else {
                    cur_conn = -1;
                    ESP_LOGW(TAG, "YAML: exceeded kMaxFlowConnections (%u); extra connections ignored",
                             static_cast<unsigned>(kMaxFlowConnections));
                }
                sub = Sub::Conn;
            }
            continue;
        }

        // ---- Processor fields (indent 2) and Properties (indent 4) --------
        if (sec == Sec::Procs && cur_proc >= 0) {

            // Exiting ProcProps when indent drops below 4
            if (sub == Sub::ProcProps && ind < 4) {
                sub = Sub::Proc;
                // fall through so this line is handled as a Proc field
            }

            if (sub == Sub::Proc && ind == 2) {
                if (tri[0] == '-') continue;   // list item (e.g. auto-terminated rel)

                const char* val;
                if ((val = value_of(tri, "id")) != nullptr) {
                    store(out.nodes[cur_proc].id,
                          sizeof(out.nodes[cur_proc].id), val);
                } else if ((val = value_of(tri, "class")) != nullptr) {
                    char cls[128];
                    store(cls, sizeof(cls), val);
                    store(out.nodes[cur_proc].type,
                          sizeof(out.nodes[cur_proc].type), short_class(cls));
                } else if (std::strncmp(tri, "Properties:", 11) == 0) {
                    sub = Sub::ProcProps;
                }
                continue;
            }

            if (sub == Sub::ProcProps && ind == 4) {
                // "    Key: Value" -- find first colon
                const char* colon = std::strchr(tri, ':');
                if (colon != nullptr &&
                    out.nodes[cur_proc].property_count < kMaxNodeProperties) {
                    size_t pk = out.nodes[cur_proc].property_count++;
                    NodeProperty& prop = out.nodes[cur_proc].properties[pk];
                    size_t klen = static_cast<size_t>(colon - tri);
                    if (klen >= sizeof(prop.key)) klen = sizeof(prop.key) - 1;
                    std::strncpy(prop.key, tri, klen);
                    prop.key[klen] = '\0';
                    const char* vp = colon + 1;
                    while (*vp == ' ') ++vp;
                    store(prop.value, sizeof(prop.value), vp);
                }
                continue;
            }
        }

        // ---- Connection fields (indent 2) and relationship list ------------
        if (sec == Sec::Conns && cur_conn >= 0) {

            // Exiting ConnRels when we see a non-list item at indent 2
            if (sub == Sub::ConnRels && ind == 2 && tri[0] != '-') {
                sub = Sub::Conn;
                // fall through so this line is handled as a Conn field
            }

            if (sub == Sub::Conn && ind == 2) {
                if (tri[0] == '-') continue;   // unexpected list item -- skip

                const char* val;
                if ((val = value_of(tri, "id")) != nullptr) {
                    store(out.connections[cur_conn].id,
                          sizeof(out.connections[cur_conn].id), val);
                } else if ((val = value_of(tri, "name")) != nullptr) {
                    store(out.connections[cur_conn].name,
                          sizeof(out.connections[cur_conn].name), val);
                } else if ((val = value_of(tri, "source id")) != nullptr) {
                    store(out.connections[cur_conn].src_id,
                          sizeof(out.connections[cur_conn].src_id), val);
                } else if ((val = value_of(tri, "destination id")) != nullptr) {
                    store(out.connections[cur_conn].dst_id,
                          sizeof(out.connections[cur_conn].dst_id), val);
                } else if (std::strncmp(tri, "source relationship names:", 25) == 0) {
                    sub = Sub::ConnRels;
                }
                continue;
            }

            // Relationship list item: "  - success"
            if (sub == Sub::ConnRels && ind == 2 && tri[0] == '-') {
                const char* rp = tri + 1;
                while (*rp == ' ') ++rp;
                store(out.connections[cur_conn].relationship,
                      sizeof(out.connections[cur_conn].relationship), rp);
                sub = Sub::Conn;   // only the first relationship is used
                continue;
            }
        }
    }

    // ---- Validation --------------------------------------------------------
    if (out.node_count == 0) {
        ESP_LOGE(TAG, "YAML parse: no valid processor nodes found");
        return Status::ParseError;
    }

    // Log what we found so the operator can confirm the topology.
    ESP_LOGI(TAG, "YAML parse OK: %u node(s), %u connection(s)",
             static_cast<unsigned>(out.node_count),
             static_cast<unsigned>(out.connection_count));
    for (size_t i = 0; i < out.node_count; ++i) {
        ESP_LOGI(TAG, "  node[%u] %-22s id=%.8s...",
                 static_cast<unsigned>(i),
                 out.nodes[i].type[0] ? out.nodes[i].type : "(unknown)",
                 out.nodes[i].id);
    }
    for (size_t i = 0; i < out.connection_count; ++i) {
        ESP_LOGI(TAG, "  conn[%u] %.8s -> %.8s  rel=%s  id=%.8s...",
                 static_cast<unsigned>(i),
                 out.connections[i].src_id,
                 out.connections[i].dst_id,
                 out.connections[i].relationship,
                 out.connections[i].id[0] ? out.connections[i].id : "(none)");
    }
    return Status::Ok;
}

}  // namespace microfi
