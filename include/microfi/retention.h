// retention.h
//
// Per-connection retention policy. Determines what happens when a
// repository write would push usage past the configured high watermark.
//
// NiFi's default semantics are BackPressure: full queue stops the upstream
// processor until the queue drains. That's the right answer when the data
// source can be paused (a file ingest, an API poller).
//
// For sensor-fed flows (CSI, IMU, microphone), the source physically can't
// be paused -- packets keep arriving whether the agent is ready or not. For
// those flows, DropOldest is the right default: the agent never refuses a
// write, but the oldest already-queued FlowFile gets evicted to make room.
//
// FailWrites is a third mode useful for tests: the repository simply returns
// Status::Full on overflow and the caller deals with it. Production flows
// rarely want this.
//
// The compile-time default is chosen via Kconfig (MICROFI_RETENTION_*); a
// flow definition pushed by EFM may override the default on a per-connection
// basis. See flow_def.h for the connection-level override field (planned
// follow-up; not yet wired through the parser).

#pragma once

#include "sdkconfig.h"

#include <cstdint>

namespace microfi {

enum class RetentionPolicy : uint8_t {
    DropOldest = 0,    // FIFO; evict oldest record when high water hit
    BackPressure,      // NiFi-style; pause upstream caller (returns Full)
    FailWrites,        // hard fail on write (returns Full, no eviction)
};

constexpr const char* to_string(RetentionPolicy p) {
    switch (p) {
        case RetentionPolicy::DropOldest:   return "DropOldest";
        case RetentionPolicy::BackPressure: return "BackPressure";
        case RetentionPolicy::FailWrites:   return "FailWrites";
    }
    return "?";
}

// The compile-time default, selected via Kconfig. Used when a connection
// doesn't carry an explicit override.
constexpr RetentionPolicy default_retention_policy() {
#if defined(CONFIG_MICROFI_RETENTION_BACK_PRESSURE)
    return RetentionPolicy::BackPressure;
#elif defined(CONFIG_MICROFI_RETENTION_FAIL_WRITES)
    return RetentionPolicy::FailWrites;
#else  // CONFIG_MICROFI_RETENTION_DROP_OLDEST (default)
    return RetentionPolicy::DropOldest;
#endif
}

}  // namespace microfi
