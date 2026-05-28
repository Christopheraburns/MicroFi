// flowfile.cpp -- implementation of microfi::FlowFile.

#include "microfi/flowfile.h"

#include <cstring>

namespace microfi {

namespace {

bool key_equals(const char* a, const char* b) {
    return std::strncmp(a, b, kMaxAttributeKeyLen) == 0;
}

}  // namespace

void FlowFile::clear() {
    id_ = 0;
    n_attrs_ = 0;
    for (auto& a : attrs_) {
        a.key[0] = '\0';
        a.value[0] = '\0';
    }
    content_len_ = 0;
}

Status FlowFile::set_attribute(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return Status::InvalidArg;
    }
    const size_t klen = ::strnlen(key, kMaxAttributeKeyLen);
    const size_t vlen = ::strnlen(value, kMaxAttributeValLen);
    if (klen == 0 || klen == kMaxAttributeKeyLen) {
        return Status::InvalidArg;  // empty key, or key too long (not NUL-terminated).
    }
    if (vlen == kMaxAttributeValLen) {
        return Status::InvalidArg;
    }

    // Overwrite if the key already exists.
    for (size_t i = 0; i < n_attrs_; ++i) {
        if (key_equals(attrs_[i].key, key)) {
            std::memcpy(attrs_[i].value, value, vlen);
            attrs_[i].value[vlen] = '\0';
            return Status::Ok;
        }
    }

    if (n_attrs_ >= kMaxAttributes) {
        return Status::Full;
    }
    std::memcpy(attrs_[n_attrs_].key, key, klen);
    attrs_[n_attrs_].key[klen] = '\0';
    std::memcpy(attrs_[n_attrs_].value, value, vlen);
    attrs_[n_attrs_].value[vlen] = '\0';
    ++n_attrs_;
    return Status::Ok;
}

const char* FlowFile::get_attribute(const char* key) const {
    if (key == nullptr) return nullptr;
    for (size_t i = 0; i < n_attrs_; ++i) {
        if (key_equals(attrs_[i].key, key)) {
            return attrs_[i].value;
        }
    }
    return nullptr;
}

void FlowFile::attribute_at(size_t i, const char** key, const char** value) const {
    if (i >= n_attrs_) {
        if (key)   *key   = nullptr;
        if (value) *value = nullptr;
        return;
    }
    if (key)   *key   = attrs_[i].key;
    if (value) *value = attrs_[i].value;
}

Status FlowFile::set_content(const uint8_t* bytes, size_t len) {
    if (len > kInlineContentBytes) {
        return Status::InvalidArg;
    }
    if (len > 0 && bytes == nullptr) {
        return Status::InvalidArg;
    }
    if (len > 0) {
        std::memcpy(content_, bytes, len);
    }
    content_len_ = len;
    return Status::Ok;
}

}  // namespace microfi
