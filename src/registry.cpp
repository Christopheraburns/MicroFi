// registry.cpp -- static processor registry.

#include "microfi/registry.h"

#include <cstring>

namespace microfi {

Registry& Registry::instance() {
    // Function-local static -- constructed on first use, thread-safe init
    // in C++11 and later. Avoids static-init ordering bugs across TUs.
    static Registry r;
    return r;
}

bool Registry::register_processor(const ProcessorDescriptor* desc) {
    if (desc == nullptr || desc->name == nullptr) return false;
    if (count_ >= kMaxProcessors) return false;
    // Reject duplicates by name.
    for (size_t i = 0; i < count_; ++i) {
        if (std::strcmp(table_[i]->name, desc->name) == 0) {
            return false;
        }
    }
    table_[count_++] = desc;
    return true;
}

const ProcessorDescriptor* Registry::find(const char* name) const {
    if (name == nullptr) return nullptr;
    for (size_t i = 0; i < count_; ++i) {
        if (std::strcmp(table_[i]->name, name) == 0) {
            return table_[i];
        }
    }
    return nullptr;
}

const ProcessorDescriptor* Registry::at(size_t i) const {
    if (i >= count_) return nullptr;
    return table_[i];
}

}  // namespace microfi
