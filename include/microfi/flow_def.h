// flow_def.h -- Parsed representation of a flow definition pushed by EFM.
//
// FlowDef is the output of flow_parser.cpp and the input to FlowEngine::apply().
// All storage is fixed-size (no heap allocation inside the struct).
//
// Limits are set conservatively for an ESP32 that processes simple edge flows.
// Raise them in sdkconfig if a richer topology is needed later.
//
// Typical EFM flow for MicroFi:
//   flowContents.processors[]   -> FlowNode[]
//   flowContents.connections[]  -> FlowConnection[]

#pragma once

#include <cstddef>

namespace microfi {

// Limits sized for realistic ESP32 edge flows (2-4 processors).
// Each Queue slot is ~10 KB of DRAM; kMaxFlowConnections controls how many
// Queue objects live in the engine's BSS.  Raise with care.
constexpr size_t kMaxFlowNodes       = 4;
constexpr size_t kMaxFlowConnections = 4;
constexpr size_t kMaxNodeProperties  = 8;

// One configured property on a processor node (key-value pair from EFM).
struct NodeProperty {
    char key[48];
    char value[64];
};

// A single processor node as described in the EFM flow definition.
//   id           -- UUID assigned by EFM (36 chars, e.g. "550e8400-e29b-41d4-...")
//   type         -- short processor type name, e.g. "GenerateFlowFile"
//                   (package prefix stripped by the parser)
//   properties   -- configured property values from EFM's Flow Designer
struct FlowNode {
    char         id[37];           // UUID + NUL
    char         type[64];         // short type name
    NodeProperty properties[kMaxNodeProperties];
    size_t       property_count;
};

// A directed edge between two processor nodes.
//   id           -- UUID assigned by EFM; used to key flowInfo.queues in heartbeat
//   name         -- display label, e.g. "GenerateFlowFile/success/LogAttribute"
//   src_id / dst_id   -- match FlowNode.id in the same FlowDef
//   relationship      -- e.g. "success", "failure"
struct FlowConnection {
    char id[37];              // connection UUID (YAML `id:` field)
    char name[64];            // human label     (YAML `name:` field)
    char src_id[37];
    char dst_id[37];
    char relationship[32];
};

// A fully parsed flow definition ready to hand to FlowEngine::apply().
struct FlowDef {
    char           flow_id[37];                        // flow UUID or all-zeros
    FlowNode       nodes[kMaxFlowNodes];
    size_t         node_count;
    FlowConnection connections[kMaxFlowConnections];
    size_t         connection_count;
};

}  // namespace microfi
