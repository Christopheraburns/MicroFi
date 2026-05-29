// flow_store.h -- Persistent flow-definition store.
//
// Saves the raw flow-definition body (JSON or YAML) to a single named file
// on the LittleFS partition so it survives a power cycle.  On the next boot,
// flow_def_load() returns it so the engine can prime itself before the first
// heartbeat -- avoiding the "back to zero UUID" race where EFM marks the
// previous UPDATE operation as FAILED and requires a manual re-publish.
//
// This is a SINGLE-SLOT store (one file, overwritten on each update).  It is
// intentionally NOT stored as an IRepository record because the repository
// participates in watermark eviction; the flow definition must survive
// indefinitely and must never be evicted by data-record pressure.
//
// Thread-safety: flow_def_save() is called from the C2 task; flow_def_load()
// and flow_def_clear() are called from app_main before any tasks start.
// No concurrent access occurs, so no mutex is needed.

#pragma once

#include "microfi/types.h"

#include <cstddef>

namespace microfi {

// Filesystem path of the saved flow definition.
// The leading '.' makes it easy to distinguish from data records in the
// same directory if a future layout change co-locates them.
constexpr const char* kFlowDefPath = "/littlefs/.flowdef";

// Maximum flow definition size that can be persisted.  Must be >= the
// kFlowBufBytes constant in c2_client.cpp (currently 16384).
constexpr size_t kFlowDefMaxBytes = 16384;

// Persist the raw flow definition body to flash.
// Overwrites any previously saved definition atomically (write then rename
// is not available on esp_littlefs, so a partial write on power-loss will
// produce a truncated file; flow_def_load() detects this via the embedded
// length header and returns NotFound, falling back to the boot-default graph).
// Returns Ok on success, IoError on write failure, InvalidArg if len == 0
// or len > kFlowDefMaxBytes.
Status flow_def_save(const char* body, size_t len);

// Load the saved flow definition into the caller-supplied buffer.
// *out_len is set to the number of bytes written on Ok.
// Returns NotFound if no definition has been saved, Full if buf_cap is
// insufficient (and *out_len is set to the required byte count).
Status flow_def_load(char* buf, size_t buf_cap, size_t* out_len);

// Erase the saved flow definition (factory reset / test fixture teardown).
void flow_def_clear();

// Persist the 36-char flow UUID so it survives a power cycle.
// Saved as a plain null-terminated string at kFlowIdPath (37 bytes on flash).
// Call after flow_def_save() whenever a non-zero flow_id is available.
constexpr const char* kFlowIdPath = "/littlefs/.flowid";
Status flow_id_save(const char* flow_id);

// Load the saved flow UUID into out[37].  Returns NotFound if not saved yet,
// InvalidArg if out is null.  On Ok, out is NUL-terminated.
Status flow_id_load(char out[37]);

}  // namespace microfi
