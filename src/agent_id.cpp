// agent_id.cpp -- MAC-derived identifiers.

#include "microfi/agent_id.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>

namespace microfi {

namespace {

const char* TAG = "microfi.agentid";

// "microfi-" + 12 hex chars + NUL = 21 bytes; round up.
char s_agent_id[32]         = {0};
char s_device_id[16]        = {0};
char s_process_group_id[37] = {0};   // 36-char UUID + NUL
bool s_initialised          = false;

}  // namespace

Status agent_id_init() {
    if (s_initialised) return Status::Ok;

    uint8_t mac[6] = {0};
    const esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_efuse_mac_get_default failed: %s",
                 esp_err_to_name(err));
        return Status::Internal;
    }

    std::snprintf(s_device_id, sizeof(s_device_id),
                  "%02x%02x%02x%02x%02x%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (CONFIG_MICROFI_AGENT_ID[0] != '\0') {
        std::strncpy(s_agent_id, CONFIG_MICROFI_AGENT_ID,
                     sizeof(s_agent_id) - 1);
    } else {
        std::snprintf(s_agent_id, sizeof(s_agent_id),
                      "microfi-%s", s_device_id);
    }

    // Synthesize a stable RFC-4122-shaped UUID for the root process group.
    // Version nibble (4xxx) and variant nibble (8xxx) are fixed; the rest is
    // a deterministic function of the MAC so the same board always reports
    // the same group id.  EFM uses this in processorStatuses[].groupId.
    std::snprintf(s_process_group_id, sizeof(s_process_group_id),
                  "%02x%02x%02x%02x-%02x%02x-4000-8000-%02x%02x%02x%02x%02x%02x",
                  mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5],
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "agent_id='%s' device_id='%s' group_id='%s'",
             s_agent_id, s_device_id, s_process_group_id);
    s_initialised = true;
    return Status::Ok;
}

const char* agent_id()         { return s_agent_id; }
const char* device_id()        { return s_device_id; }
const char* process_group_id() { return s_process_group_id; }

}  // namespace microfi
