// littlefs_repository.cpp -- IRepository on esp_littlefs.
//
// Record layout on disk:
//   /littlefs/<16-hex-digits>           one file per record
//
// The 16-hex-digit name is the zero-padded record id; directory listing
// gives us natural FIFO order without an external index.
//
// Eviction strategy: on every write, if used_bytes / capacity > high_water,
// step through records in oldest-first order deleting them until used_bytes
// / capacity < low_water (or the per-pass batch cap is reached). The id-
// ordered filename scheme makes "oldest" cheap: open the directory, walk
// entries, the smallest filename string is the oldest record.

#include "microfi/littlefs_repository.h"

#include "esp_littlefs.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>

namespace microfi {

namespace {

const char* TAG = "microfi.littlefs";

constexpr const char* kBasePathDefault = "/littlefs";

// Render an id as a zero-padded 16-hex-digit filename.
void format_record_name(RecordId id, char* dst, size_t dst_size) {
    std::snprintf(dst, dst_size, "%016llx", static_cast<unsigned long long>(id));
}

// Parse a 16-hex-digit record name back into a RecordId.
// Returns true on success.
bool parse_record_name(const char* name, RecordId* out) {
    if (name == nullptr || std::strlen(name) != 16) return false;
    RecordId v = 0;
    for (int i = 0; i < 16; ++i) {
        const char c = name[i];
        uint8_t nibble;
        if      (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else return false;
        v = (v << 4) | nibble;
    }
    *out = v;
    return true;
}

}  // namespace

LittleFSRepository::LittleFSRepository() {
    mtx_ = xSemaphoreCreateMutex();
}

LittleFSRepository::~LittleFSRepository() {
    if (mounted_) {
        esp_vfs_littlefs_unregister(partition_label_);
    }
    if (mtx_ != nullptr) {
        vSemaphoreDelete(mtx_);
    }
}

Status LittleFSRepository::mount() {
    if (mtx_ == nullptr) return Status::OutOfMemory;

    xSemaphoreTake(mtx_, portMAX_DELAY);
    if (mounted_) {
        xSemaphoreGive(mtx_);
        return Status::Ok;
    }

    partition_label_ = CONFIG_MICROFI_LITTLEFS_PARTITION_LABEL;
    std::snprintf(base_path_, sizeof(base_path_), "%s", kBasePathDefault);
    high_water_pct_ = CONFIG_MICROFI_LITTLEFS_HIGH_WATER_PCT;
    low_water_pct_  = CONFIG_MICROFI_LITTLEFS_LOW_WATER_PCT;
    eviction_batch_ = CONFIG_MICROFI_EVICTION_BATCH_HINT;
    policy_         = default_retention_policy();

    esp_vfs_littlefs_conf_t cfg = {};
    cfg.base_path              = base_path_;
    cfg.partition_label        = partition_label_;
#if defined(CONFIG_MICROFI_LITTLEFS_FORMAT_ON_MOUNT_FAIL)
    cfg.format_if_mount_failed = true;
#else
    cfg.format_if_mount_failed = false;
#endif
    cfg.dont_mount             = false;

    const esp_err_t err = esp_vfs_littlefs_register(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_littlefs_register failed: %s",
                 esp_err_to_name(err));
        xSemaphoreGive(mtx_);
        return Status::IoError;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(partition_label_, &total, &used) == ESP_OK) {
        capacity_bytes_ = total;
        used_bytes_     = used;
    }

    recompute_usage_locked();

    // next_id_ starts above the largest existing record id so monotonicity
    // survives reboot.
    DIR* dir = opendir(base_path_);
    if (dir != nullptr) {
        RecordId max_id = 0;
        while (struct dirent* ent = readdir(dir)) {
            RecordId id;
            if (parse_record_name(ent->d_name, &id) && id > max_id) {
                max_id = id;
            }
        }
        closedir(dir);
        next_id_ = max_id + 1;
    }

    mounted_ = true;
    xSemaphoreGive(mtx_);

    ESP_LOGI(TAG,
        "mounted %s -> %s: %llu/%llu bytes (%u records), watermarks=%u/%u",
        partition_label_, base_path_,
        static_cast<unsigned long long>(used_bytes_),
        static_cast<unsigned long long>(capacity_bytes_),
        static_cast<unsigned>(record_count_),
        static_cast<unsigned>(low_water_pct_),
        static_cast<unsigned>(high_water_pct_));

