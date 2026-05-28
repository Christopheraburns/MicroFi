# MicroFi: Feasibility Assessment for Porting Apache MiNiFi C++ to Microcontrollers

**Author:** Chris Burns (assessment drafted with Claude)
**Date:** 2026-05-21
**Status:** Draft v1 — research assessment, pre-prototype
**Scope:** Determine what it would take to rewrite Apache MiNiFi C++ as "MicroFi," a microcontroller-class data ingestion agent that still participates in the MiNiFi C2 ecosystem.

---

## 1. Executive Summary

Apache MiNiFi C++ (`nifi-minifi-cpp`) is already the "small" edition of NiFi — a ~3.2 MB native binary that idles around 5 MB of RAM on Linux. That makes it Raspberry-Pi-class, not microcontroller-class. To run on something Arduino-shaped, the agent has to shrink by roughly two orders of magnitude on the smallest tiers and by about one order of magnitude on the mid tier.

Two design conclusions drive the rest of this document:

1. **Not all microcontrollers are reachable.** Classic 8-bit AVR (Arduino Uno/Mega) is out of scope for anything that speaks the MiNiFi C2 protocol over HTTPS — TLS alone exceeds the entire SRAM of these chips. ESP32 and 32-bit Cortex-M class parts are the realistic targets. AVR is only viable as a *sensor leaf* behind a gateway, not as a standalone agent.
2. **A direct port is not the right shape.** The MiNiFi C++ codebase is built around assumptions — heap allocation, file-backed repositories, a plug-in architecture, a flow-file content store, dynamic processor loading — that don't fit a microcontroller. MicroFi should be a **clean-room reimplementation of the protocol contracts** (flow definition, C2 heartbeat/ack, FlowFile semantics) rather than a line-by-line port. The C++ source becomes a reference implementation, not the starting tree.

The high-confidence path is: ESP32-first MicroFi, compile-time-embedded processors, a minimal C2 subset over HTTPS, RAM-backed repositories with optional flash spillover, and a gateway-mediated path for the smallest devices.

Estimated effort for an MVP that completes a working C2 round trip on an ESP32 with two embedded processors is roughly **3–5 engineer-months**. A "feature-respectable" MicroFi covering ESP32 + a Cortex-M4 reference port with a handful of processors and durable queueing is closer to **9–12 engineer-months**.

---

## 2. Scope and Goals

**In scope.**

- A new codebase — provisionally `microfi` — written in C or constrained C++ that compiles on the toolchains of the target families (Arduino-ESP32/ESP-IDF, Zephyr, FreeRTOS, optionally Arduino-AVR for sensor-leaf mode).
- Wire-compatible C2 participation: the device shows up in a CEM / MiNiFi C2 server as an agent, sends heartbeats, accepts at least flow updates and a minimum operation set.
- A flow engine that can run a small, fixed set of processors compiled into the firmware image.
- A path to durable FlowFile semantics where the storage medium permits it (SPIFFS/LittleFS on ESP32, internal flash sectors on Cortex-M).

**Out of scope (for now).**

- Dynamic loading of processors at runtime. On microcontrollers there is no `dlopen`, and even runtime parsing of arbitrary new processor logic from C2 is unrealistic at this scale.
- Full Site-to-Site over raw socket protocol with the legacy framing. HTTP(S) S2S, or replacement with MQTT, is the realistic transport.
- Provenance lineage in the NiFi sense. Microcontrollers cannot afford a write-amplified provenance log.
- Multi-tenant or hot-config-reload semantics that NiFi normally provides.

**Success criteria for an MVP.**

1. ESP32 firmware boots, joins WiFi, performs TLS handshake to the C2 server.
2. Sends an initial heartbeat; receives and applies a flow definition.
3. Runs a `GenerateFlowFile`-equivalent and an `InvokeHTTP`-equivalent processor.
4. Heartbeat carries useful metrics (free heap, queue depth, last-error).
5. Total static footprint comfortably under 50% of the chip's flash; runtime RAM headroom of at least 50 KB.

---

## 3. Current MiNiFi C++ Baseline

This section establishes the gap MicroFi has to close.

### 3.1 Architecture

The current C++ agent is organized around a `FlowController` that loads a flow definition (YAML), instantiates a tree of `ProcessGroup`s containing `Processor`s, wires queues between them, and schedules them via timer/event triggers. Each processor consumes/produces `FlowFile`s, which are records of attributes plus an opaque content pointer that lives in a content repository. Three repositories underpin the system:

