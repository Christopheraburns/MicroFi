// flow_parser.h -- parse an EFM C2 configContent JSON body into a FlowDef.
//
// The configContent endpoint returns a NiFi versioned-flow-snapshot:
//   {
//     "flowContents": {
//       "identifier": "<flow-uuid>",
//       "processors": [ { "identifier": "...", "type": "...", "properties": {...} } ],
//       "connections": [ { "source": {"id":"..."}, "destination": {"id":"..."},
//                          "selectedRelationships": ["success"] } ]
//     }
//   }
//
// flow_parse() handles the common wrapper variants ("flowContents", "content",
// or no wrapper) so we're robust against minor EFM version differences.

#pragma once

#include "microfi/flow_def.h"
#include "microfi/types.h"

namespace microfi {

// Parse the null-terminated body returned by EFM's configContent / flows
// endpoint into `out`.  The format is detected automatically:
//
//   "MiNiFi Config Version: 3" header → MiNiFi YAML v3 (flow_yaml_parse)
//   '{' first character              → NiFi versioned-flow-snapshot JSON
//
// Returns Status::Ok on success, Status::ParseError on failure.
// On ParseError, `out` is in an indeterminate state and must not be used.
Status flow_parse(const char* body, FlowDef& out);

// Parse a MiNiFi Config Version 3 YAML body into `out`.
// Called automatically by flow_parse(); also callable directly for testing.
Status flow_yaml_parse(const char* yaml, FlowDef& out);

}  // namespace microfi
