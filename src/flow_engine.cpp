// flow_engine.cpp -- dynamic-graph flow scheduler.
//
// Stack-safety notes (root cause of the original Slice B crash):
//   FlowDef is ~9 KB (8 nodes × ~1.2 KB each).  Declaring a FlowDef as a
//   local variable in run_loop() or rebuild_from_def() overflows the 4 KB
//   engine-task stack immediately, producing the 0xa5a5a5a5 poison read and
//   Guru Meditation "LoadProhibited" panic.
//
//   Fixes applied:
//     1. pending_def_ / working_def_ are FlowEngine members (BSS), not locals.
//     2. run_loop() copies pending_def_ -> working_def_ under the mutex so
//        the copy stays in BSS; rebuild is called with working_def_ after
//        the mutex is released.
//     3. rebuild_from_def() builds directly into the live nodes_[] and
//        connections_[] member arrays -- no temporary Node[]/Connection[]
//        arrays on the stack.
//     4. Connection no longer embeds a Queue (copying semaphore handles via
//        memcpy would corrupt kernel objects).  Instead, queues_[c] is the
//        Queue for connection slot c.
//     5. Task stack bumped to 6144 bytes (FlowFile+Session peak is ~1.6 KB).

#include "microfi/flow_engine.h"

#include "microfi/flowfile_store.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/storage.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

