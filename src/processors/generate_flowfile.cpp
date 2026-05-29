// processors/generate_flowfile.cpp
//
// Emits a fixed-content FlowFile on each tick, with two attributes:
//     "source"    -- always "GenerateFlowFile"
//     "tickIndex" -- monotonic counter, useful for spotting drops downstream
//
// Stateful via a small struct kept in the engine's per-node slab.

#include "microfi/flowfile.h"
#include "microfi/flow_engine.h"
#include "microfi/processor.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/types.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace microfi {
namespace gen {

namespace {

struct State {
    uint32_t tick;
    uint32_t batch_size;              // from "Batch Size" property (default 1)
    bool     has_custom_text;         // true when "Custom Text" was provided
    char     custom_text[64];         // cached from "Custom Text" property
};

static_assert(sizeof(State) <= 256, "State larger than engine slab");

constexpr const char* kDefaultPayload    = "MicroFi GenerateFlowFile payload";
constexpr size_t      kDefaultPayloadLen = 32;   // strlen(kDefaultPayload)

// ---- Property declarations (MiNiFi C++ compatible names) ----------------
//
// Property names match MiNiFi C++ exactly so that EFM flow definitions
// published against a full MiNiFi agent work on MicroFi without edits.
// Runtime reading of these values is wired in when Slice B (flow-apply) lands.

static const AllowableValue kDataFormatValues[] = {
    { "Binary", nullptr },
    { "Text",   nullptr },
};

static const AllowableValue kBoolValues[] = {
    { "true",  nullptr },
    { "false", nullptr },
};

static const PropertyDescriptor kProperties[] = {
    {
        /* name          */ "File Size",
        /* description   */ "The size of the file that will be used.",
        /* default_value */ "1 kB",
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Batch Size",
        /* description   */ "The number of FlowFiles to be transferred in each invocation.",
        /* default_value */ "1",
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Data Format",
        /* description   */ "Specifies whether the data should be Text or Binary.",
        /* default_value */ "Binary",
        /* required      */ false,
        /* allowable     */ kDataFormatValues, 2,
    },
    {
        /* name          */ "Unique FlowFiles",
        /* description   */ "If true, each FlowFile will be unique. "
                            "If false, a fixed payload is reused for higher throughput.",
        /* default_value */ "true",
        /* required      */ false,
        /* allowable     */ kBoolValues, 2,
    },
    {
        /* name          */ "Custom Text",
        /* description   */ "If Data Format is Text and Unique FlowFiles is false, "
                            "this text is used as the FlowFile content.",
        /* default_value */ nullptr,
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
};
static constexpr size_t kPropertyCount =
    sizeof(kProperties) / sizeof(kProperties[0]);

Status on_init(void* state) {
    auto* s       = static_cast<State*>(state);
    s->tick        = 0;
    s->batch_size  = 1;
    s->has_custom_text = false;
    s->custom_text[0] = '\0';
    return Status::Ok;
}

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s = static_cast<State*>(state);

    for (size_t i = 0; i < count; ++i) {
        const NodeProperty& p = props[i];

        if (std::strcmp(p.key, "Custom Text") == 0 && p.value[0] != '\0') {
            std::strncpy(s->custom_text, p.value, sizeof(s->custom_text) - 1);
            s->custom_text[sizeof(s->custom_text) - 1] = '\0';
            s->has_custom_text = true;
        }
        else if (std::strcmp(p.key, "Batch Size") == 0 && p.value[0] != '\0') {
            const uint32_t v = static_cast<uint32_t>(atoi(p.value));
            s->batch_size = (v > 0 && v <= 64) ? v : 1;
        }
    }
}

Status on_trigger(Session& session, void* state) {
    auto* s = static_cast<State*>(state);

    const char*  payload     = s->has_custom_text ? s->custom_text : kDefaultPayload;
    const size_t payload_len = s->has_custom_text
                                 ? std::strlen(s->custom_text)
                                 : kDefaultPayloadLen;

    for (uint32_t b = 0; b < s->batch_size; ++b) {
        FlowFile f;
        f.assign_id(FlowEngine::instance().next_id());

        char tick_buf[16];
        std::snprintf(tick_buf, sizeof(tick_buf), "%u",
                      static_cast<unsigned>(s->tick));

        Status rc;
        rc = f.set_attribute("source", "GenerateFlowFile");
        if (rc != Status::Ok) return rc;
        rc = f.set_attribute("tickIndex", tick_buf);
        if (rc != Status::Ok) return rc;
        rc = f.set_content(reinterpret_cast<const uint8_t*>(payload), payload_len);
        if (rc != Status::Ok) return rc;

        rc = session.transfer(f, "success");
        if (rc != Status::Ok) return rc;

        ++s->tick;
    }
    return Status::Ok;
}

ProcessorDescriptor descriptor = {
    "GenerateFlowFile",
    "Emits a FlowFile per tick; content and batch size configurable via EFM.",
    &on_trigger,
    &on_init,
    &on_configure,
    sizeof(State),
    "INPUT_FORBIDDEN",  // source: no incoming connections
    kProperties,
    kPropertyCount,
};

}  // namespace
}  // namespace gen
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::gen::descriptor)
