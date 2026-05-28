// wifi.h
//
// Bring up the WiFi station and block until we have an IP. The first slice
// only needs station mode; provisioning, scan, and AP fallback can land
// later. Credentials come from Kconfig (see src/Kconfig.projbuild).

#pragma once

#include "microfi/types.h"

#include <cstdint>

namespace microfi {

// Blocks for at most `timeout_ms` waiting for an IP. Returns Ok on success,
// IoError on connect timeout. Safe to call exactly once at boot.
Status wifi_start_and_wait(uint32_t timeout_ms);

bool wifi_connected();

}  // namespace microfi
