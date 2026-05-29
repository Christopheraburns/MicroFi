// flowfile_store.cpp -- FlowFile binary serialization / deserialization.

#include "microfi/flowfile_store.h"

#include <cstring>

namespace microfi {

namespace {

constexpr uint8_t kMagic[4] = {'M', 'F', 'F', 'F'};
constexpr uint8_t kVersion  = 0x01;

static void write_u16_le(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static uint16_t read_u16_le(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void write_u64_le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i, v >>= 8) p[i] = (uint8_t)(v & 0xFF);
}
static uint64_t read_u64_le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

}  // namespace

Status flowfile_serialize(const FlowFile& ff,
                          const char*     conn_id,
                          uint8_t*        buf,
                          size_t          buf_cap,
                          size_t*         out_len) {
    if (buf == nullptr || out_len == nullptr) return Status::InvalidArg;
    if (buf_cap < kFlowFileRecordMaxBytes)    return Status::InvalidArg;
    if (conn_id == nullptr)                   return Status::InvalidArg;

    uint8_t* p = buf;

    // Header: magic + version
    std::memcpy(p, kMagic, 4);  p += 4;
    *p++ = kVersion;

    // connection_id (36 chars + NUL pad to 37 bytes)
    std::memset(p, 0, 37);
    std::strncpy(reinterpret_cast<char*>(p), conn_id, 36);
    p += 37;

    // flowfile_id
    write_u64_le(p, ff.id());  p += 8;

    // attr_count
    const uint8_t ac = (uint8_t)ff.attribute_count();
    *p++ = ac;

    // attributes (each 64-byte key + 64-byte value, zero-padded)
    for (uint8_t i = 0; i < ac; ++i) {
        const char* key   = nullptr;
        const char* value = nullptr;
        ff.attribute_at(i, &key, &value);

        std::memset(p, 0, 64);
        if (key)   std::strncpy(reinterpret_cast<char*>(p), key,   63);
        p += 64;

        std::memset(p, 0, 64);
        if (value) std::strncpy(reinterpret_cast<char*>(p), value, 63);
        p += 64;
    }

    // content_len + content
    const uint16_t clen = (uint16_t)ff.content_size();
    write_u16_le(p, clen);  p += 2;
    if (clen > 0) {
        std::memcpy(p, ff.content(), clen);
        p += clen;
    }

    *out_len = (size_t)(p - buf);
    return Status::Ok;
}

Status flowfile_deserialize(const uint8_t* buf,
                            size_t         len,
                            char*          conn_id_out,
                            FlowFile*      ff_out) {
    if (buf == nullptr || conn_id_out == nullptr || ff_out == nullptr) {
        return Status::InvalidArg;
    }
    // Minimum header: 4 magic + 1 version + 37 conn_id + 8 ff_id + 1 attr_count + 2 clen = 53
    if (len < 53) return Status::ParseError;

    const uint8_t* p = buf;

    if (std::memcmp(p, kMagic, 4) != 0) return Status::ParseError;
    p += 4;

    if (*p != kVersion) return Status::ParseError;
    p += 1;

    // connection_id
    std::memcpy(conn_id_out, p, 36);
    conn_id_out[36] = '\0';
    p += 37;

    // flowfile_id
    const uint64_t ff_id = read_u64_le(p);  p += 8;

    // attr_count
    const uint8_t ac = *p++;

    ff_out->clear();
    ff_out->assign_id(ff_id);

    // Bounds check: need ac*128 + 2 more bytes
    const size_t remaining = (size_t)(buf + len - p);
    if (remaining < (size_t)ac * 128 + 2) return Status::ParseError;

    for (uint8_t i = 0; i < ac; ++i) {
        const char* key   = reinterpret_cast<const char*>(p);  p += 64;
        const char* value = reinterpret_cast<const char*>(p);  p += 64;
        // Ignore overflow (set_attribute returns Full if kMaxAttributes exceeded)
        if (key[0] != '\0') ff_out->set_attribute(key, value);
    }

    // content
    const uint16_t clen = read_u16_le(p);  p += 2;
    if (clen > 0) {
        if ((size_t)(buf + len - p) < clen) return Status::ParseError;
        ff_out->set_content(p, clen);
    }

    return Status::Ok;
}

}  // namespace microfi
