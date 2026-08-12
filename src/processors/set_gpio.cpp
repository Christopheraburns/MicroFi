// processors/set_gpio.cpp
//
// Sink/mid-chain processor: drives a configured GPIO pin's digital level
// when triggered by an incoming FlowFile. MicroFi-original -- the write-side
// counterpart to GetGPIO, and the first processor that lets a flow act on
// the physical world (issue #134's MicroFi-3 track: ListenHTTP -> SetGPIO
// as the LED-control trigger flow).
//
// The level to drive comes from either a fixed property or -- the default --
// the FlowFile's own content ("Pin Level" = from-content), so a single
// ListenHTTP -> SetGPIO chain gives runtime on/off/toggle control over HTTP
// with no branching (MicroFi has no RouteOnAttribute/EL):
//
//   POST body "1" / "high" / "on"   -> logical high
//   POST body "0" / "low"  / "off"  -> logical low
//   POST body "toggle"              -> invert the last driven level
//
// "Invert" flips the physical output for active-low hardware. The XIAO
// ESP32-S3's onboard user LED (GPIO 21) is active-low: configure
// GPIO Pin=21, Invert=true and logical "on" lights the LED.
//
// Unparseable content is a warn + pass-through (pin untouched) rather than
// a failure route: MicroFi processors declare a single "success"
// relationship by design (see the one-consumer-per-relationship engine
// constraint), and a fire-and-forget HTTP trigger has nobody to report a
// 4xx to anyway.

#include "microfi/flowfile.h"
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
namespace setgpio {

namespace {

static const char* TAG = "microfi.proc.setgpio";

struct State {
    int32_t pin;          // -1 = unconfigured
    uint8_t mode;         // 0=from-content 1=high 2=low 3=toggle
    bool    invert;       // flip physical level for active-low hardware
    bool    configured;
    uint8_t last_level;   // last driven logical level (for toggle)
};
static_assert(sizeof(State) <= 256, "State larger than engine slab");

static const AllowableValue kLevelValues[] = {
    { "from-content", nullptr },
    { "high",         nullptr },
    { "low",          nullptr },
    { "toggle",       nullptr },
};

static const AllowableValue kBoolValues[] = {
    { "false", nullptr },
    { "true",  nullptr },
};

static const PropertyDescriptor kProperties[] = {
    {
        /* name          */ "GPIO Pin",
        /* description   */ "GPIO number to drive (e.g. 21 for the XIAO ESP32-S3 "
                            "onboard user LED -- pair with Invert=true, it is active-low).",
        /* default_value */ nullptr,
        /* required      */ true,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Pin Level",
        /* description   */ "Level to drive on each trigger. from-content reads the "
                            "FlowFile content: 1/high/on, 0/low/off, or toggle.",
        /* default_value */ "from-content",
        /* required      */ false,
        /* allowable     */ kLevelValues, 4,
    },
    {
        /* name          */ "Invert",
        /* description   */ "If true, the physical output is the inverse of the logical "
                            "level -- for active-low wiring like the XIAO onboard LED.",
        /* default_value */ "false",
        /* required      */ false,
        /* allowable     */ kBoolValues, 2,
    },
};
static constexpr size_t kPropertyCount =
    sizeof(kProperties) / sizeof(kProperties[0]);

Status on_init(void* state) {
    auto* s = static_cast<State*>(state);
    s->pin = -1;
    s->mode = 0;  // from-content
    s->invert = false;
    s->configured = false;
    s->last_level = 0;
    return Status::Ok;
}

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s = static_cast<State*>(state);

    for (size_t i = 0; i < count; ++i) {
        const NodeProperty& p = props[i];
        if (std::strcmp(p.key, "GPIO Pin") == 0 && p.value[0] != '\0') {
            s->pin = static_cast<int32_t>(atoi(p.value));
        }
        else if (std::strcmp(p.key, "Pin Level") == 0 && p.value[0] != '\0') {
            if      (std::strcmp(p.value, "from-content") == 0) s->mode = 0;
            else if (std::strcmp(p.value, "high")         == 0) s->mode = 1;
            else if (std::strcmp(p.value, "low")          == 0) s->mode = 2;
            else if (std::strcmp(p.value, "toggle")       == 0) s->mode = 3;
        }
        else if (std::strcmp(p.key, "Invert") == 0 && p.value[0] != '\0') {
            s->invert = (std::strcmp(p.value, "true") == 0);
        }
    }

