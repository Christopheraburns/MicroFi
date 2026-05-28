// storage.cpp -- top-level storage subsystem.
//
// app_main calls storage_init() once. The selected backend mounts; the
// singleton is exposed via repository().
//
// On SD overflow (MICROFI_SD_OVERFLOW=y) we currently warn and fall back to
// LittleFS-only: the Sd / Tiered backends are a Phase 2 deliverable, and the
// Kconfig is in place ahead of the implementation so users can audit the
// build-time surface before the code lands.

#include "microfi/storage.h"

#include "microfi/littlefs_repository.h"

#include "esp_log.h"
#include "sdkconfig.h"

namespace microfi {

namespace {

const char* TAG = "microfi.storage";

// File-scope singleton. Lifetime is the lifetime of the process.
LittleFSRepository  s_littlefs;
IRepository*        s_active = nullptr;
bool                s_initialised = false;

}  // namespace

Status storage_init() {
    if (s_initialised) {
        return Status::Ok;
    }

#if defined(CONFIG_MICROFI_SD_OVERFLOW)
    ESP_LOGW(TAG,
        "MICROFI_SD_OVERFLOW=y but SdRepository / TieredRepository are not "
        "yet implemented; falling back to LittleFS-only for this build.");
#endif

    const Status mount_rc = s_littlefs.mount();
    if (mount_rc != Status::Ok) {
        ESP_LOGE(TAG,
            "LittleFS mount failed: %s -- agent continuing in volatile-only "
            "mode (FlowFiles will not survive reboot).",
            to_string(mount_rc));
        s_active = nullptr;
        s_initialised = true;  // mark complete so we don't keep re-trying
        return mount_rc;
    }

    s_active = &s_littlefs;
    s_initialised = true;

    const RepositoryStats st = s_active->stats();
    ESP_LOGI(TAG,
        "LittleFS repository mounted: %llu / %llu bytes (%u%%), "
        "%u record(s), policy=%s",
        static_cast<unsigned long long>(st.used_bytes),
        static_cast<unsigned long long>(st.capacity_bytes),
        static_cast<unsigned>(st.fill_percent),
        static_cast<unsigned>(st.record_count),
        to_string(s_active->retention_policy()));

    return Status::Ok;
}

IRepository* repository() {
    return s_active;
}

}  // namespace microfi
