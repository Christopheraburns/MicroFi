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
// Dynamic properties and this engine: ProcessorDescriptor has no fixed
// property list here (matches upstream's "no fixed properties" shape) and
// EFM's manifest always reports supportsDynamicProperties=false (hardcoded
// in manifest.cpp, not per-processor) -- so the Flow Designer's own UI has
// no "+" affordance to add a property to this node. The flow parser doesn't
// care, though: flow_parser.cpp copies every key/value pair out of a pushed
// node's "properties" object with no filtering against any declared list,
// so pushing the flow directly via the Designer REST API with an explicit
// properties object (the same way every processor in this fork has been
// verified so far) works today. A real "+" button needs an engine change,
// not a processor change.
//
// Mutation: Session::input()/transfer() only expose a `const FlowFile*` --
// there is no in-place attribute-mutation path through Session. Work around
// it by copying the input FlowFile (fixed-size, trivially copyable) into a
// local, mutating the copy, and transferring the copy.
//
// Sizing: capped at kMaxDynamicProps=4 configured attributes, not the
// engine-wide kMaxNodeProperties=8, to keep State inside the 256-byte
// per-node slab (4 * (24 + 32) + 1 = 225 bytes). Four literal attributes is
// enough to exercise attribute-mutation flows in testing; raise the cap
// (and shrink key/value width or the slab) if a real flow needs more.

#include "microfi/flowfile.h"
#include "microfi/processor.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/types.h"

#include "esp_log.h"

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

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s = static_cast<State*>(state);
    s->count = 0;

    for (size_t i = 0; i < count && s->count < kMaxDynamicProps; ++i) {
        const NodeProperty& p = props[i];
        if (p.key[0] == '\0') continue;

        DynAttr& d = s->attrs[s->count];
        std::strncpy(d.key, p.key, kKeyLen - 1);
        d.key[kKeyLen - 1] = '\0';
        std::strncpy(d.value, p.value, kValLen - 1);
        d.value[kValLen - 1] = '\0';
        ++s->count;
    }

    if (count > kMaxDynamicProps) {
        ESP_LOGW(TAG, "flow configured %u properties, only the first %u are applied",
                 static_cast<unsigned>(count), static_cast<unsigned>(kMaxDynamicProps));
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
    "Adds/overwrites literal-value attributes on the FlowFile from configured "
    "dynamic properties (no Expression Language evaluation).",
    &on_trigger,
    nullptr,          // no on_init
    &on_configure,    // reads up to kMaxDynamicProps key/value pairs
    sizeof(State),
    "INPUT_REQUIRED", // must have an incoming connection
    nullptr, 0,       // no fixed properties -- see file header
};

}  // namespace
}  // namespace updateattr
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::updateattr::descriptor)