- **Flow file repository** — durable record of in-flight FlowFile attributes and queue placement (RocksDB by default).
- **Content repository** — the actual payload bytes (filesystem by default, with optional RocksDB or volatile).
- **Provenance repository** — lineage events for auditing (RocksDB).

A C2 client runs on a periodic timer, building a heartbeat JSON payload, POSTing it to a configured C2 URL, and applying any operations the server returns (update properties, replace flow, start/stop, etc.).

### 3.2 Default dependencies

A minimum Linux build pulls in, at least: OpenSSL, libcurl (curl + nghttp2 + zlib), RocksDB (which brings snappy/lz4/zstd/bzip2 transitively), libuuid, and bz2/xz. RocksDB alone is multi-megabyte and is fundamentally a flash-friendly LSM-tree database designed for SSDs, not flash sectors with limited erase cycles. OpenSSL is the largest single contributor to binary size after RocksDB.

### 3.3 Footprint

Public Apache and Cloudera material lists the C++ agent at ~3.2 MB on disk and roughly 5 MB resident at idle on Linux, versus ~49 MB and ~24 MB JVM-heap-default for the Java agent. The 3.2 MB figure assumes a fairly trim build; enabling more extensions easily pushes it past 10 MB.

### 3.4 Why the existing codebase is hard to port directly

- **Heavy use of the C++ standard library and exceptions.** `std::string`, `std::vector`, `std::shared_ptr`, RTTI, and exceptions are pervasive. Several embedded toolchains either disable these (avr-gcc, some Cortex-M0 builds) or charge a steep code-size price for them.
- **Heap-centric design.** Every FlowFile, attribute map, and processor instance is heap-allocated. On AVR there is essentially no heap; on Cortex-M0/M3 the heap is small and fragmentation is a real failure mode for long-running agents.
- **Filesystem-shaped repositories.** RocksDB, filesystem content repo, RocksDB provenance — none of these have an obvious port to microcontroller flash. LittleFS exists on ESP32 / Cortex-M, but it's a different data model.
- **Plugin / extension system.** The C++ agent supports extensions and dynamic processor loading; on a microcontroller, everything must be statically linked.

These aren't show-stoppers, but they justify the recommendation that MicroFi is a reimplementation rather than a fork.

---

## 4. Microcontroller Tier Constraints

Three tiers cover the spectrum the user named.

### 4.1 Tier 1 — Classic Arduino / AVR

- **Representative parts:** ATmega328P (Uno), ATmega2560 (Mega).
- **Flash:** 32 KB (Uno; ~31.5 KB after bootloader) / 256 KB (Mega; 248 KB usable).
- **SRAM:** 2 KB (Uno) / 8 KB (Mega).
- **EEPROM:** 1 KB / 4 KB.
- **Networking:** none native; requires a shield (W5500 Ethernet, ESP-01 as WiFi co-processor) that takes its own RAM/flash share.
- **TLS:** infeasible. mbedTLS at minimum needs tens of KB of RAM just for one handshake (handshake heap measured around ~36 KB after optimization on ESP32; ~60 KB without).

**Verdict.** AVR cannot host a standalone MicroFi that talks the C2 protocol. Even non-TLS heartbeat over plain HTTP is uncomfortable on a Uno once you also need to parse JSON and hold a buffered request/response.

**Where AVR fits.** As a *sensor leaf* speaking a trivial binary or text protocol (CBOR over UART/RS-485/CAN) to a nearby ESP32 or Cortex-M gateway, which is the real MicroFi agent. This pattern is realistic and worth designing for.

### 4.2 Tier 2 — ESP32 / ESP8266 (recommended primary target)

- **Representative parts:** ESP32, ESP32-S3, ESP32-C3, ESP8266 (lower bound).
- **Flash:** typically 4 MB on common modules; up to 16 MB.
- **SRAM:** ~520 KB on ESP32 (DRAM + IRAM, ~320 KB usable for application after WiFi/BT stacks); 80 KB on ESP8266.
- **Optional PSRAM:** 4–16 MB external; useful but slower than internal SRAM.
- **Networking:** WiFi (and BT on classic ESP32) built in. Mature mbedTLS integration in ESP-IDF.
- **RTOS:** FreeRTOS bundled.
- **TLS cost:** ESP-IDF mbedTLS uses ~32–35 KB at typical configuration, with ~16 KB TX + 16 KB RX content buffers by default (configurable smaller if both sides negotiate `max_fragment_length`).

