// processors/log_attribute.cpp
//
// Sink processor that logs the input FlowFile's id, attributes, and content
// length to the ESP-IDF logger. Useful as the simplest possible terminator
// for the first-slice graph -- mirrors NiFi's LogAttribute processor.

#include "microfi/flowfile.h"
#include "microfi/processor.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/types.h"

#include "esp_log.h"

#include <cstring>

namespace microfi {
namespace logattr {

namespace {

static const char* TAG = "microfi.proc.log";

// Per-instance state: cached property values set by on_configure.
struct State {
    bool    log_payload;   // "Log Payload" property (default false)
    uint8_t log_level;     // 0=trace 1=debug 2=info 3=warn 4=error (default 2=info)
};
static_assert(sizeof(State) <= 256, "State larger than engine slab");

// ---- Property declarations (MiNiFi C++ compatible names) ----------------

static const AllowableValue kLogLevelValues[] = {
    { "trace", nullptr },
    { "debug", nullptr },
    { "info",  nullptr },
    { "warn",  nullptr },
    { "error", nullptr },
};

static const AllowableValue kBoolValues[] = {
    { "true",  nullptr },
    { "false", nullptr },
};

static const PropertyDescriptor kProperties[] = {
    {
        /* name          */ "Log Level",
        /* description   */ "The Log Level to use when logging the Attributes.",
        /* default_value */ "info",
        /* required      */ false,
        /* allowable     */ kLogLevelValues, 5,
    },
    {
        /* name          */ "Log Payload",
        /* description   */ "If true, the FlowFile's content will be logged in addition to its attributes.",
        /* default_value */ "false",
        /* required      */ false,
        /* allowable     */ kBoolValues, 2,
    },
    {
        /* name          */ "Log prefix",
        /* description   */ "An optional identifier prepended to each log line for this processor.",
        /* default_value */ nullptr,
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Attributes to Log",
        /* description   */ "A comma-separated list of attributes to log. "
                            "If blank, all attributes are logged.",
        /* default_value */ nullptr,
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Attributes to Ignore",
        /* description   */ "A comma-separated list of attributes to exclude from logging.",
        /* default_value */ nullptr,
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
};
static constexpr size_t kPropertyCount =
    sizeof(kProperties) / sizeof(kProperties[0]);

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s      = static_cast<State*>(state);
    s->log_payload = false;
    s->log_level   = 2;  // info

    for (size_t i = 0; i < count; ++i) {
        const NodeProperty& p = props[i];

        if (std::strcmp(p.key, "Log Payload") == 0) {
            s->log_payload = (std::strcmp(p.value, "true") == 0);
        }
        else if (std::strcmp(p.key, "Log Level") == 0) {
            if      (std::strcmp(p.value, "trace") == 0) s->log_level = 0;
            else if (std::strcmp(p.value, "debug") == 0) s->log_level = 1;
            else if (std::strcmp(p.value, "info")  == 0) s->log_level = 2;
            else if (std::strcmp(p.value, "warn")  == 0) s->log_level = 3;
            else if (std::strcmp(p.value, "error") == 0) s->log_level = 4;
        }
    }
}

// Log a header line + attribute list at the configured level.
#define LOG_AT(lvl, tag, fmt, ...) \
    do { \
        if      ((lvl) == 0) ESP_LOGV(tag, fmt, ##__VA_ARGS__); \
        else if ((lvl) == 1) ESP_LOGD(tag, fmt, ##__VA_ARGS__); \
        else if ((lvl) == 2) ESP_LOGI(tag, fmt, ##__VA_ARGS__); \
        else if ((lvl) == 3) ESP_LOGW(tag, fmt, ##__VA_ARGS__); \
        else                 ESP_LOGE(tag, fmt, ##__VA_ARGS__); \
    } while (0)

Status on_trigger(Session& session, void* state) {
    const FlowFile* in = session.input();
    if (in == nullptr) return Status::Again;

    const auto* s = static_cast<const State*>(state);
    const uint8_t lvl = s->log_level;

    LOG_AT(lvl, TAG, "FlowFile id=%llu content_size=%u attributes:",
           static_cast<unsigned long long>(in->id()),
           static_cast<unsigned>(in->content_size()));

    for (size_t i = 0; i < in->attribute_count(); ++i) {
        const char* k = nullptr;
        const char* v = nullptr;
        in->attribute_at(i, &k, &v);
        if (k != nullptr && v != nullptr)
            LOG_AT(lvl, TAG, "  %s = %s", k, v);
    }

    if (s->log_payload && in->content_size() > 0) {
        // Log up to 128 bytes of content as a string.
        const uint8_t* data = in->content();
        const size_t   show = in->content_size() < 128 ? in->content_size() : 128;
        // Print as a null-terminated C-string (content may already be text).
        char preview[129];
        std::memcpy(preview, data, show);
        preview[show] = '\0';
        LOG_AT(lvl, TAG, "  payload: %s", preview);
    }

    return Status::Ok;
}

ProcessorDescriptor descriptor = {
    "LogAttribute",
    "Logs FlowFile id, attributes, and optionally content to ESP-IDF logger.",
    &on_trigger,
    nullptr,          // no on_init
    &on_configure,    // reads Log Level + Log Payload from EFM flow definition
    sizeof(State),
    "INPUT_REQUIRED", // sink: must have an incoming connection
    kProperties,
    kPropertyCount,
};

}  // namespace
}  // namespace logattr
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::logattr::descriptor)
