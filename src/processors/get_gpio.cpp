// processors/get_gpio.cpp
//
// Source processor: reads a configured GPIO pin's digital level on every
// tick and emits it as a FlowFile. MicroFi-original -- no upstream MiNiFi
// C++ equivalent (see docs/Processor-Inventory-And-Roadmap.md's Sensor I/O
// table). This is the "real ingress source" called for in
// efm-xiao-microfi.md's design spec #2 / issue #45's build order item 2:
// the simplest genuinely-real onboard-hardware read available on this XIAO
// unit without board-specific pin research -- every ESP32-S3 board,
// including this one, has a BOOT button wired to GPIO0 with an external
// pull-up, so "GPIO Pin"=0 reads a real physical button press with zero
// guessing about this specific unit's wiring.
//
// Polled, not edge/interrupt-driven: the engine schedules on_trigger on a
// timer (matching every other processor here), so this reads the pin's
// current level once per tick rather than reacting to a transition. A true
// edge-triggered variant would need engine-level interrupt support that
// doesn't exist yet.

#include "microfi/flowfile.h"
#include "microfi/flow_engine.h"
#include "microfi/processor.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/types.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace microfi {
namespace getgpio {

namespace {

static const char* TAG = "microfi.proc.gpio";

struct State {
    int32_t pin;         // -1 = unconfigured
    uint8_t pull_mode;    // 0=none 1=up 2=down
    bool    configured;
    uint32_t tick;
};
static_assert(sizeof(State) <= 256, "State larger than engine slab");

static const AllowableValue kPullValues[] = {
    { "none", nullptr },
    { "up",   nullptr },
    { "down", nullptr },
};

static const PropertyDescriptor kProperties[] = {
    {
        /* name          */ "GPIO Pin",
        /* description   */ "GPIO number to read (e.g. 0 for the onboard BOOT button).",
        /* default_value */ nullptr,
        /* required      */ true,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Pull Mode",
        /* description   */ "Internal pull resistor to enable on the pin.",
        /* default_value */ "up",
        /* required      */ false,
        /* allowable     */ kPullValues, 3,
    },
};
static constexpr size_t kPropertyCount =
    sizeof(kProperties) / sizeof(kProperties[0]);

Status on_init(void* state) {
    auto* s = static_cast<State*>(state);
    s->pin = -1;
    s->pull_mode = 1;  // up
    s->configured = false;
    s->tick = 0;
    return Status::Ok;
}

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s = static_cast<State*>(state);

    for (size_t i = 0; i < count; ++i) {
        const NodeProperty& p = props[i];
        if (std::strcmp(p.key, "GPIO Pin") == 0 && p.value[0] != '\0') {
            s->pin = static_cast<int32_t>(atoi(p.value));
        }
        else if (std::strcmp(p.key, "Pull Mode") == 0 && p.value[0] != '\0') {
            if      (std::strcmp(p.value, "none") == 0) s->pull_mode = 0;
            else if (std::strcmp(p.value, "up")   == 0) s->pull_mode = 1;
            else if (std::strcmp(p.value, "down") == 0) s->pull_mode = 2;
        }
    }

    if (s->pin < 0) {
        ESP_LOGE(TAG, "GPIO Pin not configured or invalid; processor will not run");
        s->configured = false;
        return;
    }

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << s->pin);
    cfg.mode         = GPIO_MODE_INPUT;
    cfg.pull_up_en   = (s->pull_mode == 1) ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = (s->pull_mode == 2) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;

    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(pin=%d) failed: %s", static_cast<int>(s->pin),
                 esp_err_to_name(err));
        s->configured = false;
        return;
    }

    s->configured = true;
    ESP_LOGI(TAG, "configured pin=%d pull=%u", static_cast<int>(s->pin), s->pull_mode);
}

Status on_trigger(Session& session, void* state) {
    auto* s = static_cast<State*>(state);

    if (!s->configured) {
        ESP_LOGW(TAG, "not configured; skipping tick");
        return Status::InvalidArg;
    }

    const int level = gpio_get_level(static_cast<gpio_num_t>(s->pin));

    FlowFile f;
    f.assign_id(FlowEngine::instance().next_id());

    char level_buf[4];
    std::snprintf(level_buf, sizeof(level_buf), "%d", level);

    char pin_buf[8];
    std::snprintf(pin_buf, sizeof(pin_buf), "%d", static_cast<int>(s->pin));

    char tick_buf[16];
    std::snprintf(tick_buf, sizeof(tick_buf), "%u", static_cast<unsigned>(s->tick));

    Status rc;
    rc = f.set_attribute("source", "GetGPIO");
    if (rc != Status::Ok) return rc;
    rc = f.set_attribute("gpio_pin", pin_buf);
    if (rc != Status::Ok) return rc;
    rc = f.set_attribute("tickIndex", tick_buf);
    if (rc != Status::Ok) return rc;
    rc = f.set_content(reinterpret_cast<const uint8_t*>(level_buf), std::strlen(level_buf));
    if (rc != Status::Ok) return rc;

    rc = session.transfer(f, "success");
    if (rc != Status::Ok) return rc;

    ++s->tick;
    return Status::Ok;
}

ProcessorDescriptor descriptor = {
    "GetGPIO",
    "Reads a configured GPIO pin's digital level (0/1) on every tick and "
    "emits it as FlowFile content.",
    &on_trigger,
    &on_init,
    &on_configure,
    sizeof(State),
    "INPUT_FORBIDDEN",  // source: no incoming connections
    kProperties,
    kPropertyCount,
};

}  // namespace
}  // namespace getgpio
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::getgpio::descriptor)
