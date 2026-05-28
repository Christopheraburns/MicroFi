// registry.h
//
// Compile-time-discoverable list of processors. Each processor source file
// invokes MICROFI_REGISTER_PROCESSOR(descriptor) at namespace scope. The
// macro creates a global object whose constructor runs at static-init time
// and pushes the descriptor pointer into a fixed-capacity table.
//
// Using global constructors (rather than a linker-section attribute) is
// intentional: it avoids touching the ESP-IDF linker fragments, works on
// every C++ toolchain we'd care about, and the cost is fully paid at boot.
// The "construct on first use" idiom keeps initialization order safe across
// translation units.

#pragma once

#include "microfi/processor.h"
#include "microfi/types.h"

#include <cstddef>

namespace microfi {

class Registry {
public:
    static Registry& instance();

    // Add a descriptor to the table. Called from MICROFI_REGISTER_PROCESSOR.
    // Returns false if the table is already full -- in which case the
    // missing processor will be invisible to the engine. Bump kMaxProcessors
    // (below) if you hit this; it is a deliberate static cap.
    bool register_processor(const ProcessorDescriptor* desc);

    // Lookup by name; returns null if not found.
    const ProcessorDescriptor* find(const char* name) const;

    size_t count() const { return count_; }
    const ProcessorDescriptor* at(size_t i) const;

private:
    Registry() = default;
    static constexpr size_t kMaxProcessors = 16;
    const ProcessorDescriptor* table_[kMaxProcessors] = {};
    size_t count_ = 0;
};

}  // namespace microfi

// Drop this at namespace scope after defining your descriptor:
//     MICROFI_REGISTER_PROCESSOR(microfi::gen::descriptor)
//
// The token-pasting machinery ensures each invocation generates a unique
// auto-register symbol; pass the *fully qualified* descriptor name.
#define MICROFI_PP_CAT_(a, b) a##b
#define MICROFI_PP_CAT(a, b)  MICROFI_PP_CAT_(a, b)

#define MICROFI_REGISTER_PROCESSOR(DESC)                                       \
    namespace {                                                                \
    struct MICROFI_PP_CAT(MicroFiAutoReg_, __LINE__) {                         \
        MICROFI_PP_CAT(MicroFiAutoReg_, __LINE__)() {                          \
            ::microfi::Registry::instance().register_processor(&(DESC));       \
        }                                                                      \
    };                                                                         \
    static MICROFI_PP_CAT(MicroFiAutoReg_, __LINE__)                           \
        MICROFI_PP_CAT(microfi_autoreg_instance_, __LINE__);                   \
    }
