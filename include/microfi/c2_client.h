// c2_client.h
//
// Minimal MiNiFi C2 subset (Option B from the assessment): periodic HTTPS
// heartbeat POST that carries agent id, free heap, queue depth, and the
// list of compiled-in processor names.
//
// UPDATE/configuration operations are fetched, parsed, applied, and then
// explicitly acknowledged via POST to CONFIG_MICROFI_C2_ACK_URL
// (FULLY_APPLIED on success, NOT_APPLIED with details on failure).

#pragma once

#include "microfi/types.h"

namespace microfi {

// Starts a FreeRTOS task that POSTs a heartbeat every
// CONFIG_MICROFI_HEARTBEAT_INTERVAL_MS to CONFIG_MICROFI_C2_URL. Returns
// OutOfMemory if the task cannot be created.
Status c2_client_start();

// Returns the raw flow-definition JSON most recently fetched from EFM in
// response to an UPDATE_CONFIGURATION operation. Empty string until the
// first successful fetch. Slice B (flow parser) reads this buffer.
const char* flow_config_json();
size_t      flow_config_len();

// Read-only status getters (additive, #185): diagnostics for host UIs such
// as the Brookesia agent status tile. Racy reads are acceptable.
//
// Successful heartbeat POSTs since boot.
uint64_t c2_heartbeat_count();
// Milliseconds since the last successful heartbeat POST, or -1 if none yet.
int64_t  c2_last_heartbeat_age_ms();

}  // namespace microfi
