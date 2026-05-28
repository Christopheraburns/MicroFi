// main.cpp -- MicroFi entry point.
//
// app_main runs once when ESP-IDF finishes its own boot. We:
//   1. Initialize the agent identifier from the eFuse MAC.
//   2. Build the agent manifest from the static registry (+ its hash).
//   3. Log the registered processors (sanity check on the static registry).
//   4. Bring up WiFi.
//   5. Start the flow engine (hard-wired GenerateFlowFile -> LogAttribute).
//   6. Start the C2 heartbeat client (EFM 2.x envelope).
// Then app_main returns and FreeRTOS continues running our tasks.

#include "microfi/agent_id.h"
#include "microfi/c2_client.h"
#include "microfi/flow_engine.h"
#include "microfi/manifest.h"
#include "microfi/registry.h"
#include "microfi/storage.h"
#include "microfi/types.h"
#include "microfi/wifi.h"

#include "esp_log.h"
#include "sdkconfig.h"

static const char* TAG = "microfi";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "MicroFi 0.1.0 starting");

    if (microfi::agent_id_init() != microfi::Status::Ok) {
        ESP_LOGE(TAG, "agent_id init failed");
        return;
    }
    if (microfi::manifest_init() != microfi::Status::Ok) {
        ESP_LOGE(TAG, "manifest init failed");
        return;
    }

    // Storage subsystem: mount LittleFS so the engine has a durable
    // repository available. Non-fatal on failure -- the agent continues in
    // volatile-only mode (FlowFiles will not survive reboot) and the
    // condition is loud-logged for diagnosis.
    const microfi::Status storage_rc = microfi::storage_init();
    if (storage_rc != microfi::Status::Ok) {
        ESP_LOGW(TAG, "storage init failed: %s -- volatile-only mode",
                 microfi::to_string(storage_rc));
    }

    auto& reg = microfi::Registry::instance();
    ESP_LOGI(TAG, "%u processor(s) registered:",
             static_cast<unsigned>(reg.count()));
    for (size_t i = 0; i < reg.count(); ++i) {
        const auto* d = reg.at(i);
        ESP_LOGI(TAG, "  - %s -- %s", d->name, d->description);
    }
    ESP_LOGI(TAG, "agent_id=%s class=%s manifest_hash=%s",
             microfi::agent_id(),
             CONFIG_MICROFI_AGENT_CLASS,
             microfi::manifest_hash());

    const microfi::Status wifi_rc = microfi::wifi_start_and_wait(20000);
    if (wifi_rc != microfi::Status::Ok) {
        ESP_LOGW(TAG, "wifi not up (%s); continuing -- engine will run, "
                      "heartbeats will be skipped until link comes up",
                 microfi::to_string(wifi_rc));
    }

    const microfi::Status engine_rc = microfi::F