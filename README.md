# MicroFi

An exploratory microcontroller-class data ingestion agent that participates in the Apache MiNiFi C2 ecosystem. See [`MICROFI_ASSESSMENT.md`](docs/MICROFI_ASSESSMENT.md) for the design rationale.

This repository is a clean-room reimplementation of the MiNiFi protocol contracts (FlowFile semantics, C2 heartbeat/ack) targeted at ESP32 first. The upstream `nifi-minifi-cpp` source is a reference, not a starting tree.

## Status

Pre-alpha. First slice scope:

- [x] Project skeleton (PlatformIO + ESP-IDF).
- [x] Core types: `FlowFile`, `Queue`, `Session`, `Processor`, static `Registry`.
- [x] Minimal flow engine (single-task scheduler, hard-wired graph).
- [x] WiFi station bring-up.
- [x] HTTPS heartbeat POST to a configurable C2 URL (mbedTLS via `esp_http_client`).
- [x] Two embedded processors: `GenerateFlowFile`, `LogAttribute`.
- [x] Flow-definition parsing — NiFi versioned-flow-snapshot (JSON) with auto-detect fall-through to MiNiFi Config Version 3 (YAML).
- [x] C2 operation dispatch — `DESCRIBE/manifest` re-send and `UPDATE/configuration` fetch + parse + engine apply. EFM 2.x ack is delivered implicitly via the next heartbeat's `flowInfo.flowId`.
- [x] Durable storage substrate — LittleFS mount, `IRepository` interface, `LittleFSRepository` with watermark eviction (DropOldest / BackPressure / FailWrites), storage metrics in the EFM heartbeat. Default partition layout in `partitions.csv` sizes for ~30 days offline.
- [ ] Engine queue integration — replay queued FlowFiles on boot, persist on enqueue, erase on successful ack.
- [ ] SD card overflow tier — Kconfig surface is in place (`MICROFI_SD_OVERFLOW`); `SdRepository` / `TieredRepository` implementations are Phase 2.
- [ ] Per-flow retention policy override — read connection-level retention settings from the EFM flow definition and apply per-connection.

## Layout

```
include/microfi/        Public headers — the API surface of the engine.
src/                    Engine implementation, WiFi, C2, main.
src/processors/         Compile-time-embedded processors.
vendor/cjson/           Vendored DaveGamble/cJSON (MIT) — see vendor/cjson/LICENSE.
boards/                 Custom PlatformIO board JSON for the Lonely Binary ESP32-S3 N16R8.
scripts/                Dev-loop helpers (PowerShell + bash; see scripts/secrets.local.*.example).
docs/                   Design rationale, processor inventory, demo-kit architecture.
sdkconfig.defaults      ESP-IDF defaults committed to the repo.
platformio.ini          PlatformIO build environments (esp32s3 default, esp32-c3 for size checks).
partitions.csv          Custom partition table — OTA + ~12 MB LittleFS for durable storage.
```

## Building

Prerequisites: [VS Code](https://code.visualstudio.com/) + the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode). PlatformIO will install the ESP-IDF toolchain on first build. Works on Windows, macOS, and Linux.

The primary target is the Lonely Binary ESP32-S3 N16R8 (16 MB flash, 8 MB OPI PSRAM). Its board definition is shipped in `boards/lonely-binary-esp32s3-n16r8.json` and is picked up automatically by the default `esp32s3` PlatformIO environment.

```sh
# Build (uses esp32s3 by default)
pio run

# Build + flash + open the serial monitor
pio run -t upload -t monitor
```

The firmware binary lands at `.pio/build/esp32s3/firmware.bin`.

A second environment, `esp32-c3`, is provided to confirm the binary still fits on a tighter target — it builds but is not the primary deployment platform.

## Configuration

Edit `sdkconfig.defaults` (or `pio run -t menuconfig` → "MicroFi configuration") to set:

- `CONFIG_MICROFI_WIFI_SSID` — placeholder by default; set to your WiFi network's SSID before flashing.
- `CONFIG_MICROFI_WIFI_PASSWORD` — defaults to empty (open network); set for WPA2.
- `CONFIG_MICROFI_C2_HEARTBEAT_URL` — full heartbeat URL. Default targets Cloudera EFM 2.x at `http://localhost:10090/efm/api/c2-protocol/heartbeat`.
- `CONFIG_MICROFI_C2_ACK_URL` — operation-ack URL. Currently unused: EFM 2.x treats a heartbeat whose `flowInfo.flowId` matches the pushed flow UUID as the ack. Reserved for future operation types (e.g. `RESTART`, `CLEAR`) that require an explicit POST.
- `CONFIG_MICROFI_AGENT_CLASS` — agent class reported to EFM; defaults to `default`.
- `CONFIG_MICROFI_AGENT_ID` — leave empty to derive `microfi-<mac>` from the eFuse MAC, or set a fixed override.
- `CONFIG_MICROFI_HEARTBEAT_INTERVAL_MS` — defaults to 30000.

Storage layer settings (under the "MicroFi storage" menu):

- `CONFIG_MICROFI_LITTLEFS_HIGH_WATER_PCT` / `CONFIG_MICROFI_LITTLEFS_LOW_WATER_PCT` — eviction watermarks for the durable queue. Defaults 80/70 keep writes in the predictable-performance band.
- `CONFIG_MICROFI_RETENTION_DROP_OLDEST` / `..._BACK_PRESSURE` / `..._FAIL_WRITES` — default retention policy for new connections. `DropOldest` is the right answer for sensor-fed flows (the source can't pause); `Bac