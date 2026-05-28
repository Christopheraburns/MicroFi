// storage.h
//
// Top-level storage subsystem. Called once from app_main during boot:
//
//     storage_init();
//
// On success, the singleton IRepository is mounted and accessible via
// repository(). On failure, the agent continues to run with no persistence
// (a warning is logged and repository() returns nullptr -- callers must
// tolerate the nullptr to keep the engine working in volatile-only mode).
//
// The choice of concrete backend is driven by Kconfig:
//   MICROFI_SD_OVERFLOW=n (default)  -> LittleFSRepository
//   MICROFI_SD_OVERFLOW=y             -> TieredRepository (Phase 2; warns
//                                        and falls back to LittleFS today)

#pragma once

#include "microfi/repository.h"
#include "microfi/types.h"

namespace microfi {

// Mounts the configured backend. Safe to call multiple times; subsequent
// calls return Ok without re-mounting.
Status storage_init();

// Returns the mounted repository, or nullptr if init was never called or
// failed. Callers must tolerate nullptr (volatile-only operation).
IRepository* repository();

}  // namespace microfi
