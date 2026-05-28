// session.h
//
// A Session is the unit of work a processor sees on each on_trigger call.
// It exposes (a) the input FlowFile, if any, and (b) the set of outbound
// relationships the processor can route to.
//
// In the upstream NiFi/MiNiFi design, sessions buffer changes and commit
// atomically. For the first MicroFi slice we simplify aggressively:
//   - At most one input FlowFile per trigger.
//   - At most one output FlowFile per trigger.
//   - Routing is by relationship name; the engine maps relationship names
//     to outbound queues at flow-load time.
// The full commit/rollback semantics land in a later slice.

#pragma once

#include "microfi/flowfile.h"
#include "microfi/queue.h"
#include "microfi/types.h"

namespace microfi {

constexpr size_t kMaxRelationships = 4;
constexpr size_t kMaxRelationshipNameLen = 32;

class Session {
public:
    Session() = default;

    // Wire up before invoking the processor. The engine fills these in.
    void reset();

    void set_input(const FlowFile* in) { input_ = in; }
    const FlowFile* input() const { return input_; }

    // Bind a relationship name to a queue that the processor can transfer to.
    // Returns Full if we have already bound kMaxRelationships.
    Status bind_relationship(const char* name, Queue* q);

    // Called by processors. Looks up `relationship` in the bound table and
    // pushes `f` to the associated queue.
    Status transfer(const FlowFile& f, const char* relationship);

    // For diagnostics -- how many outbound FlowFiles the processor produced
    // during this trigger.
    size_t output_count() const { return n_outputs_; }

private:
    struct Binding {
        char  name[kMaxRelationshipNameLen] = {0};
        Queue* queue = nullptr;
    };

    const FlowFile* input_     = nullptr;
    size_t          n_bound_   = 0;
    size_t          n_outputs_ = 0;
    Binding         bindings_[kMaxRelationships] = {};
};

}  // namespace microfi