**Verdict.** This is where MicroFi clearly works. Even after WiFi/TLS overhead there's headroom for a flow engine, a handful of processors, and durable spool storage on flash. ESP32-C3 is the most cost-effective sweet spot; ESP32-S3 if compute headroom or PSRAM is needed.

ESP8266 is borderline — runnable for a minimal "heartbeat + one processor" build, but the 80 KB SRAM leaves very little headroom once mbedTLS is loaded. Treat it as best-effort.

### 4.3 Tier 3 — ARM Cortex-M (mid)

- **Representative parts:** STM32F4/F7/H7, NXP i.MX RT, Teensy 4.x (i.MX RT 1062, Cortex-M7), RP2040 (Cortex-M0+).
- **Flash:** 256 KB up to several MB.
- **SRAM:** 64 KB to 1 MB+.
- **Networking:** typically not on-die — needs an external PHY (Ethernet) or a WiFi module (ESP-AT, ublox).
- **RTOS:** Zephyr or FreeRTOS commonly used; CMSIS-RTOS for the bare-Cortex world.

**Verdict.** Cortex-M4/M7 parts with >=256 KB SRAM are comfortable hosts. RP2040 is doable for the engine but networking requires careful pairing (typically with a WiFi chip on the side, as on the Pico W). Teensy 4.x has more headroom than most ESP32 modules but lacks built-in WiFi.

### 4.4 Tier matrix at a glance

| Capability                       | AVR (Uno/Mega) | ESP8266 | ESP32 / S3 / C3 | Cortex-M4 ≥256KB | Cortex-M7 |
|----------------------------------|----------------|---------|------------------|------------------|-----------|
| TLS to C2 server                 | No             | Tight   | Yes              | Yes              | Yes       |
| In-RAM FlowFile queue            | <=4 items, tiny | Modest | Yes              | Yes              | Yes       |
| Durable spool on flash           | EEPROM only    | SPIFFS  | LittleFS/SPIFFS  | LittleFS         | LittleFS  |
| Multiple processors per build    | 1–2            | 2–3     | 6–10             | 8–12             | 12+       |
| JSON config parsing              | Painful        | Yes (small) | Yes          | Yes              | Yes       |
| Standalone C2 agent              | No (leaf only) | Marginal | **Yes**         | **Yes**          | Yes       |

---

## 5. Component-by-Component Porting Plan

For each MiNiFi C++ subsystem the disposition is one of: **drop**, **replace**, **port** (with effort tier T1 trivial, T2 moderate, T3 significant).

### 5.1 Flow engine (FlowController, ProcessGroup, scheduling)

- **Disposition:** Replace (T2).
- A microcontroller flow engine should be a static graph of processors connected by bounded queues, scheduled by a single FreeRTOS / Zephyr task (or a cooperative scheduler if no RTOS). The MiNiFi notion of "session" — atomic commit of attribute/content/queue changes after a processor returns — is worth keeping because it is what makes recovery sane after a crash mid-run.
- Eliminate ProcessGroup nesting beyond two levels. Eliminate runtime processor instantiation.

### 5.2 FlowFile

- **Disposition:** Port with constraints (T2).
- Keep the attribute-map + content-handle data model — it is the value proposition of NiFi semantics and is what makes flows portable from desktop MiNiFi to MicroFi.
- Cap attributes per FlowFile (e.g., 8) and per-attribute size (e.g., 64 B), so a FlowFile header is fixed size and can live in a slab allocator.
- Content handles should be small-content-inline (e.g., up to 256 B in the FlowFile struct) with optional spill to flash-backed content repository for larger payloads.

### 5.3 Repositories

- **Flow-file repository:** Replace (T2). Use a fixed-size ring buffer in RAM with an optional write-through to a small append-only journal on flash (LittleFS, or a raw flash sector pair with the classic A/B-erase pattern). RocksDB is not appropriate here.
- **Content repository:** Replace (T2). For payloads beyond the inline-FlowFile threshold, write to LittleFS as one file per content blob, GC on commit. On parts with no filesystem (RP2040 bare flash, AVR EEPROM), restrict to inline-only.
- **Provenance repository:** Drop in MVP (T1). Optionally support compact, sampled provenance later — a 32-byte record per terminal event, in a bounded ring.

### 5.4 Processors

