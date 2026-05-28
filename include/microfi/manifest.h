// manifest.h
//
// The agent manifest describes the agent's capabilities: agent type,
// version, build info, processor catalog (drawn from the static registry),
// scheduling defaults, and supported C2 operations.
//
// EFM uses the manifest to know which flows are valid for this device.
// Pushing the full manifest is expensive (kilobytes); pushing just the
// hash is cheap. We compute both at boot, then send the full manifest
// only on the first heartbeat or when EFM explicitly asks for it via a
// DESCRIBE/manifest operation.

#pragma once

#include "microfi/types.h"

#include <cstddef>

namespace microfi {

// Build the manifest JSON from the static registry and compute its
// SHA-256 hex hash. Idempotent. Allocates the manifest string on the heap
// once; the pointer is valid until reboot.
Status manifest_init();

// NUL-terminated JSON; never null after a successful manifest_init().
const char* manifest_json();
size_t      manifest_json_len();

// 64-character lowercase hex SHA-256 of manifest_json().
const char* manifest_hash();

}  // namespace microfi
