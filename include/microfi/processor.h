// processor.h
//
// A Processor in MicroFi is a static descriptor record plus a function
// pointer. There are no Processor *instances* until the flow engine
// allocates per-node state from a fixed slab.
//
// The descriptor is registered at static-init time via the
// MICROFI_REGISTER_PROCESSOR macro (see registry.h). Each compiled-in
// processor source file declares a descriptor like this:
//
//   namespace microfi { namespace gen {
//     static Status on_trigger(Session& s, void* state) { ... }
//     ProcessorDescriptor descriptor = {
//       .name        = "GenerateFlowFile",
//       .description = "Emits a fixed-content FlowFile on each tick.",
//       .on_trigger  = on_trigger,
//       .state_size  = sizeof(MyState),
//     };
//   } }
//   MICROFI_REGISTER_PROCESSOR(microfi::gen::descriptor)
//
// The capability list reported in the C2 heartbeat is derived from the
// registry: pushing a flow that references an unknown processor name is
// rejected at flow-apply time.

#pragma once

#include "microfi/flow_def.h"   // NodeProperty
#include "microfi/session.h"
#include "microfi/types.h"

#include <cstddef>

namespace microfi {

// One entry in a property's constrained value list. Both fields are required;
// set display_name to null to fall back to value in the EFM palette.
struct AllowableValue {
    const char* value;        // wire value used in flow definitions
    const char* display_name; // human-readable label; null → same as value
};

// Describes a single configurable property on a processor. Property names
// are matched case-sensitively against the names used in MiNiFi C++ so that
// flow definitions designed against a full MiNiFi agent work without edits on
// MicroFi agents of the same class.
//
// Fields mirror the EFM manifest schema:
//   name           -- unique key within this processor; used as the map key
//                     in the serialized "propertyDescriptors" object.
//   description    -- shown in the EFM Flow Designer property tooltip.
//   default_value  -- pre-filled value when a new processor is dropped onto
//                     the canvas. May be null for optional free-text fields.
//   required       -- if true, EFM will require the operator to provide a
//                     value before publishing the flow.
//   allowable_values / allowable_count
//                  -- if non-null, EFM renders a dropdown instead of a text
//                     box. Null means free-text input.
struct PropertyDescriptor {
    const char*           name;
    const char*           description;
    const char*           default_value;
    bool                  required;
    const AllowableValue* allowable_values;
    size_t                allowable_count;
};

struct ProcessorDescriptor {
    const char* name;
    const char* description;

    // Called once per scheduling tick that the engine assigns to this node.
    // `state` is a pointer to per-instance memory of size `state_size`,
    // zero-initialized the first time the engine sees the node.
    Status (*on_trigger)(Session& session, void* state);

    // Optional one-time init at flow-apply time. Called before on_configure.
    // May be null.
    Status (*on_init)(void* state);

    // Optional property configuration, called once after on_init with the
    // key-value pairs from the EFM flow definition.  Use this to read
    // properties into typed fields in `state` so on_trigger doesn't have to
    // do string comparisons on every tick.  May be null.
    void (*on_configure)(void* state, const NodeProperty* props, size_t count);

    // Size of per-instance state. May be 0 for stateless processors.
    size_t state_size;

    // C2 manifest: how EFM's Flow Designer draws connection ports.
    //   "INPUT_FORBIDDEN" -- source processor; no incoming connections allowed.
    //   "INPUT_REQUIRED"  -- must have at least one incoming connection.
    //   "INPUT_ALLOWED"   -- optional incoming connections (default).
    // Null is treated as "INPUT_ALLOWED".
    const char* input_requirement;

    // Optional property declarations, serialized into the C2 manifest so EFM
    // can render them in the Flow Designe