- **Disposition:** Replace, but keep the contract (T2 per processor).
- The MiNiFi C++ processor library is large and Linux-centric (ExecuteProcess, GetFile, ListSFTP, etc., all assume an OS). MicroFi should ship a much smaller, embedded-appropriate processor set, each compiled in by `#ifdef`:
  - `GenerateFlowFile` — fixed-content emitter, useful as a sentinel.
  - `ReadSensor` — new processor; reads I2C/SPI/ADC into a FlowFile. (This is what makes MicroFi worth doing.)
  - `RouteOnAttribute` — small expression evaluator (numeric and string compare; no full NiFi EL).
  - `UpdateAttribute` — static literal substitution.
  - `MergeContent` — bounded batch merger.
  - `InvokeHTTP` — outbound HTTPS via mbedTLS.
  - `PublishMQTT` — outbound MQTT.
  - `LogAttribute` / `PutSerial` — diagnostic sink.
- **Embedded-processor strategy:** all processors live in the same binary, declared with a registration macro that emits a static descriptor record. The flow definition references them by name; the loader resolves the name against the static table at flow-apply time. There is no `.so`/`.dll` loading.
- This deliberately departs from the "any processor at any time" NiFi model. It's the right trade for the platform.

### 5.5 Site-to-Site

- **Disposition:** Drop the raw-socket S2S, retain HTTP S2S as optional (T3 to fully implement).
- For most MicroFi deployments, the outbound transport will be `InvokeHTTP` to a NiFi listener, or `PublishMQTT` to a broker that NiFi pulls from via `ConsumeMQTT`. This is operationally simpler and friendlier to firewalls. Implementing true S2S framing on a microcontroller is feasible but not high value for the MVP.

### 5.6 C2 client

- **Disposition:** Port the protocol, replace the implementation (T2).
- Hand-rolled HTTPS heartbeat using mbedTLS + a tiny HTTP client (no libcurl). JSON encoded with a single-pass serializer (no DOM); parse responses with a small streaming JSON parser (e.g., jsmn or `json-c-mini`). Section 6 covers protocol trade-offs.

### 5.7 Configuration

- **Disposition:** Replace (T1).
- MiNiFi C++ accepts YAML and a newer JSON-form flow. YAML is impractical to parse on a microcontroller; **MicroFi takes JSON-only flow definitions**, and ideally a CBOR-encoded version for delivery over C2 to save bytes and parse cost.
- Properties files can be replaced with a single key-value blob in flash.

### 5.8 Logging

- **Disposition:** Replace (T1).
- One small logger with compile-time level filtering and a ring buffer the C2 client can sample for "last N log lines" in heartbeats. Drop spdlog.

### 5.9 TLS / crypto

- **Disposition:** Replace (T1).
- mbedTLS for ESP32 (already in ESP-IDF), mbedTLS or wolfSSL for Cortex-M. OpenSSL is too large for this class of device.

### 5.10 Build / toolchain

- **Disposition:** New (T2).
- CMake top-level with per-platform toolchain files (esp-idf, gcc-arm-none-eabi, avr-gcc), feature flags per processor, and a "size budget" CI job that fails the build if footprint exceeds a configured threshold for the target.

---

## 6. C2 Protocol Trade-offs

The C2 protocol is the contract that makes a device a MiNiFi agent rather than just a thing-that-talks-MQTT. Three viable strategies:

### Option A — Full MiNiFi C2 over HTTPS

The device speaks the documented C2 heartbeat/ack JSON protocol directly to the server. Maximum interoperability with existing CEM-style C2 servers; the device shows up the same as any other agent.

- **Cost:** mbedTLS (~32–35 KB RAM, plus ~32 KB for default content buffers; reducible with `max_fragment_length`), a JSON serializer/parser, a small HTTP/1.1 client. Plausibly fits in well under 100 KB of code on ESP32. RAM headroom on ESP32 is comfortable but you will feel it on ESP8266.
- **Pro:** Drop-in replacement for desktop MiNiFi from the server's perspective. No new server-side work.
- **Con:** Heaviest path on the device. Every heartbeat pays for a TLS session (or session resumption) and JSON serialization. Requires the device to hold a trusted CA, which complicates provisioning.

### Option B — Minimal C2 subset

Device implements only heartbeat + receive-flow + acknowledge-operation, against the same JSON envelope. Operations like "request asset," "transfer debug bundle," or "run diagnostic" are unsupported and return a "not implemented" ack.

- **Cost:** Roughly the same as Option A — the cost driver is TLS, not the message catalog.
- **Pro:** Cleaner code, smaller test surface, less to break.
- **Con:** A server-side admin sees an oddly limited agent; some tooling that assumes full op coverage may not work.

