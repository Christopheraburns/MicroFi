# MicroFi

An exploratory microcontroller-class data ingestion agent that participates in the Apache MiNiFi C2 ecosystem. See [`MICROFI_ASSESSMENT.md`](docs/MICROFI_ASSESSMENT.md) for the design rationale.

This repository is a clean-room reimplementation of the MiNiFi protocol contracts (FlowFile semantics, C2 heartbeat/ack) targeted at ESP32 first. The upstream `nifi-minifi-cpp` source is a reference, not a starting tree.

## Status

Pre-alpha scope:

- [x] Project skeleton (PlatformIO + ESP-IDF).
- [x] Core types: `FlowFile`, `Queue`, `Session`, `Processor`, static `Registry`.
- [x] Minimal flow engine (single-task scheduler, hard-wired graph).
- [x] WiFi station bring-up.
- [x] HTTPS heartbeat POST to a configurable C2 URL (mbedTLS via `esp_http_client`).
- [x] Nine embedded processors: `GenerateFlowFile`, `LogAttribute`, `UpdateAttribute`, `PublishMQTT`, `GetGPIO`, `SetGPIO`, `ListenHTTP`, `CaptureImage`, `PublishSparkplug` — see [`docs/Processor-Inventory-And-Roadmap.md`](docs/Processor-Inventory-And-Roadmap.md#shipped) for what each does and how it maps to the roadmap.
- [x] Flow-definition parsing — NiFi versioned-flow-snapshot (JSON) with auto-detect fall-through to MiNiFi Config Version 3 (YAML).
- [x] C2 operation dispatch — `DESCRIBE/manifest` re-send and `UPDATE/configuration` fetch + parse + engine apply, followed by an explicit acknowledge POST (`FULLY_APPLIED` / `NOT_APPLIED`) to `CONFIG_MICROFI_C2_ACK_URL`.
- [x] Durable storage substrate — LittleFS mount, `IRepository` interface, `LittleFSRepository` with watermark eviction (DropOldest / BackPressure / FailWrites), storage metrics in the EFM heartbeat. Default partition layout in `partitions.csv` sizes for ~30 days offline.
- [ ] Engine queue integration — replay queued FlowFiles on boot, persist on enqueue, erase on successful ack.
- [ ] SD card overflow tier — Kconfig surface is in place (`MICROFI_SD_OVERFLOW`); `SdRepository` / `TieredRepository` implementations are Phase 2.
- [ ] Per-flow retention policy override — read connection-level retention settings from the EFM flow definition and apply per-connection.

## Layout

```
include/microfi/                 Public headers — the API surface of the engine.
src/                             Engine implementation, WiFi, C2, main.
src/processors/                  Compile-time-embedded processors.
vendor/cjson/                    Vendored DaveGamble/cJSON (MIT) — see vendor/cjson/LICENSE.
boards/                          Custom PlatformIO board JSON for the Lonely Binary ESP32-S3 N16R8.
scripts/                         Dev-loop helpers (PowerShell + bash; see scripts/secrets.local.*.example).
docs/                            Design rationale, processor inventory.
sdkconfig.defaults               Non-secret ESP-IDF project defaults (stack sizes, LittleFS config). Committed.
sdkconfig.defaults.local         Your WiFi credentials and EFM URLs. Gitignored — copy from .example.
sdkconfig.defaults.local.example Template for sdkconfig.defaults.local.
platformio.ini                   Build environments: esp32s3 (default), esp32s3-4mb, esp32-c3.
partitions.csv                   16 MB partition layout — dual OTA + ~11.5 MB LittleFS.
partitions_4mb.csv               4 MB partition layout — single app slot + ~2.4 MB LittleFS.
```

## Building

Prerequisites: [VS Code](https://code.visualstudio.com/) + the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode). PlatformIO will install the ESP-IDF toolchain on first build. Works on Windows, macOS, and Linux.

MicroFi was built and tested on a ESP32-S3 N16R8 (16 MB flash, 8 MB OPI PSRAM) from Lonely Binary. Its board definition is shipped in `boards/lonely-binary-esp32s3-n16r8.json` and is picked up automatically by the default `esp32s3` PlatformIO environment.

```sh
# Build (uses esp32s3 by default)
pio run

# Build + flash + open the serial monitor
pio run -t upload -t monitor
```

The firmware binary lands at `.pio/build/esp32s3/firmware.bin`.

Three build environments are defined in `platformio.ini`:

| Environment   | Board                        | Flash  | LittleFS  | PSRAM    |
|:--------------|:-----------------------------|:------:|:---------:|:--------:|
| `esp32s3`     | Lonely Binary ESP32-S3 N16R8 | 16 MB  | ~11.5 MB  | 8 MB OPI |
| `esp32s3-4mb` | Generic ESP32-S3 (DevKitC-1) | 4 MB   | ~2.4 MB   | —        |
| `esp32-c3`    | ESP32-C3 DevKitM-1           | 4 MB   | None      | —        |

```sh
# Flash a specific environment
pio run -e esp32s3-4mb -t upload
pio run -e esp32-c3 -t upload
```

## Configuration

Copy `sdkconfig.defaults.local.example` to `sdkconfig.defaults.local` and fill in your values before the first build. This file is gitignored and holds your WiFi credentials and EFM endpoint URLs. Non-secret project defaults (stack sizes, LittleFS watermarks, etc.) live in the committed `sdkconfig.defaults`. Both can also be edited via `pio run -t menuconfig` → "MicroFi configuration".

Settings in `sdkconfig.defaults.local`:

- `CONFIG_MICROFI_WIFI_SSID` — your WiFi network SSID.
- `CONFIG_MICROFI_WIFI_PASSWORD` — WPA2 passphrase; leave empty for an open network.
- `CONFIG_MICROFI_C2_HEARTBEAT_URL` — full heartbeat URL. Default targets Cloudera EFM 2.x at `http://localhost:10090/efm/api/c2-protocol/heartbeat`.
- `CONFIG_MICROFI_C2_ACK_URL` — operation-ack URL. Currently unused: EFM 2.x treats a heartbeat whose `flowInfo.flowId` matches the pushed flow UUID as the ack. Reserved for future operation types (e.g. `RESTART`, `CLEAR`) that require an explicit POST.

Additional settings in `sdkconfig.defaults`:

- `CONFIG_MICROFI_AGENT_CLASS` — agent class reported to EFM; defaults to `default`.
- `CONFIG_MICROFI_AGENT_ID` — leave empty to derive `microfi-<mac>` from the eFuse MAC, or set a fixed override.
- `CONFIG_MICROFI_HEARTBEAT_INTERVAL_MS` — defaults to 30000.

Storage layer settings (under the "MicroFi storage" menu):

- `CONFIG_MICROFI_LITTLEFS_HIGH_WATER_PCT` / `CONFIG_MICROFI_LITTLEFS_LOW_WATER_PCT` — eviction watermarks for the durable queue. Defaults 80/70 keep writes in the predictable-performance band.
- `CONFIG_MICROFI_RETENTION_DROP_OLDEST` / `..._BACK_PRESSURE` / `..._FAIL_WRITES` — default retention policy for new connections. `DropOldest` is the right answer for sensor-fed flows (the source can't pause); `BackPressure` matches NiFi defaults; `FailWrites` is mostly for tests.
- `CONFIG_MICROFI_SD_OVERFLOW` — reserve the Kconfig surface for adding an SD card as a second-tier overflow store. Implementation is Phase 2; enabling today logs a warning and falls back to LittleFS-only.
- `CONFIG_MICROFI_STORAGE_METRICS` — emit fill percentages and eviction counters under `flowInfo.microfi` in the heartbeat. Recommended on; turning it off opts out of operational visibility into silent evictions.

## Durability story

The agent ships with a custom partition table (`partitions.csv`) that carves the 16 MB of flash into a standard ESP-IDF bootloader region, two 2 MB OTA app slots (so EFM-pushed firmware updates work with rollback), and a ~12 MB LittleFS data partition for durable FlowFile storage. LittleFS was chosen over SPIFFS or FATFS for its crash-safe metadata updates — a power cut mid-write cannot corrupt the filesystem.

Out of the box this sizes the agent for roughly **30 days of offline durability** under typical edge-sensor data rates (heartbeats, environmental sensors, event-triggered CSI features). When LittleFS usage crosses 80%, the configured retention policy kicks in: `DropOldest` (the default) evicts oldest records until usage drops back below 70%, so writes never fail and sensors never get refused. Eviction counters are reported in every heartbeat under `flowInfo.microfi`, so an operator can see when a fleet is starting to evict and respond before it becomes systemic data loss.

For longer durability windows or higher-bandwidth workloads, the design supports an SD card as a second tier (LittleFS as the crash-safe recent tip, SD as bulk overflow). The Kconfig surface for this is already in place; the implementation is Phase 2.

## Connecting to a Cloudera EFM server

The agent sends a MiNiFi C2 protocol heartbeat (EFM 2.x envelope) to `CONFIG_MICROFI_C2_HEARTBEAT_URL`. The first heartbeat carries the full agent manifest derived from the static processor registry; subsequent heartbeats send only the manifest hash.

Operations sent back from EFM in the heartbeat response are dispatched immediately:

- **`DESCRIBE / manifest`** — flips an internal flag so the next heartbeat re-includes the full manifest.
- **`UPDATE / configuration`** — resolves the flow-definition URL from `args.location` → `args.flowUrl` → `args.configuration` → `args.url`, falling back to a constructed `configContent/<op-id>` URL if none are present. The body is fetched, format-detected (NiFi versioned-flow-snapshot JSON or MiNiFi Config Version 3 YAML), parsed into a `FlowDef`, and applied to the running flow engine. The flow UUID is recovered from the URL when the payload doesn't carry one explicitly.

After the apply (or a failed fetch/parse/apply), the agent POSTs an explicit acknowledge to `CONFIG_MICROFI_C2_ACK_URL`: `{"operationId": …, "operationState": {"state": "FULLY_APPLIED" | "NOT_APPLIED", "details": …}}`. EFM 2.x maps `FULLY_APPLIED` to operation state DONE and anything else to FAILED — it does **not** honor an implicit ack via the heartbeat's `flowInfo.flowId` (an unacknowledged operation times out to FAILED server-side). The ack body deliberately omits `agentInfo`/`deviceInfo`/`flowInfo`; including any of them makes EFM additionally process the ack as a heartbeat.

Reachability: **`localhost` in the default URL won't work from a real ESP32** — to the device, `localhost` is itself, not your dev box. Two paths to a reachable EFM:

1. **EFM on your LAN.** Set `CONFIG_MICROFI_C2_HEARTBEAT_URL` to `http://<efm-host-lan-ip>:10090/efm/api/c2-protocol/heartbeat` and make sure EFM is bound to `0.0.0.0:10090` (or that LAN IP) rather than `127.0.0.1`.
2. **EFM behind a tunnel.** Run `ngrok http 10090` (or cloudflared, tailscale-funnel) on the EFM host and put the resulting public URL in the heartbeat setting.

Once the heartbeat reaches EFM the agent auto-registers and appears in the EFM UI under the configured agent class.

## Hardware Requirements

MicroFi targets three capability tiers. Each tier is a strict superset of the one before it.

---

### Tier 1 — Agent registration + base flow (`GenerateFlowFile` → `LogAttribute`)

**Minimum: any ESP32 variant with WiFi and ≥ 4 MB flash.**

The confirmed minimum is the **ESP32-C3** (4 MB flash, 400 KB SRAM, no PSRAM). The firmware binary is approximately 1.5–2 MB, leaving the remainder of flash for the partition table and NVS. Active heap usage during a heartbeat cycle (WiFi stack, LwIP, mbedTLS, cJSON body) peaks at roughly 180–200 KB, which fits within the C3's 400 KB SRAM with margin. No PSRAM is required at this tier.

Flow definitions are **not** persisted across reboots at this tier — if the device power-cycles before EFM delivers a flow, it boots on the default graph. This is the `esp32-c3` build environment.

---

### Tier 2 — Tier 1 + LittleFS durability

**Minimum: any ESP32 with WiFi and ≥ 4 MB flash.**

LittleFS itself adds only ~6–8 KB of SRAM overhead, so the memory bar does not change meaningfully. The constraint is purely flash: a LittleFS partition needs space alongside the app slot and NVS. Two partition layouts are provided depending on flash size:

| Build environment | Flash | LittleFS capacity | OTA support |
|:------------------|:-----:|:-----------------:|:-----------:|
| `esp32s3-4mb`     | 4 MB  | ~2.4 MB           | No          |
| `esp32s3`         | 16 MB | ~11.5 MB          | Yes (2 × 2 MB slots) |

At this tier the agent survives a power cycle with its last-known flow definition and all in-flight FlowFiles intact — EFM does not need to re-push a flow on reconnect. The ESP32-S3 is the recommended platform for any deployment that requires durability.

---

### Tier 3 — Physical AI Data Fabric (P0 processor set)

**Minimum: ESP32-S3 with ≥ 8 MB PSRAM and ≥ 16 MB flash.**  
**Reference platform: [Lonely Binary ESP32-S3 N16R8](https://lonelybinary.com/products/esp32-s3-n16r8).**

The P0 processor set (see [`docs/Processor-Inventory-And-Roadmap.md`](docs/Processor-Inventory-And-Roadmap.md)) drives three hard requirements beyond Tier 2:

**PSRAM is required.** `WindowCSI` buffers CSI packet windows as `(T × subcarriers × 2)` float tensors. At 20 MHz with 64 active subcarriers and a 200-packet window, a single window is ~100 KB. Concurrent windows, IMU sample buffers, and TFLite Micro model weights push well past the ESP32-S3's 512 KB internal SRAM. The 8 MB OPI PSRAM on the reference board provides the working memory needed.

**16 MB flash is required.** TFLite Micro models for motion and gesture classification are typically 50–500 KB. Combined with dual OTA slots (2 × 2 MB), the LittleFS retention tier, and the firmware binary, 16 MB is the practical floor.

**ESP32-S3 specifically is required** (not plain ESP32 or C3). The S3's hardware FPU makes CSI amplitude/phase extraction and FFT viable at sensor data rates. `GetCamera` and `RunESPDLModel` (Espressif's accelerated inference runtime) are S3-only by design.

| Processor | Why S3 + PSRAM is required |
|:----------|:---------------------------|
| `WindowCSI` | ~100 KB tensor per window exceeds internal SRAM |
| `GetCamera` | Frame buffer for JPEG / RGB565 output |
| `RunTFLiteMicro` | Model weights + activation buffers |
| `RunESPDLModel` | S3 SIMD acceleration; not available on other variants |
| `FFTContent` / `ComputeDopplerSpectrum` | FPU throughput required at sensor data rates |

---

## Next steps

See [`docs/Processor-Inventory-And-Roadmap.md`](docs/Processor-Inventory-And-Roadmap.md) for the full processor inventory and Physical AI Data Fabric build roadmap.
