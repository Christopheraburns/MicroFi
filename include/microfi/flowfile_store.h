// flowfile_store.h -- FlowFile binary serialization for IRepository.
//
// FlowFiles in transit through the engine's queues are persisted to the
// LittleFS repository so they survive a power cycle.  On reboot, the engine
// deserializes all records and replays them into the matching connection
// queues before starting the heartbeat loop.
//
// Wire format (little-endian throughout):
//
//   Offset    Size  Field
//   0         4     magic: {'M','F','F','F'}
//   4         1     version: 0x01
//   5         37    connection_id: 36-char UUID + NUL  (routing key on replay)
//   42        8     flowfile_id: uint64_t LE
//   50        1     attr_count: uint8_t (0..kMaxAttributes)
//   51        N*128 attributes: N * {64-byte key + 64-byte value}, NUL-padded
//   51+N*128  2     content_len: uint16_t LE
//   53+N*128  M     content bytes
//
// Max record size (8 attrs, 256-byte content):
//   51 + 8*128 + 2 + 256 = 1333 bytes → kFlowFileRecordMaxBytes = 1400 (padded).
//
// Thread-safety: stateless free functions, safe from any task.

#pragma once

#include "microfi/flowfile.h"
#include "microfi/types.h"

#include <cstddef>
#include <cstdint>

namespace microfi {

// Buffer size guaranteed to hold any serialized FlowFile.
constexpr size_t kFlowFileRecordMaxBytes = 1400;

// Serialize ff into buf[0..buf_cap).
// conn_id must be the 36-char UUID of the connection this FlowFile is on.
// *out_len receives the number of bytes written.
// Returns InvalidArg if buf_cap < kFlowFileRecordMaxBytes.
Status flowfile_serialize(const FlowFile& ff,
                          const char*     conn_id,
                          uint8_t*        buf,
                          size_t          buf_cap,
                          size_t*         out_len);

// Deserialize a record previously written by flowfile_serialize.
// conn_id_out must be a caller-supplied 37-byte buffer; it receives the
// connection UUID string (NUL-terminated) used as the replay routing key.
// Returns ParseError if the magic or version field is wrong.
Status flowfile_deserialize(const uint8_t* buf,
                            size_t         len,
                            char*          conn_id_out,
                            FlowFile*      ff_out);

}  // namespace microfi