This is the **recommended default** for MicroFi v1.

### Option C — Gateway-mediated C2

Device speaks MQTT (or CoAP) to a nearby gateway — a Raspberry Pi or another ESP32 — running a "MicroFi proxy" that aggregates many devices and represents them to the C2 server as either one or many logical agents.

- **Cost:** Gateway is full MiNiFi C++. Device side is just MQTT + an envelope for "command-from-server" / "telemetry-to-server."
- **Pro:** Smallest possible device footprint. TLS-PSK is plausible instead of full PKI. Devices can be Cortex-M0+, even AVR-leaf-class behind a serial bus to the gateway.
- **Pro:** Fleet provisioning concentrates at the gateway, which is much easier than provisioning each MCU with a CA bundle.
- **Con:** Introduces a new component (the proxy/translator) that has to be designed, written, and operated. Until that proxy is real, devices are not "real" MiNiFi agents.

### Recommendation

Implement **Option B** first on ESP32 (proves the agent is a real MiNiFi participant). Add **Option C** second to bring sub-ESP32 hardware and ultra-low-power profiles into the family. Reserve **Option A** as a label for "Option B that happens to cover the long tail of operations" — i.e., it's the same code path, just with the op set fleshed out over time.

---

## 7. Embedded-Processor Strategy

A short, separate section because the user flagged this explicitly.

The MiNiFi C++ extension model — dynamic, where processors are discovered at runtime — is incompatible with the target platform. The replacement model:

1. **Processors are first-class build-time features.** Each processor has a CMake option (`MICROFI_PROC_INVOKE_HTTP=ON`), a single `.c` or `.cc` file, and a registration macro at file scope that adds a `ProcessorDescriptor` to a `__attribute__((section(".microfi_procs")))` array.
2. **The flow-loader resolves processor names against this static table.** A flow definition that references an unknown processor is rejected at apply-time and reported back through the C2 ack, so the operator sees that this device's build doesn't include `PublishMQTT`.
3. **Heartbeat advertises capabilities.** The C2 heartbeat includes the list of compiled-in processor types and their property schema. The CEM server (or operator) uses this to decide which flows are valid for the device. This is how MicroFi keeps the value of MiNiFi's "configure from the server" model without paying for dynamic loading.
4. **A device-class manifest, generated at build time**, is the canonical record of "what this firmware image can do." Pushing a new flow that uses an unsupported processor fails fast; pushing a new firmware image is the path to gaining new processor types.

This converges with what people doing MCU agents have built independently elsewhere — it is the embedded-systems-native answer to the plug-in problem.

---

## 8. Effort Estimate

These are rough order-of-magnitude estimates for a small (1–2 engineer) team. They assume one of the engineers has both embedded C and MiNiFi familiarity.

| Phase | Scope | Effort |
|---|---|---|
| 0. Spike | Single-binary ESP32: WiFi join, TLS handshake to a stub C2 server, send a hand-coded heartbeat. No flow engine. | 2–3 weeks |
| 1. MVP | ESP32 + Option B C2 + 2 processors (`GenerateFlowFile`, `InvokeHTTP`) + in-RAM flow repo + JSON flow loader. Round-trip C2 flow update demonstrated. | 2–3 months |
| 2. Production-shaped v1 | LittleFS-backed durable flow + content repos; expression-language subset for `RouteOnAttribute`; 6 processors total; size-budget CI; documented protocol-conformance test against the reference C2 server. | +3–4 months |
| 3. Tier-3 reference port | Cortex-M4 (STM32F4) port behind the same build system; abstract the WiFi/TLS provider; reproduce the MVP end-to-end. | +2 months |
| 4. Gateway-mediated leaf mode | AVR sensor-leaf protocol + ESP32 "MicroFi proxy" mode that talks C2 upstream and proprietary leaf protocol downstream. | +2–3 months |

A 4-month MVP and a ~9–12-month "feature-respectable" v1 are the numbers to plan around. Most of the cost is in tests, the conformance suite, and the size-budget discipline — not in the flow engine itself, which is genuinely small.

---

## 9. Risks

**TLS dominates the budget.** On ESP8266 and Cortex-M0, mbedTLS plus the heartbeat machinery can crowd the available RAM. Mitigation: design for `max_fragment_length`, prefer session resumption, allow falling back to Option C on the tightest parts.

