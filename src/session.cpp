// session.cpp -- transfer routing for processors.

#include "microfi/session.h"

#include <cstring>

namespace microfi {

void Session::reset() {
    input_ = nullptr;
    n_bound_ = 0;
    n_outputs_ = 0;
    for (auto& b : bindings_) {
        b.name[0] = '\0';
        b.queue = nullptr;
    }
}

Status Session::bind_relationship(const char* name, Queue* q) {
    if (name == nullptr || q == nullptr) return Status::InvalidArg;
    const size_t nlen = ::strnlen(name, kMaxRelationshipNameLen);
    if (nlen == 0 || nlen == kMaxRelationshipNameLen) return Status::InvalidArg;
    if (n_bound_ >= kMaxRelationships) return Status::Full;

    std::memcpy(bindings_[n_bound_].name, name, nlen);
    bindings_[n_bound_].name[nlen] = '\0';
    bindings_[n_bound_].queue = q;
    ++n_bound_;
    return Status::Ok;
}

Status Session::transfer(const FlowFile& f, const char* relationship) {
    if (relationship == nullptr) return Status::InvalidArg;
    for (size_t i = 0; i < n_bound_; ++i) {
        if (std::strncmp(bindings_[i].name, relationship,
                         kMaxRelationshipNameLen) == 0) {
            const Status rc = bindings_[i].queue->try_push(f);
            if (rc != Status::Ok) return rc;
            ++n_outputs_;
        }
    }
    // Unbound relationship is treated as "auto-terminate" -- not an error,
    // the FlowFile is dropped. Mirrors NiFi's auto-terminate semantics.
    return Status::Ok;
}

}  // namespace microfi
