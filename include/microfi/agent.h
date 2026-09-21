// agent.h -- the whole MicroFi agent as a single callable.
//
// microfi_agent_start() is everything app_main() used to do: identity,
// manifest, storage, WiFi, flow engine, C2 client, liveness LED. The XIAO
// builds call it from a two-line app_main(); a host firmware that owns the
// board (e.g. ESP-Brookesia on the AMOLED, #188) calls the same function
// from its own startup path, typically with CONFIG_MICROFI_WIFI_ADOPT_EXISTING
// so the agent rides the host's network instead of bringing up its own.
//
// Safe to call exactly once. Returns when startup is complete (tasks keep
// running); startup failures are logged, never fatal to the caller.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void microfi_agent_start(void);

#ifdef __cplusplus
}
#endif