    return Status::Ok;
}

// ---- IRepository -----------------------------------------------------------

Status LittleFSRepository::write(const uint8_t* bytes, size_t len,
                                 RecordId* out_id) {
    if (!mounted_ || bytes == nullptr || out_id == nullptr) {
        return Status::InvalidArg;
    }

    xSemaphoreTake(mtx_, portMAX_DELAY);

    // Watermark check: if the incoming write would push us over the high
    // watermark, react per policy.
    const uint64_t cap = capacity_bytes_ ? capacity_bytes_ : 1;
    const uint64_t projected = used_bytes_ + len;
    const bool over_high =
        (projected * 100) > (cap * static_cast<uint64_t>(high_water_pct_));

    if (over_high) {
        switch (policy_) {
            case RetentionPolicy::BackPressure:
            case RetentionPolicy::FailWrites:
                ++failed_writes_;
                xSemaphoreGive(mtx_);
                return Status::Full;
            case RetentionPolicy::DropOldest:
                evict_oldest_locked(eviction_batch_);
                // If eviction couldn't free enough, we still attempt the
                // write; LittleFS will fail with -ENOSPC and we'll surface
                // that as IoError below.
                break;
        }
    }

    const RecordId id = next_id_++;
    char path[kPathMax];
    char name[kRecordNameLen + 1];
    format_record_name(id, name, sizeof(name));
    std::snprintf(path, sizeof(path), "%s/%s", base_path_, name);

    xSemaphoreGive(mtx_);  // file IO without holding the mutex

    FILE* fp = std::fopen(path, "wb");
    if (fp == nullptr) {
        ESP_LOGE(TAG, "fopen(%s) failed for write", path);
        return Status::IoError;
    }
    const size_t written = std::fwrite(bytes, 1, len, fp);
    std::fclose(fp);
    if (written != len) {
        ESP_LOGE(TAG, "short write to %s: %u of %u",
                 path, static_cast<unsigned>(written),
                 static_cast<unsigned>(len));
        unlink(path);
        return Status::IoError;
    }

    xSemaphoreTake(mtx_, portMAX_DELAY);
    used_bytes_  += len;
    record_count_ += 1;
    *out_id = id;
    xSemaphoreGive(mtx_);

    return Status::Ok;
}

Status LittleFSRepository::read(RecordId id, uint8_t* buf,
                                size_t* buf_size) const {
    if (!mounted_ || buf == nullptr || buf_size == nullptr) {
        return Status::InvalidArg;
    }

    char path[kPathMax];
    {
        const Status rc = path_for(id, path, sizeof(path));
        if (rc != Status::Ok) return rc;
    }

    struct stat st;
    if (::stat(path, &st) != 0) {
        return Status::NotFound;
    }
    const size_t needed = static_cast<size_t>(st.st_size);
    if (*buf_size < needed) {
        *buf_size = needed;
        return Status::Full;
    }

    FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) return Status::IoError;
    const size_t got = std::fread(buf, 1, needed, fp);
    std::fclose(fp);
    if (got != needed) return Status::IoError;

    *buf_size = needed;
    return Status::Ok;
}

Status LittleFSRepository::erase(RecordId id) {
    if (!mounted_) return Status::InvalidArg;

    char path[kPathMax];
    {
        const Status rc = path_for(id, path, sizeof(path));
        if (rc != Status::Ok) return rc;
    }

    struct stat st;
    if (::stat(path, &st) != 0) {
        // Already gone -- treat as success (idempotent erase).
        return Status::Ok;
    }
    const uint64_t freed = st.st_size;

    if (unlink(path) != 0) {
        return Status::IoError;
    }

    xSemaphoreTake(mtx_, portMAX_DELAY);
    if (used_bytes_ >= freed) used_bytes_ -= freed;
    if (record_count_ > 0)    record_count_ -= 1;
    xSemaphoreGive(mtx_);

    return Status::Ok;
}

Status LittleFSRepository::oldest(RecordId* out_id) const {
    if (!mounted_ || out_id == nullptr) return Status::InvalidArg;

    DIR* dir = opendir(base_path_);
    if (dir == nullptr) return Status::IoError;

    RecordId min_id = 0;
    bool found = false;
    while (struct dirent* ent = readdir(dir)) {
        RecordId id;
        if (!parse_record_name(ent->d_name, &id)) continue;
        if (!found || id < min_id) {
            min_id = id;
            found = true;
        }
    }
    closedir(dir);

    if (!found) return Status::NotFound;
    *out_id = min_id;
    return Status::Ok;
}

