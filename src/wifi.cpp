// wifi.cpp -- standard ESP-IDF WiFi station bring-up.
//
// We use an event group to block on connect: GOT_IP_BIT means we have an
// address, FAIL_BIT means the connect attempt failed enough times to give
// up during the initial connection phase.
//
// Reconnection strategy (two phases):
//   Initial phase: retry up to kMaxAttempts; give up and set FAIL_BIT if
//     all attempts fail so wifi_start_and_wait() can return an error.
//   Persistent phase: once we have ever received an IP (s_ever_connected),
//     always call esp_wifi_connect() on disconnect — no cap — so the device
//     recovers silently from beacon timeouts and transient AP unavailability.
//     A short back-off delay is NOT applied here (the WiFi driver already
//     uses its own probe-request retry timing); adding a FreeRTOS delay
//     inside an event-loop callback would block the event task.

#include "microfi/wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <cstring>

namespace microfi {

namespace {

constexpr int GOT_IP_BIT  = BIT0;
constexpr int FAIL_BIT    = BIT1;
// kMaxAttempts only applies during the initial connection phase (before we
// ever get an IP).  After that, reconnects are unlimited.
constexpr int kMaxAttempts = 8;

const char* TAG = "microfi.wifi";

EventGroupHandle_t s_wifi_events    = nullptr;
int                s_attempts       = 0;
bool               s_connected      = false;
bool               s_ever_connected = false;  // true once we received an IP

void on_event(void* /*arg*/, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;

        if (s_ever_connected) {
            // Persistent phase: always reconnect — never give up.
            ++s_attempts;
            ESP_LOGW(TAG, "wifi lost; reconnect attempt %d", s_attempts);
            esp_wifi_connect();
        } else {
            // Initial phase: honour the retry cap so wifi_start_and_wait()
            // can detect a bad credential / out-of-range scenario.
            if (s_attempts < kMaxAttempts) {
                ++s_attempts;
                ESP_LOGW(TAG, "wifi disconnect; retry %d/%d", s_attempts, kMaxAttempts);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "wifi failed after %d attempts; giving up initial connect",
                         kMaxAttempts);
                xEventGroupSetBits(s_wifi_events, FAIL_BIT);
            }
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "got ip: " IPSTR " (reconnect attempts since last IP: %d)",
                 IP2STR(&ev->ip_info.ip), s_attempts);
        s_attempts       = 0;
        s_connected      = true;
        s_ever_connected = true;
        xEventGroupSetBits(s_wifi_events, GOT_IP_BIT);
    }
}

}  // namespace

bool wifi_connected() {
    return s_connected;
}

Status wifi_start_and_wait(uint32_t timeout_ms) {
    // NVS is required by the WiFi driver for calibration data.
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs);
    }

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == nullptr) return Status::OutOfMemory;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    esp_event_handler_instance_t inst_any;
    esp_event_handler_instance_t inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr, &inst_got_ip));

    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid),
                 CONFIG_MICROFI_WIFI_SSID,
                 sizeof(cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password),
                 CONFIG_MICROFI_WIFI_PASSWORD,
                 sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;  // require at least WPA2 on real hardware

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to ssid='%s'", CONFIG_MICROFI_WIFI_SSID);

    const EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        GOT_IP_BIT | FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & GOT_IP_BIT) return Status::Ok;
    if (bits & FAIL_BIT)   return Status::IoError;
    return Status::IoError;   // timeout
}

}  // namespace microfi
