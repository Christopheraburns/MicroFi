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

    // Launch the default graph and the scheduler task.
    // Returns Internal if called more than once.
    Status start();

    // Replace the running graph with the topology in `def`.
    // Thread-safe. The new graph takes effect at the next tick boundary.
    Status apply(const FlowDef& def);

    // ---- Diagnostic accessors -----------------------------------------------

    // Global throughput counters.
    uint64_t flowfiles_produced() const { return produced_; }
    uint64_t flowfiles_consumed() const { return consumed_; }
    size_t   queue_depth()        const;

    // Per-node and per-connection read-only access for heartbeat reporting.
    // Minor read/write races are acceptable -- these are metrics, not control.
    size_t            node_count()          const { return node_count_; }
    size_t            conn_