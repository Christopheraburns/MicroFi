// flow_engine.h -- dynamic-graph flow scheduler.
//
// Boot behaviour: start() wires the hard-coded default graph
// (GenerateFlowFile -> LogAttribute) so there is something running before EFM
// pushes its first flow definition.
//
// Runtime update: apply(FlowDef) is called by the C2 task after parsing an
// UPDATE_CONFIGURATION. It stores the FlowDef into pending_def_ (under mutex)
// and sets pending_apply_.  run_loop() copies pending_def_ into working_def_
// (still under mutex -- safe, no stack copy) at the next tick boundary, then
// calls rebuild_from_def(working_def_) with the mutex released.
//
// Memory layout note:
//   Connection is a plain-old-data struct -- it carries no Queue.
//   Queue objects live in queues_[], indexed by connection slot.
//   This avoids copying semaphore handles when rebuilding the graph.
//
// Thread-safety contract:
//   apply()  -- may be called from any task (uses mutex_)
//   all other methods -- called from the single engine task

#pragma once

#include "microfi/flow_def.h"
#include "microfi/processor.h"
#include "microfi/queue.h"
#include "microfi/types.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstddef>
#include <cstdint>

namespace microfi {

class FlowEngine {
public:
    static FlowEngine& instance();

    // ---- Public type definitions (needed by heartbeat reporter) -------------

    static constexpr size_t kMaxNodes       = kMaxFlowNodes;
    static constexpr size_t kMaxConnections = kMaxFlowConnections;
    static constexpr size_t kStateBytes     = 256;

    // Per-node throughput counters, reset on each graph rebuild.
    struct NodeStats {
        uint64_t invocations;   // on_trigger call count
        uint64_t ff_in;         // FlowFiles consumed (sink/transform nodes)
        uint64_t ff_out;        // FlowFiles produced (all nodes)
        uint64_t bytes_out;     // content bytes in outgoing FlowFiles
    };

    struct Node {
        const ProcessorDescriptor* desc;
        alignas(8) uint8_t state[kStateBytes];
        char         id[37];
        NodeProperty props[kMaxNodeProperties];
        size_t       prop_count;
        bool         active;
        NodeStats    stats;
    };

    // Connection is POD -- no embedded Queue.
    // The queue for connection slot c lives in queues_[c].
    struct Connection {
        uint8_t src;      // index into nodes_
        uint8_t dst;      // index into nodes_
        char    rel[32];  // relationship name (e.g. "success")
        char    id[37];   // connection UUID from EFM (keys flowInfo.queues)
        char    name[64]; // display label, e.g. "GenerateFlowFile/success/LogAttribute"
    };

    // ---- Lifecycle ----------------------------------------------------------

    // Apply a flow definition synchronously BEFORE the scheduler task starts.
    // Use from app_main after storage_init() and before start() to prime the
    // engine with a saved flow definition; the first heartbeat will then carry
    // the correct flowId rather than the zero UUID.
    // Returns Internal if start() has already been called.
    Status prime(const FlowDef& def);

    // Launch the scheduler task.  If prime() was called the engine starts with
    // that graph; otherwise the hard-wired boot-default (GenerateFlowFile ->
    // LogAttribute) is installed first.  Returns Internal if called twice.
    Status start();

    // Replace the running graph with the topology in `def`.
    // Thread-safe. The new graph takes effect at the next tick boundary.
    Status apply(const FlowDef& def);

    // Scan the IRepository for persisted FlowFile records and push them into
    // the matching connection queues.  Must be called after prime()/start()
    // (so the connection UUIDs are known) and before c2_client_start().
    // No-op if no repository is mounted or the repository is empty.
    void replay_from_repository();

    // ---- Diagnostic accessors -----------------------------------------------

    // Global throughput counters.
    uint64_t flowfiles_produced() const { return produced_; }
    uint64_t flowfiles_consumed() const { return consumed_; }
    size_t   queue_depth()        const;

    // Per-node and per-connection read-only access for heartbeat reporting.
    // Minor read/write races are acceptable -- these are metrics, not control.
    size_t            node_count()          const { return node_count_; }
    size_t            conn_count()          const { return conn_count_; }
    const Node&       node(size_t i)        const { return nodes_[i]; }
    const Connection& conn(size_t i)        const { return connections_[i]; }
    size_t            conn_queue_size(size_t i) const { return queues_[i].size(); }

    // Current flow ID: zero UUID on boot, updated by each successful apply().
    // Included in heartbeat flowInfo.flowId so EFM can confirm the update landed.
    const char* flow_id() const { return flow_id_; }

    // Next monotonic FlowFile id.
    uint64_t next_id();

private:
    FlowEngine();
    ~FlowEngine();

    static void task_entry(void* arg);
    void run_loop();
    void rebuild_from_def(const FlowDef& def);
    void run_tick();

    Node        nodes_[kMaxNodes]             = {};
    Connection  connections_[kMaxConnections] = {};
    Queue       queues_[kMaxConnections];       // one Queue per connection slot
    size_t      node_count_  = 0;
    size_t      conn_count_  = 0;
    char        flow_id_[37] = "00000000-0000-0000-0000-000000000000";

    // Double-buffer for pending flow apply.
    //   pending_def_  -- written by C2 task under mutex_
    //   working_def_  -- copied from pending_def_ by engine task (under mutex),
    //                    then used by rebuild_from_def() without holding mutex.
    //   Both are members (BSS), not stack locals, so they never overflow a task.
    FlowDef           pending_def_   = {};
    FlowDef           working_def_   = {};
    bool              pending_apply_ = false;
    SemaphoreHandle_t mutex_         = nullptr;

    TaskHandle_t task_    = nullptr;
    bool         started_ = false;

    uint64_t produced_ = 0;
    uint64_t consumed_ = 0;
    uint64_t next_id_  = 1;
};

}  // namespace microfi
