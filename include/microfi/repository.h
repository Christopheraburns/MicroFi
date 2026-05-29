// repository.h
//
// Abstract durable byte-blob store used by MicroFi as the substrate for
// FlowFile / Content persistence. Concrete backends:
//
//     LittleFSRepository   internal flash, crash-safe, ~12 MB on a 16 MB part
//     SdRepository         SD card via FATFS (Phase 2; not yet implemented)
//     TieredRepository     LittleFS-as-tip + SD-as-overflow (Phase 2)
//
// The repository is intentionally generic over what it stores -- a "record"
// is just an opaque variable-length byte buffer with a monotonic record id.
// FlowFile serialization is layered on top in storage.cpp; the repository
// itself knows nothing about FlowFile structure.
//
// Records are FIFO-ordered by insertion time (record_id is monotonically
// increasing). The retention policy and watermark thresholds are owned by
// the concrete backend, configured via Kconfig.
//
// Thread-safety: each repository instance must be safe for concurrent use
// from the engine task and the C2 task. Concrete backends use FreeRTOS
// mutexes internally.

#pragma once

#include "microfi/retention.h"
#include "microfi/types.h"

#include <cstddef>
#include <cstdint>

namespace microfi {

// Monotonic record identifier, assigned by the repository on write.
using RecordId = uint64_t;

// Snapshot of repository utilisation, suitable for heartbeat reporting.
struct RepositoryStats {
    uint64_t capacity_bytes;     // total usable capacity (after FS overhead)
    uint64_t used_bytes;         // current bytes consumed by records
    uint32_t record_count;       // number of records currently stored
    uint32_t eviction_count;     // cumulative evictions since boot
    uint32_t failed_writes;      // writes refused under FailWrites / Full
    uint8_t  fill_percent;       // used_bytes * 100 / capacity_bytes
};

class IRepository {
public:
    virtual ~IRepository() = default;

    // ---- Write / read / erase --------------------------------------------

    // Append a new record. On success, *out_id is set to the assigned
    // monotonic record id. The semantics on Full depend on the configured
    // RetentionPolicy:
    //
    //   DropOldest    evicts oldest records until the new write fits,
    //                 returns Ok
    //   BackPressure  returns Full; caller (e.g. an upstream processor)
    //                 should back off and retry
    //   FailWrites    returns Full; record is not stored
    virtual Status write(const uint8_t* bytes, size_t len, RecordId* out_id) = 0;

    // Read a previously-written record into the caller's buffer.
    //   buf_size  in: caller buffer capacity
    //             out: actual bytes written (on Ok) or required size (on Full)
    // Returns NotFound if id was never written or has since been evicted.
    virtual Status read(RecordId id, uint8_t* buf, size_t* buf_size) const = 0;

    // Erase a specific record (e.g. after successful downstream ack).
    // No-op if the record is already gone; returns Ok in that case.
    virtual Status erase(RecordId id) = 0;

    // ---- Iteration --------------------------------------------------------

    // Get the oldest currently-stored record id. Returns NotFound if the
    // repository is empty. Used by the engine to replay FlowFiles after a
    // reboot and by the eviction logic to find the next victim.
    virtual Status oldest(RecordId* out_id) const = 0;

    // Get the record id immediately following `after` in insertion order.
    // Returns NotFound if `after` is the newest record or the repository is
    // empty. Used by replay_from_repository() to iterate all records without
    // erasing them. Backends that do not support ordered iteration return
    // NotImplemented; callers must fall back gracefully.
    virtual Status next(RecordId after, RecordId* out_id) const {
        (void)after; (void)out_id;
        return Status::NotImplemented;
    }


    // ---- Configuration ----------------------------------------------------

    virtual RetentionPolicy retention_policy() const = 0;
    virtual void set_retention_policy(RetentionPolicy p) = 0;

    // ---- Observability ----------------------------------------------------

    virtual RepositoryStats stats() const = 0;
};

}  // namespace microfi
