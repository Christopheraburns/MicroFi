// littlefs_repository.h
//
// IRepository backed by an esp_littlefs mount. Each record lives in its own
// file under a flat directory, named by zero-padded record id so the
// directory entries iterate in insertion order. This is the simplest viable
// layout; a ring-buffer-style single-file backend would be more efficient
// but is not justified at the current data rates.
//
// Watermark eviction: writes that would push usage above the configured
// high watermark trigger an eviction pass that deletes oldest-first until
// usage drops below the low watermark (or, if MICROFI_EVICTION_BATCH_HINT
// is set, until N records have been evicted).
//
// Thread-safety: a FreeRTOS mutex guards all mutation paths and the stats
// snapshot. Single instance, owned by storage.cpp.

#pragma once

#include "microfi/repository.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace microfi {

class LittleFSRepository : public IRepository {
public:
    LittleFSRepository();
    ~LittleFSRepository() override;

    // Mount the underlying partition. Returns Ok on success; on first boot
    // the partition is empty and esp_littlefs auto-formats if the Kconfig
    // flag MICROFI_LITTLEFS_FORMAT_ON_MOUNT_FAIL is enabled.
    Status mount();

    // ---- IRepository --------------------------------------------------------

    Status write(const uint8_t* bytes, size_t len, RecordId* out_id) override;
    Status read(RecordId id, uint8_t* buf, size_t* buf_size) const override;
    Status erase(RecordId id) override;
    Status oldest(RecordId* out_id) const override;
    Status next(RecordId after, RecordId* out_id) const override;

    RetentionPolicy retention_policy() const override { return policy_; }
    void set_retention_policy(RetentionPolicy p) override { policy_ = p; }

    RepositoryStats stats() const override;

private:
    // Resolve a record id to a full filesystem path under base_path_.
    // dst must be at least kPathMax bytes. Returns Ok on success.
    Status path_for(RecordId id, char* dst, size_t dst_size) const;

    // Refresh used_bytes_ and record_count_ from the filesystem. Called from
    // mount() and periodically (every Nth write) to recover from drift if
    // any path slips through the manual accounting. Caller must hold mtx_.
    void recompute_usage_locked();

    // Evict oldest records until usage drops below low watermark, or until
    // batch_hint records have been evicted (if batch_hint > 0). Caller
    // must hold mtx_. Returns the number of records actually evicted.
    uint32_t evict_oldest_locked(uint32_t batch_hint);

    // Filesystem usage in bytes against the partition capacity. Caller
    // must hold mtx_.
    uint64_t capacity_bytes_locked() const;

    // Constants
    static constexpr size_t kPathMax       = 64;
    static constexpr size_t kRecordNameLen = 16;  // 16 hex digits = 64-bit id

    // Configuration (cached from Kconfig at mount time)
    const char*     partition_label_   = nullptr;
    char            base_path_[16]     = {0};   // "/littlefs"
    uint8_t         high_water_pct_    = 80;
    uint8_t         low_water_pct_     = 70;
    uint32_t        eviction_batch_    = 0;

    // State (guarded by mtx_)
    mutable SemaphoreHandle_t mtx_       = nullptr;
    RetentionPolicy policy_              = RetentionPolicy::DropOldest;
    RecordId        next_id_             = 1;
    uint64_t        capacity_bytes_      = 0;
    uint64_t        used_bytes_          = 0;
    uint32_t        record_count_        = 0;
    bool            mounted_             = false;

    // Lock-free counters; safe to read from any task without holding mtx_.
    std::atomic<uint32_t> eviction_count_ {0};
    std::atomic<uint32_t> failed_writes_  {0};
};

}  // namespace microfi