**Long-lived devices on flash storage.** LittleFS and SPIFFS handle wear leveling, but a misbehaving flow that thrashes the content repo will burn flash. Mitigation: cap content-repo writes per minute as a configurable safeguard, and prefer streaming content directly to the network rather than spooling when the flow allows.

**Flow definitions are an attack surface.** A C2 server (or anyone who can spoof it) can push a flow definition. On a desktop NiFi this is well-understood; on a device it deserves more scrutiny. Mitigation: signed flow definitions, certificate pinning to the C2 server, and a per-device pre-shared identity provisioned at manufacture.

**Memory fragmentation in long uptimes.** Heap fragmentation is a real failure mode for agents that run for months. Mitigation: slab allocator for FlowFile structs, fixed-size content buffers, and a watchdog that resets if the largest free block falls below a threshold.

**Drift from upstream MiNiFi.** The C2 schema and operation set will evolve. A reimplementation has to track that, and unlike a fork, won't get those changes for free. Mitigation: protocol-conformance test suite run against the official C2 server in CI; conservative subset rather than chasing every new feature.

**Cert/PKI provisioning at fleet scale.** Putting per-device certs onto thousands of MCUs is its own engineering problem. Mitigation: secure-element chips (ATECC608A, NXP SE050) on the BOM where the threat model justifies the cost; otherwise PSK / shared-CA with strong device-identity attributes.

**"It's already C++" misleads the estimate.** Treating MicroFi as a porting project rather than a reimplementation will produce something that compiles and is unusable. Mitigation: the recommendation in §1 — clean-room MicroFi from the protocol contract, reference the existing source.

---

## 10. Recommended Path

1. **Adopt the reimplementation framing.** Treat the existing `nifi-minifi-cpp` source as a reference for protocol semantics and processor behavior, not as a starting tree.
2. **Target ESP32 first.** It is the only platform where you can move quickly while still proving the hard parts (TLS, flow engine, C2 round trip). Everything else gets easier once that's done.
3. **Commit to compile-time-embedded processors and an advertised capability manifest.** Resist the temptation to make this dynamic; the static model is what makes the platform credible.
4. **Pick Option B C2 (minimal subset over HTTPS) for v1.** Design the code so Option A is a superset, and so the wire is shaped to support Option C (gateway-proxied) as a later mode.
5. **Lock in a size-budget CI from day one.** Microcontroller agents do not slip back into budget once they have left it.
6. **Build a protocol-conformance test that runs your firmware against a real MiNiFi C2 server.** This is the test that catches the wrong kind of drift.
7. **Plan for a Cortex-M reference port as the second platform**, not the first. It validates the abstraction boundaries and surfaces ESP-specific assumptions early enough to fix.

If those choices hold up under a v0 spike — and there's no reason from the public material to think they won't — MicroFi is a credible project at a 4-month MVP / 12-month v1 horizon.

---

## Appendix A — Reference Material Consulted

- Apache NiFi MiNiFi project page and overview.
- `apache/nifi-minifi-cpp` README, CONFIGURE.md, PROCESSORS.md, C2.md, SITE_TO_SITE.md.
- Apache MiNiFi C2 Design wiki page.
- Cloudera CEM MiNiFi getting-started documentation (binary size / RAM idle figures).
- ESP-IDF programming guide — Minimizing RAM Usage; Mbed TLS configuration.
- mbedTLS documentation — reducing memory and storage footprint.
- Espressif blog — Optimizing RAM Usage on ESP32-C2.
- ATmega328P / ATmega2560 datasheet excerpts (Arduino docs, Microcenter, Seeed Studio).
- PJRC Teensy technical specs; STMicroelectronics STM32F4/H7 product pages; RP2040 datasheet.
- Eclipse Paho embedded MQTT C client documentation.

---

## Appendix B — Open Questions for the Next Iteration

1. Is the C2 server going to be Apache MiNiFi C2 reference, Cloudera CEM, or a homegrown server? Each has its own quirks in the heartbeat schema.
2. What is the maximum payload size that needs to flow through a MicroFi node? This sets the content-repo design more than anything else.
3. Is there a hard latency requirement on flow update propagation? That changes the heartbeat cadence and the choice between long-poll, push (MQTT broker), and pure polling.
4. Are there regulated-environment constraints (medical, automotive, industrial) that would force the choice of OS (Zephyr, FreeRTOS, ThreadX, safety-certified)?
5. Power profile — is the device line-powered, or duty-cycled on a battery? Battery-powered devices favor MQTT-with-QoS-1 and a long sleep between heartbeats, which biases toward Option C.
