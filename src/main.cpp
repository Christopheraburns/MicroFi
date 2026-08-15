// main.cpp -- MicroFi entry point.
//
// app_main runs once when ESP-IDF finishes its own boot. We:
//   1. Initialize the agent identifier from the eFuse MAC.
//   2. Build the agent manifest from the static registry (+ its hash).
//   3. Log the registered processors (sanity check on the static registry).
//   4. Bring up WiFi.
//   5. Mount storage (LittleFS); load saved flow def if present and prime engine.
//   6. Start the flow engine; if primed it skips the boot-default graph.
//   7. Replay persisted FlowFiles from IRepository into connection queues.
//   8. Start the C2 heartbeat client (EFM 2.x envelope).
// Then app_main returns and FreeRTOS continues running our tasks.

#include "microfi/agent_id.h"
#include "microfi/c2_client.h"
#include "microfi/liveness_led.h"
#include "microfi/flow_def.h"
#include "microfi/flow_engine.h"
#include "microfi/flow_parser.h"
#include "microfi/flow_store.h"
#include "microfi/manifest.h"
#include "microfi/registry.h"
#include "microfi/storage.h"
#include "microfi/types.h"
#include "microfi/wifi.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include <cstdlib>

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
    // volatile-only mode (FlowFiles will not survive reboot).
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
        ESP_LOGW(TAG, "wifi not up (%s); continuing -- heartbeats skipped until link comes up",
                 microfi::to_string(wifi_rc));
    }

    // If storage came up, try to restore the last known flow definition so the
    // engine starts with the right graph and the first heartbeat carries the
    // correct flowId (avoiding another UPDATE/configuration round-trip).
    // Heap-allocate the parse buffer: kFlowDefMaxBytes (16 KB) is too large
    // for the main_task stack or a static BSS slot.
    if (storage_rc == microfi::Status::Ok) {
        char* flow_buf = static_cast<char*>(std::malloc(microfi::kFlowDefMaxBytes));
        if (flow_buf == nullptr) {
            ESP_LOGW(TAG, "flow_buf alloc failed -- boot default");
        } else {
            size_t flow_len = 0;
            const microfi::Status load_rc =
                microfi::flow_def_load(flow_buf, microfi::kFlowDefMaxBytes, &flow_len);
            if (load_rc == microfi::Status::Ok && flow_len > 0) {
                microfi::FlowDef def{};
                const microfi::Status parse_rc =
                    microfi::flow_parse(flow_buf, def);
                if (parse_rc == microfi::Status::Ok && def.node_count > 0) {
                    // YAML carries no UUID; restore it from the sidecar file.
                    if (def.flow_id[0] == '\0') {
                        microfi::flow_id_load(def.flow_id);
                    }
                    const microfi::Status prime_rc =
                        microfi::FlowEngine::instance().prime(def);
                    if (prime_rc == microfi::Status::Ok) {
                        ESP_LOGI(TAG, "engine primed from saved flow def (flow_id=%.36s)",
                                 def.flow_id);
                    } else {
                        ESP_LOGW(TAG, "prime failed (%s) -- boot default",
                                 microfi::to_string(prime_rc));
                    }
                } else if (parse_rc != microfi::Status::Ok) {
                    ESP_LOGW(TAG, "saved flow def parse failed (%s) -- boot default",
                             microfi::to_string(parse_rc));
                }
            } else if (load_rc != microfi::Status::NotFound) {
                ESP_LOGW(TAG, "flow_def_load: %s", microfi::to_string(load_rc));
            }
            std::free(flow_buf);
        }
    }

    const microfi::Status engine_rc = microfi::FlowEngine::instance().start();
    if (engine_rc != microfi::Status::Ok) {
        ESP_LOGE(TAG, "engine failed to start: %s",
                 microfi::to_string(engine_rc));
        return;
    }

    // Replay any FlowFiles that were in-flight before the last power cycle.
    microfi::FlowEngine::instance().replay_from_repository();

    const microfi::Status c2_rc = microfi::c2_client_start();
    if (c2_rc != microfi::Status::Ok) {
        ESP_LOGE(TAG, "c2 client failed to start: %s",
                 microfi::to_string(c2_rc));
        return;
    }

    // Liveness strobe last: past every fatal-init return above, so a blinking
    // LED means the agent is genuinely up. Failure is a warn, never fatal.
    const microfi::Status led_rc = microfi::liveness_led_start();
    if (led_rc != microfi::Status::Ok) {
        ESP_LOGW(TAG, "liveness LED failed to start: %s",
                 microfi::to_string(led_rc));
    }

    ESP_LOGI(TAG, "boot complete; heartbeating to %s",
             CONFIG_MICROFI_C2_HEARTBEAT_URL);
}