RepositoryStats LittleFSRepository::stats() const {
    xSemaphoreTake(mtx_, portMAX_DELAY);
    const uint64_t cap = capacity_bytes_ ? capacity_bytes_ : 1;
    RepositoryStats st {};
    st.capacity_bytes  = capacity_bytes_;
    st.used_bytes      = used_bytes_;
    st.record_count    = record_count_;
    st.eviction_count  = eviction_count_.load(std::memory_order_relaxed);
    st.failed_writes   = failed_writes_.load(std::memory_order_relaxed);
    st.fill_percent    = static_cast<uint8_t>((used_bytes_ * 100) / cap);
    xSemaphoreGive(mtx_);
    return st;
}

// ---- Private helpers -------------------------------------------------------

Status LittleFSRepository::path_for(RecordId id, char* dst,
                                    size_t dst_size) const {
    if (dst == nullptr || dst_size < kPathMax) return Status::InvalidArg;
    char name[kRecordNameLen + 1];
    format_record_name(id, name, sizeof(name));
    std::snprintf(dst, dst_size, "%s/%s", base_path_, name);
    return Status::Ok;
}

void LittleFSRepository::recompute_usage_locked() {
    // Use esp_littlefs_info as the authoritative used-bytes source; it
    // accounts for filesystem metadata overhead correctly. We separately
    // count records by walking the directory.
    size_t total = 0, used = 0;
    if (esp_littlefs_info(partition_label_, &total, &used) == ESP_OK) {
        capacity_bytes_ = total;
        used_bytes_     = used;
    }

    DIR* dir = opendir(base_path_);
    record_count_ = 0;
    if (dir != nullptr) {
        while (struct dirent* ent = readdir(dir)) {
            RecordId id;
            if (parse_record_name(ent->d_name, &id)) {
                ++record_count_;
            }
        }
        closedir(dir);
    }
}

uint32_t LittleFSRepository::evict_oldest_locked(uint32_t batch_hint) {
    const uint64_t cap = capacity_bytes_ ? capacity_bytes_ : 1;
    const uint64_t low_water_bytes =
        (cap * static_cast<uint64_t>(low_water_pct_)) / 100;

    uint32_t evicted = 0;
    while (used_bytes_ > low_water_bytes) {
        if (batch_hint > 0 && evicted >= batch_hint) break;

        // Find current oldest. We re-scan the directory each pass; for the
        // expected record counts (low hundreds to low thousands at the
        // 30-day default) this is cheap enough not to warrant an index.
        DIR* dir = opendir(base_path_);
        if (dir == nullptr) break;
        RecordId oldest_id = 0;
        bool found = false;
        while (struct dirent* ent = readdir(dir)) {
            RecordId id;
            if (!parse_record_name(ent->d_name, &id)) continue;
            if (!found || id < oldest_id) {
                oldest_id = id;
                found = true;
            }
        }
        closedir(dir);
        if (!found) break;

        char path[kPathMax];
        format_record_name(oldest_id, path + 0, kRecordNameLen + 1);
        // path_for would re-build; we already have the name -- compose:
        char full[kPathMax];
        std::snprintf(full, sizeof(full), "%s/%016llx",
                      base_path_, static_cast<unsigned long long>(oldest_id));

        struct stat st;
        const uint64_t bytes = (::stat(full, &st) == 0)
            ? static_cast<uint64_t>(st.st_size) : 0;

        if (unlink(full) != 0) {
            // Couldn't delete -- bail out to avoid an infinite loop.
            ESP_LOGW(TAG, "unlink(%s) failed during eviction", full);
            break;
        }

        if (used_bytes_ >= bytes) used_bytes_ -= bytes;
        if (record_count_ > 0)    record_count_ -= 1;
        ++evicted;
        ++eviction_count_;
    }

    return evicted;
}

uint64_t LittleFSRepository::capacity_bytes_locked() const {
    return capacity_bytes_;
}

// Ordered iteration: return the smallest record id strictly greater than `after`.
// Allows replay_from_repository() to walk all records oldest-first without erasing.
Status LittleFSRepository::next(RecordId after, RecordId* out_id) const {
    if (!mounted_ || out_id == nullptr) return Status::InvalidArg;

    DIR* dir = opendir(base_path_);
    if (dir == nullptr) return Status::IoError;

    RecordId best = 0;
    bool found = false;
    while (struct dirent* ent = readdir(dir)) {
        RecordId id;
        if (!parse_record_name(ent->d_name, &id)) continue;
        if (id > after && (!found || id < best)) {
            best  = id;
            found = true;
        }
    }
    closedir(dir);

    if (!found) return Status::NotFound;
    *out_id = best;
    return Status::Ok;
}

}  // namespace microfi
