// flowfile.h
//
// The FlowFile is MicroFi's analogue of Apache NiFi's FlowFile: a record
// of attributes plus a content payload that travels through the processor
// graph.
//
// Sizing is deliberately fixed. The assessment caps us at:
//     8 attributes per FlowFile
//     64 bytes per attribute key or value
//     256 bytes of inline content per FlowFile
//
// Anything bigger is rejected at the API surface. A later iteration adds
// flash-backed spillover for larger payloads; for the first slice everything
// lives inline so a FlowFile is a value type and we never call malloc on a
// hot path.

#pragma once

#include "microfi/types.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace microfi {

constexpr size_t kMaxAttributes      = 8;
constexpr size_t kMaxAttributeKeyLen = 64;
constexpr size_t kMaxAttributeValLen = 64;
constexpr size_t kInlineContentBytes = 256;

class FlowFile {
public:
    FlowFile() = default;

    // Attribute API. Keys and values are NUL-terminated strings; on overflow
    // we return InvalidArg and the FlowFile is left untouched.
    Status set_attribute(const char* key, const char* value);
    const char* get_attribute(const char* key) const;
    size_t attribute_count() const { return n_attrs_; }

    // Iterate attributes by index (i in [0, attribute_count())).
    void attribute_at(size_t i, const char** key, const char** value) const;

    // Content API. The first slice is inline-only: set_content overwrites
    // whatever was there; larger payloads return InvalidArg.
    Status set_content(const uint8_t* bytes, size_t len);
    const uint8_t* content() const { return content_; }
    size_t content_size() const { return content_len_; }

    // Monotonic identifier assigned at FlowFile creation. Useful for tracing
    // a record through the engine without paying for full provenance.
    uint64_t id() const { return id_; }
    void assign_id(uint64_t id) { id_ = id; }
    // Repository record identifier.  0 = not persisted to flash.
    // Set by Queue::try_push() after writing to IRepository; carried through
    // the queue slot; read by the engine's sink commit path to erase the record
    // after successful downstream processing.
    uint64_t record_id() const       { return record_id_; }
    void set_record_id(uint64_t rid) { record_id_ = rid; }


    void clear();

private:
    struct Attr {
        char key[kMaxAttributeKeyLen]   = {0};
        char value[kMaxAttributeValLen] = {0};
    };

    uint64_t id_           = 0;
    uint64_t record_id_   = 0;
    size_t   n_attrs_      = 0;
    Attr     attrs_[kMaxAttributes] = {};
    uint8_t  content_[kInlineContentBytes] = {0};
    size_t   content_len_  = 0;
};

}  // namespace microfi
