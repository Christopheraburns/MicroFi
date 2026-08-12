// processors/update_attribute.cpp
//
// Adds/overwrites literal-value attributes on the input FlowFile, then
// passes it through. Mirrors MiNiFi C++'s UpdateAttribute -- upstream takes
// arbitrary dynamic properties (attribute name -> Expression-Language value)
// and writes each as a FlowFile attribute. This is the "literal values
// first" slice called for in efm-xiao-microfi.md's design spec #3: no EL
// evaluator exists yet, so a configured value is written to the attribute
// verbatim, unevaluated.
//
// Not true dynamic properties: an earlier revision of this file declared
// zero fixed properties, relying on flow_parser.cpp copying every key/value
// pair out of a pushed node's "properties" object with no filtering against
// any declared list. That parses fine, but EFM's own flow *validation*
// layer (GET .../validate, which /publish depends on) independently checks
// every configured property name against propertyDescriptors and rejects
// anything not declared -- "Property 'x' is not supported" -- blocking
// publish entirely. Confirmed on hardware (issue #45 follow-on work): a
// manifest with supportsDynamicProperties=false (hardcoded engine-wide in
// manifest.cpp, not overridable per-processor) means EFM's Designer will
// never treat any property as dynamic, no matter what the processor itself
// declares. So this uses kMaxDynamicProps declared name/value slot pairs
// ("Attribute 1 Name" / "Attribute 1 Value", ...) instead -- validates
// cleanly, and on_configure reads each pair into the same DynAttr storage
// the literal-values design already used.
//
// Mutation: Session::input()/transfer() only expose a `const FlowFile*` --
// there is no in-place attribute-mutation path through Session. Work around
// it by copying the input FlowFile (fixed-size, trivially copyable) into a
// local, mutating the copy, and transferring the copy.
//
// Sizing: capped at kMaxDynamicProps=4 configured attributes to keep State
// inside the 256-byte per-node slab (4 * (24 + 32) + 1 = 225 bytes).

#include "microfi/flowfile.h"
#include "microfi/processor.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/types.h"

#include "esp_log.h"

#include <cstdio>
#include <cstring>

namespace microfi {
namespace updateattr {

namespace {

static const char* TAG = "microfi.proc.update";

constexpr size_t kMaxDynamicProps = 4;
constexpr size_t kKeyLen          = 24;
constexpr size_t kValLen          = 32;

struct DynAttr {
    char key[kKeyLen]   = {0};
    char value[kValLen] = {0};
};

struct State {
    DynAttr attrs[kMaxDynamicProps];
    uint8_t count;
};
static_assert(sizeof(State) <= 256, "State larger than engine slab");

// ---- Property declarations: declared name/value slot pairs --------------
// EFM's flow validation rejects any property not in this list (see file
// header) so upstream's "true" dynamic properties aren't reachable through
// the Designer API today -- named slots are the workable shape.

static const PropertyDescriptor kProperties[] = {
    { "Attribute 1 Name",  "Name of the first attribute to set.",  nullptr, false, nullptr, 0 },
    { "Attribute 1 Value", "Literal value for the first attribute.", nullptr, false, nullptr, 0 },
    { "Attribute 2 Name",  "Name of the second attribute to set.", nullptr, false, nullptr, 0 },
    { "Attribute 2 Value", "Literal value for the second attribute.", nullptr, false, nullptr, 0 },
    { "Attribute 3 Name",  "Name of the third attribute to set.",  nullptr, false, nullptr, 0 },
    { "Attribute 3 Value", "Literal value for the third attribute.", nullptr, false, nullptr, 0 },
    { "Attribute 4 Name",  "Name of the fourth attribute to set.", nullptr, false, nullptr, 0 },
    { "Attribute 4 Value", "Literal value for the fourth attribute.", nullptr, false, nullptr, 0 },
};
static constexpr size_t kPropertyCount =
    sizeof(kProperties) / sizeof(kProperties[0]);

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s = static_cast<State*>(state);
    s->count = 0;

    char name_key[24];
    char value_key[24];

    for (size_t slot = 1; slot <= kMaxDynamicProps; ++slot) {
        std::snprintf(name_key,  sizeof(name_key),  "Attribute %u Name",  static_cast<unsigned>(slot));
        std::snprintf(value_key, sizeof(value_key), "Attribute %u Value", static_cast<unsigned>(slot));

        const char* name_val  = nullptr;
        const char* value_val = nullptr;
        for (size_t i = 0; i < count; ++i) {
            if (std::strcmp(props[i].key, name_key) == 0)  name_val  = props[i].value;
            if (std::strcmp(props[i].key, value_key) == 0) value_val = props[i].value;
        }

        if (name_val == nullptr || name_val[0] == '\0') continue;  // slot unused

        DynAttr& d = s->attrs[s->count];
        std::strncpy(d.key, name_val, kKeyLen - 1);
        d.key[kKeyLen - 1] = '\0';
        std::strncpy(d.value, value_val ? value_val : "", kValLen - 1);
        d.value[kValLen - 1] = '\0';
        ++s->count;
    }
}

Status on_trigger(Session& session, void* state) {
    const FlowFile* in = session.input();
    if (in == nullptr) return Status::Again;

    const auto* s = static_cast<const State*>(state);

    // Session exposes only a const FlowFile*; copy it to get a mutable one.
    FlowFile out = *in;

    for (uint8_t i = 0; i < s->count; ++i) {
        const DynAttr& d = s->attrs[i];
        const Status rc = out.set_attribute(d.key, d.value);
        if (rc != Status::Ok) {
            ESP_LOGW(TAG, "set_attribute(%s) failed (FlowFile attribute cap reached?)",
                     d.key);
        }
    }

    session.transfer(out, "success");
    return Status::Ok;
}

ProcessorDescriptor descriptor = {
    "UpdateAttribute",
    "Adds/overwrites literal-value attributes on the FlowFile from up to 4 "
    "configured name/value slot pairs (no Expression Language evaluation).",
    &on_trigger,
    nullptr,          // no on_init
    &on_configure,    // reads up to kMaxDynamicProps name/value slot pairs
    sizeof(State),
    "INPUT_REQUIRED", // must have an incoming connection
    kProperties,
    kPropertyCount,
};

}  // namespace
}  // namespace updateattr
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::updateattr::descriptor)