namespace microfi {

static const char* TAG = "microfi.engine";

static constexpr const char* kDefaultSource = "GenerateFlowFile";
static constexpr const char* kDefaultSink   = "LogAttribute";
static constexpr const char* kSuccessRel    = "success";

// ---- Singleton ---------------------------------------------------------------

FlowEngine& FlowEngine::instance() {
    static FlowEngine e;
    return e;
}

FlowEngine::FlowEngine() {
    mutex_ = xSemaphoreCreateMutex();
    // Zero-init the POD node and connection arrays.
    // Queue objects in queues_[] are default-constructed by C++ and already
    // have valid semaphore handles -- do NOT memset them.
    std::memset(nodes_,       0, sizeof(nodes_));
    std::memset(connections_, 0, sizeof(connections_));
}

FlowEngine::~FlowEngine() {
    if (mutex_) vSemaphoreDelete(mutex_);
}

uint64_t FlowEngine::next_id() { return next_id_++; }

size_t FlowEngine::queue_depth() const {
    size_t total = 0;
    for (size_t i = 0; i < conn_count_; ++i)
        total += queues_[i].size();
    return total;
}

// ---- Default graph (boot) ----------------------------------------------------

Status FlowEngine::start() {
    if (started_) return Status::Internal;

    // If prime() was already called, the graph is set up and we go straight
    // to launching the task.  Otherwise install the boot-default graph.
    if (node_count_ == 0) {
        auto& reg = Registry::instance();
        const ProcessorDescriptor* src = reg.find(kDefaultSource);
        const ProcessorDescriptor* snk = reg.find(kDefaultSink);

        if (src == nullptr || snk == nullptr) {
            ESP_LOGE(TAG, "default processors '%s'/'%s' not found",
                     kDefaultSource, kDefaultSink);
            return Status::NotFound;
        }
        if (src->state_size > kStateBytes || snk->state_size > kStateBytes) {
            ESP_LOGE(TAG, "default processor state exceeds kStateBytes (%u)",
                     static_cast<unsigned>(kStateBytes));
            return Status::InvalidArg;
        }

        // Node 0: source
        {
            Node& n = nodes_[0];
            std::memset(&n, 0, sizeof(n));
            n.desc   = src;
            n.active = true;
            std::strncpy(n.id, "default-source", sizeof(n.id) - 1);
            if (src->on_init) src->on_init(n.state);
        }
        // Node 1: sink
        {
            Node& n = nodes_[1];
            std::memset(&n, 0, sizeof(n));
            n.desc   = snk;
            n.active = true;
            std::strncpy(n.id, "default-sink", sizeof(n.id) - 1);
            if (snk->on_init) snk->on_init(n.state);
        }
        node_count_ = 2;

        // Connection 0: node[0] success -> node[1]
        connections_[0].src = 0;
        connections_[0].dst = 1;
        std::strncpy(connections_[0].rel, kSuccessRel, sizeof(connections_[0].rel) - 1);
        queues_[0].set_name("default:GFF->LA");
        conn_count_ = 1;

        ESP_LOGI(TAG, "engine starting with boot-default graph [%s -> %s]",
                 kDefaultSource, kDefaultSink);
    } else {
        ESP_LOGI(TAG, "engine starting with primed graph: %u node(s), %u conn(s), id=%.36s",
                 static_cast<unsigned>(node_count_),
                 static_cast<unsigned>(conn_count_),
                 flow_id_[0] ? flow_id_ : "(none)");
    }

    started_ = true;
    const BaseType_t ok = xTaskCreate(
        &FlowEngine::task_entry, "microfi-engine",
        // Stack budget (deepest path: run_tick → on_trigger → try_push):
        //   run_tick locals (FlowFile in + 2×Session):   ~1722 B
        //   on_trigger (FlowFile f alive during transfer): ~1340 B
        //   try_push (FlowFile copy):                      ~1340 B
        //   Xtensa register window saves (~64B × 7):        ~450 B
        //   FreeRTOS overhead:                              ~768 B
        //   Total worst case:                              ~5620 B
        // 12288 gives ~6.5 KB headroom for future processors.
        // (ser_buf was moved to a static local in queue.cpp to keep it off-stack.)
        /*stack_depth=*/12288,
        this,
        tskIDLE_PRIORITY + 2,
        &task_);
    if (ok != pdPASS) {
        started_ = false;
        ESP_LOGE(TAG, "xTaskCreate failed");
        return Status::OutOfMemory;
    }
    return Status::Ok;
}

// ---- Synchronous prime (app_main, before start()) ----------------------------

Status FlowEngine::prime(const FlowDef& def) {
    if (started_) {
        ESP_LOGW(TAG, "prime() called after start(); use apply() instead");
        return Status::Internal;
    }
    if (def.node_count == 0) return Status::InvalidArg;
    ESP_LOGI(TAG, "priming engine from saved flow def: %u node(s), %u conn(s), id=%.36s",
             static_cast<unsigned>(def.node_count),
             static_cast<unsigned>(def.connection_count),
             def.flow_id[0] ? def.flow_id : "(none)");
    rebuild_from_def(def);
    return Status::Ok;
}

// ---- Flow apply (called from C2 task) ----------------------------------------

Status FlowEngine::apply(const FlowDef& def) {
    if (def.node_count == 0) return Status::InvalidArg;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    pending_def_   = def;   // struct copy into BSS member -- not on any task stack
    pending_apply_ = true;
    xSemaphoreGive(mutex_);
    ESP_LOGI(TAG, "flow apply queued: %u node(s), %u conn(s), id=%.36s",
             static_cast<unsigned>(def.node_count),
             static_cast<unsigned>(def.connection_count),
             def.flow_id[0] ? def.flow_id : "(none)");
    return Status::Ok;
}

// ---- Replay persisted FlowFiles from IRepository ----------------------------
//
// Called from app_main after prime()/start() so connection UUIDs are known.

void FlowEngine::replay_from_repository() {
    IRepository* repo = repository();
    if (repo == nullptr) return;

    RecordId cur;
    if (repo->oldest(&cur) != Status::Ok) {
        ESP_LOGD(TAG, "replay: repository empty");
        return;
    }

    uint32_t replayed = 0;
    uint32_t orphans  = 0;

    while (true) {
        uint8_t buf[kFlowFileRecordMaxBytes];
        size_t  buf_size = sizeof(buf);

        const Status rd = repo->read(cur, buf, &buf_size);
        if (rd != Status::Ok) {
            ESP_LOGW(TAG, "replay: unreadable record %llu -- erasing",
                     (unsigned long long)cur);
            repo->erase(cur);
        } else {
            char     conn_id[37] = {};
            FlowFile ff;
            if (flowfile_deserialize(buf, buf_size, conn_id, &ff) != Status::Ok) {
                ESP_LOGW(TAG, "replay: deserialize failed for record %llu -- erasing",
                         (unsigned long long)cur);
                repo->erase(cur);
            } else {
                bool found = false;
                for (size_t c = 0; c < conn_count_; ++c) {
                    if (std::strcmp(connections_[c].id, conn_id) == 0) {
                        ff.set_record_id(cur);
                        queues_[c].try_push(ff);
                        ++replayed;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    ESP_LOGW(TAG, "replay: no connection for UUID %.8s... -- erasing orphan",
                             conn_id);
                    repo->erase(cur);
                    ++orphans;
                }
            }
        }

        RecordId next_id;
        if (repo->next(cur, &next_id) != Status::Ok) break;
        cur = next_id;
    }

    if (replayed > 0 || orphans > 0) {
        ESP_LOGI(TAG, "replay: %u FlowFile(s) restored, %u orphan(s) erased",
                 replayed, orphans);
    }
}

// ---- Rebuild graph from FlowDef (engine task only) ---------------------------
//
// Builds directly into the live nodes_[], connections_[], queues_[] members.
// Since this is called exclusively from run_loop() (engine task), and the
// engine task is the only writer of those arrays, no additional locking is
// needed here.
//
// Stack budget: only small POD locals (loop indices, one FlowFile for draining).

void FlowEngine::rebuild_from_def(const FlowDef& def) {
    auto& reg = Registry::instance();

    // Step 1: Stop, then deactivate, all existing nodes.  on_stop releases
    //         external resources the old instances hold (httpd server, MQTT
    //         client, FreeRTOS queues) before Step 3 zeroes the state slabs;
    //         skipping it orphans them on every republish (#150).
    for (size_t i = 0; i < node_count_; ++i) {
        Node& n = nodes_[i];
        if (n.active && n.desc != nullptr && n.desc->on_stop != nullptr) {
            n.desc->on_stop(n.state);
        }
    }
    for (size_t i = 0; i < kMaxNodes; ++i) nodes_[i].active = false;

    // Step 2: Drain all queues.  Old FlowFiles are discarded -- the new graph
    //         starts fresh.  We use one stack-allocated FlowFile for this.
    {
        FlowFile discard;
        for (size_t i = 0; i < kMaxConnections; ++i) {
            while (queues_[i].try_pop(discard) == Status::Ok) { /* drain */ }
        }
    }

    // Step 3: Build new nodes directly into nodes_[].
    size_t new_nc = 0;
    for (size_t i = 0; i < def.node_count && new_nc < kMaxNodes; ++i) {
        const FlowNode& fn = def.nodes[i];
        const ProcessorDescriptor* desc = reg.find(fn.type);
        if (desc == nullptr) {
            ESP_LOGW(TAG, "  unknown processor type '%s' (id=%.8s); skipped",
                     fn.type, fn.id);
            continue;
        }
        if (desc->state_size > kStateBytes) {
            ESP_LOGW(TAG, "  '%s' state_size %u > kStateBytes %u; skipped",
                     fn.type,
                     static_cast<unsigned>(desc->state_size),
                     static_cast<unsigned>(kStateBytes));
            continue;
        }

        Node& n = nodes_[new_nc];
        n.desc = desc;
        std::memset(n.state, 0, sizeof(n.state));
        std::memset(&n.stats, 0, sizeof(n.stats));
        std::strncpy(n.id, fn.id, sizeof(n.id) - 1);
        n.id[sizeof(n.id) - 1] = '\0';
        n.prop_count = (fn.property_count < kMaxNodeProperties)
                         ? fn.property_count : kMaxNodeProperties;
        for (size_t j = 0; j < n.prop_count; ++j) n.props[j] = fn.properties[j];
        if (desc->on_init)      desc->on_init(n.state);
        if (desc->on_configure) desc->on_configure(n.state, n.props, n.prop_count);
        n.active = true;   // activate after init+configure so partial state isn't visible

        ESP_LOGI(TAG, "  node[%u] %-20s id=%.8s...",
                 static_cast<unsigned>(new_nc), fn.type, fn.id);
        ++new_nc;
    }

    // Clear any leftover slots from the previous (possibly larger) graph.
    for (size_t i = new_nc; i < node_count_; ++i) {
        std::memset(&nodes_[i], 0, sizeof(nodes_[i]));
    }
    node_count_ = new_nc;

    if (new_nc == 0) {
        ESP_LOGE(TAG, "rebuild: no valid nodes; old graph was cleared -- engine idle");
        conn_count_ = 0;
        return;
    }

    // Step 4: Build new connections directly into connections_[].
    //         UUID matching: nodes_[j].id vs FlowConnection.src_id / dst_id.
    size_t new_cc = 0;
    for (size_t i = 0; i < def.connection_count && new_cc < kMaxConnections; ++i) {
        const FlowConnection& fc = def.connections[i];
        int src_idx = -1, dst_idx = -1;
        for (size_t j = 0; j < new_nc; ++j) {
            if (std::strcmp(nodes_[j].id, fc.src_id) == 0) src_idx = (int)j;
            if (std::strcmp(nodes_[j].id, fc.dst_id) == 0) dst_idx = (int)j;
        }
        if (src_idx < 0 || dst_idx < 0) {
            ESP_LOGW(TAG, "  conn[%u]: src=%.8s or dst=%.8s not found; skipped",
                     static_cast<unsigned>(i), fc.src_id, fc.dst_id);
            continue;
        }
        connections_[new_cc].src = static_cast<uint8_t>(src_idx);
        connections_[new_cc].dst = static_cast<uint8_t>(dst_idx);
        std::strncpy(connections_[new_cc].rel, fc.relationship,
                     sizeof(connections_[new_cc].rel) - 1);
        connections_[new_cc].rel[sizeof(connections_[new_cc].rel) - 1] = '\0';
        std::strncpy(connections_[new_cc].id, fc.id,
                     sizeof(connections_[new_cc].id) - 1);
        connections_[new_cc].id[sizeof(connections_[new_cc].id) - 1] = '\0';
        std::strncpy(connections_[new_cc].name, fc.name,
                     sizeof(connections_[new_cc].name) - 1);
        connections_[new_cc].name[sizeof(connections_[new_cc].name) - 1] = '\0';
        queues_[new_cc].set_name(connections_[new_cc].name[0]
                                   ? connections_[new_cc].name : "c2-conn");

        ESP_LOGI(TAG, "  conn[%u] node[%d]->node[%d] rel=%s",
                 static_cast<unsigned>(new_cc), src_idx, dst_idx,
                 connections_[new_cc].rel);
        ++new_cc;
    }
    conn_count_ = new_cc;

    // Attach repository to each queue so new FlowFiles are persisted on push.
    // Boot-default connections have no UUID (id[0]=='\0'); those stay volatile.
    {
        IRepository* repo = repository();
        for (size_t c = 0; c < conn_count_; ++c) {
            if (connections_[c].id[0] != '\0') {
                queues_[c].attach_repo(repo, connections_[c].id);
            } else {
                queues_[c].attach_repo(nullptr, nullptr);
            }
        }
    }

    // Update the flow ID surfaced in C2 heartbeats.
    std::strncpy(flow_id_,
                 (def.flow_id[0] != '\0') ? def.flow_id
                                          : "00000000-0000-0000-0000-000000000000",
                 sizeof(flow_id_) - 1);
    flow_id_[sizeof(flow_id_) - 1] = '\0';

    produced_ = 0;
    consumed_ = 0;

    ESP_LOGI(TAG, "graph rebuilt: %u node(s), %u connection(s), flow_id=%.36s",
             static_cast<unsigned>(node_count_),
             static_cast<unsigned>(conn_count_),
             flow_id_);
}

// ---- Scheduler task ----------------------------------------------------------

void FlowEngine::task_entry(void* arg) {
    static_cast<FlowEngine*>(arg)->run_loop();
}

void FlowEngine::run_loop() {
    const TickType_t period    = pdMS_TO_TICKS(1000);
    TickType_t       last_wake = xTaskGetTickCount();

    while (true) {
        // Check for a pending flow apply from the C2 task.
        // Copy pending_def_ -> working_def_ while holding the mutex so C2
        // cannot overwrite it mid-copy.  working_def_ is a BSS member, so the
        // copy never touches the task stack.
        bool do_apply = false;
        xSemaphoreTake(mutex_, portMAX_DELAY);
        if (pending_apply_) {
            working_def_   = pending_def_;  // BSS-to-BSS struct copy
            do_apply       = true;
            pending_apply_ = false;
        }
        xSemaphoreGive(mutex_);

        // Rebuild the graph from working_def_ with the mutex released so the
        // C2 task can accept the next heartbeat response without stalling.
        if (do_apply) rebuild_from_def(working_def_);

        run_tick();
        vTaskDelayUntil(&last_wake, period);
    }
}

// ---- Per-tick execution ------------------------------------------------------

void FlowEngine::run_tick() {
    for (size_t i = 0; i < node_count_; ++i) {
        Node& n = nodes_[i];
        if (!n.active || n.desc == nullptr) continue;

        // Source node: no incoming connections -> called once per tick.
        // Sink/transform node: has incoming connections -> drain all input queues.
        bool has_incoming = false;
        for (size_t c = 0; c < conn_count_; ++c) {
            if (connections_[c].dst == static_cast<uint8_t>(i)) {
                has_incoming = true;
                break;
            }
        }

        if (!has_incoming) {
            // ---- Source ----
            Session s;
            s.reset();
            for (size_t c = 0; c < conn_count_; ++c) {
                if (connections_[c].src == static_cast<uint8_t>(i)) {
                    s.bind_relationship(connections_[c].rel, &queues_[c]);
                }
            }
            const Status rc = n.desc->on_trigger(s, n.state);
            if (rc == Status::Ok) {
                const size_t out = s.output_count();
                produced_            += out;
                n.stats.invocations  += 1;
                n.stats.ff_out       += out;
                // bytes_out for source nodes is tracked by the downstream sink
                // when it pops the FlowFile and has direct access to content_size().
            } else if (rc != Status::Again && rc != Status::Full) {
                ESP_LOGW(TAG, "node[%u] '%s' source tick: %s",
                         static_cast<unsigned>(i), n.desc->name, to_string(rc));
            }
        } else {
            // ---- Sink / transform: drain each incoming queue ----
            for (size_t c = 0; c < conn_count_; ++c) {
                if (connections_[c].dst != static_cast<uint8_t>(i)) continue;

                Queue& q = queues_[c];
                while (!q.empty()) {
                    FlowFile in;
                    if (q.try_pop(in) != Status::Ok) break;

                    Session s;
                    s.reset();
                    s.set_input(&in);
                    // Bind any outgoing connections from this node.
                    for (size_t oc = 0; oc < conn_count_; ++oc) {
                        if (connections_[oc].src == static_cast<uint8_t>(i)) {
                            s.bind_relationship(connections_[oc].rel, &queues_[oc]);
                        }
                    }
                    const Status rc = n.desc->on_trigger(s, n.state);
                    if (rc == Status::Ok) {
                        ++consumed_;
                        const size_t out = s.output_count();
                        produced_           += out;
                        n.stats.invocations += 1;
                        n.stats.ff_in       += 1;
                        n.stats.ff_out      += out;
                        n.stats.bytes_out   += in.content_size();
                        // Erase the durable record now that processing succeeded.
                        if (in.record_id() > 0) {
                            IRepository* r = repository();
                            if (r != nullptr) r->erase(in.record_id());
                        }
                    } else if (rc != Status::Again) {
                        ESP_LOGW(TAG, "node[%u] '%s' sink tick: %s",
                                 static_cast<unsigned>(i), n.desc->name, to_string(rc));
                    }
                }
            }
        }
    }
}

}  // namespace microfi
