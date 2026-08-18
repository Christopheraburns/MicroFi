// liveness_led.cpp
//
// See include/microfi/liveness_led.h. One FreeRTOS task, one pin, a plain
// on/off strobe. Pin, polarity, and period come from Kconfig
// (MICROFI_LIVENESS_LED_*) so a per-device sdkconfig overlay can retarget or
// disable it — relevant because a flow-level SetGPIO on the same pin has no
// arbitration against this task (last writer wins, and a flow apply re-runs
// gpio_config on its pin); if a board needs GPIO 21 back for a flow demo,
// point the strobe elsewhere in that board's overlay.

#include "microfi/liveness_led.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

namespace microfi {

namespace {

// Gated like liveness_led_start()'s stub below: the MICROFI_LIVENESS_LED_*
// pin/period symbols only exist in sdkconfig when the strobe is enabled
// (they sit inside Kconfig's `if MICROFI_LIVENESS_LED`).
#if CONFIG_MICROFI_LIVENESS_LED

const char* TAG = "microfi.led";

void strobe_task(void* /*arg*/) {
    const int  pin        = CONFIG_MICROFI_LIVENESS_LED_GPIO;
#if CONFIG_MICROFI_LIVENESS_LED_ACTIVE_LOW
    const int  on_level   = 0;
#else
    const int  on_level   = 1;
#endif
    const TickType_t half = pdMS_TO_TICKS(CONFIG_MICROFI_LIVENESS_LED_PERIOD_MS / 2);

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << pin);
    cfg.mode         = GPIO_MODE_OUTPUT;
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "gpio_config failed for pin %d; strobe disabled", pin);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "liveness strobe on GPIO %d (%s, period %d ms)",
             pin, on_level == 0 ? "active-low" : "active-high",
             CONFIG_MICROFI_LIVENESS_LED_PERIOD_MS);

    bool on = false;
    for (;;) {
        on = !on;
        gpio_set_level(static_cast<gpio_num_t>(pin), on ? on_level : !on_level);
        vTaskDelay(half);
    }
}

#endif  // CONFIG_MICROFI_LIVENESS_LED

}  // namespace

Status liveness_led_start() {
#if !CONFIG_MICROFI_LIVENESS_LED
    return Status::Ok;  // compiled out by config
#else
    if (xTaskCreate(&strobe_task, "microfi-led", 2048, nullptr,
                    tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
        return Status::OutOfMemory;
    }
    return Status::Ok;
#endif
}

}  // namespace microfi
