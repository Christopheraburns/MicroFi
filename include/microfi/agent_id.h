// agent_id.h
//
// Stable per-device identifier derived from the ESP32 eFuse MAC. The agent
// identifier reported in heartbeats is "microfi-<mac-hex>" by default; the
// device identifier reported under deviceInfo.identifier is just the MAC
// in lowercase hex.
//
// CONFIG_MICROFI_AGENT_ID overrides the agent identifier when non-empty.

#pragma once

#include "microfi/types.h"

namespace microfi {

// Populate the cached identifiers from the eFuse MAC. Idempotent.
Status agent_id_init();

// "microfi-aabbccddeeff" (or the override if configured).
const char* agent_id();

// Lowercase hex MAC, no prefix. Used for deviceInfo.identifier.
const char* device_id();

// Stable per-device UUID used as MicroFi's root process-group identifier.
// EFM's Flow Designer Monitor mode matches per-processor counters by
// (processGroupId, processorId), so we report this value as:
//   - agentInfo.status.components.FlowController.uuid
//   - flowInfo.processorStatuses[].groupId
// Derived deterministically from the MAC so the same physical board always
// reports the same group id across reboots.
const char* process_group_id();

}  // namespace microfi