    if (s->pin < 0) {
        ESP_LOGE(TAG, "GPIO Pin not configured or invalid; processor will not run");
        s->configured = false;
        return;
    }

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << s->pin);
    cfg.mode         = GPIO_MODE_OUTPUT;
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;

    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(pin=%d) failed: %s", static_cast<int>(s->pin),
                 esp_err_to_name(err));
        s->configured = false;
        return;
    }

    // Start at logical low so an active-low LED begins dark.
    gpio_set_level(static_cast<gpio_num_t>(s->pin), s->invert ? 1 : 0);

    s->configured = true;
    ESP_LOGI(TAG, "configured pin=%d mode=%u invert=%d",
             static_cast<int>(s->pin), s->mode, s->invert ? 1 : 0);
}

// Parse a from-content command. Returns 0/1 for a level, 2 for toggle,
// -1 for unrecognized.
int parse_command(const uint8_t* data, size_t len) {
    // Content arrives as raw bytes (e.g. a ListenHTTP POST body); compare as
    // a bounded, trimmed string.
    char buf[16];
    size_t n = 0;
    for (size_t i = 0; i < len && n < sizeof(buf) - 1; ++i) {
        const char c = static_cast<char>(data[i]);
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        buf[n++] = c;
    }
    buf[n] = '\0';

    if (std::strcmp(buf, "1") == 0 || std::strcmp(buf, "high") == 0 ||
        std::strcmp(buf, "on") == 0)
        return 1;
    if (std::strcmp(buf, "0") == 0 || std::strcmp(buf, "low") == 0 ||
        std::strcmp(buf, "off") == 0)
        return 0;
    if (std::strcmp(buf, "toggle") == 0)
        return 2;
    return -1;
}

Status on_trigger(Session& session, void* state) {
    const FlowFile* in = session.input();
    if (in == nullptr) return Status::Again;

    auto* s = static_cast<State*>(state);

    if (!s->configured) {
        ESP_LOGW(TAG, "not configured; passing FlowFile through untouched");
        FlowFile out = *in;
        session.transfer(out, "success");
        return Status::Ok;
    }

    int logical;
    switch (s->mode) {
        case 1: logical = 1; break;
        case 2: logical = 0; break;
        case 3: logical = s->last_level ? 0 : 1; break;
        default: {
            const int cmd = parse_command(in->content(), in->content_size());
            if (cmd < 0) {
                ESP_LOGW(TAG, "unrecognized content (%u bytes); pin %d untouched",
                         static_cast<unsigned>(in->content_size()),
                         static_cast<int>(s->pin));
                FlowFile out = *in;
                out.set_attribute("gpio_result", "unrecognized-content");
                session.transfer(out, "success");
                return Status::Ok;
            }
            logical = (cmd == 2) ? (s->last_level ? 0 : 1) : cmd;
            break;
        }
    }

    const int physical = s->invert ? (logical ? 0 : 1) : logical;
    gpio_set_level(static_cast<gpio_num_t>(s->pin), physical);
    s->last_level = static_cast<uint8_t>(logical);

    ESP_LOGI(TAG, "pin %d -> logical %d (physical %d)",
             static_cast<int>(s->pin), logical, physical);

    char pin_buf[12];
    std::snprintf(pin_buf, sizeof(pin_buf), "%d", static_cast<int>(s->pin));

    FlowFile out = *in;
    out.set_attribute("gpio_pin", pin_buf);
    out.set_attribute("gpio_level", logical ? "1" : "0");
    session.transfer(out, "success");
    return Status::Ok;
}

ProcessorDescriptor descriptor = {
    "SetGPIO",
    "Drives a configured GPIO pin's digital level when triggered -- fixed "
    "high/low/toggle or parsed from the FlowFile content (1/0/toggle).",
    &on_trigger,
    &on_init,
    &on_configure,
    sizeof(State),
    "INPUT_REQUIRED",  // acts on an incoming FlowFile (e.g. from ListenHTTP)
    kProperties,
    kPropertyCount,
};

}  // namespace
}  // namespace setgpio
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::setgpio::descriptor)
