# MicroFi Processor Inventory & Physical AI Data Fabric Roadmap

**Date:** 2026-05-27
**Author:** Chris Burns
**Purpose:** Catalog every processor in Apache MiNiFi C++ (the upstream we're porting from), assess ESP32 build feasibility for each, and propose the MicroFi-original processors needed to make the platform a useful node in a Physical AI Data Fabric — including dedicated CSI and WiFi-passive-sensing processors.

---

## Conventions

**Feasibility column (column 3):**

| Code              | Meaning |
| :---------------- | :------ |
| 🟢 **Yes**        | Compiles and runs on stock ESP32 (WROOM-class) within reasonable RAM/flash budgets. Direct port or near-direct port viable. |
| 🔵 **Yes (S3)**   | Needs ESP32-S3 with PSRAM or extra flash; impractical on plain ESP32 but fine on S3. |
| 🟡 **Partial**    | Compilable in a reduced or rewritten form (e.g., swap libcurl→esp_http_client, std::regex→re-lite, full archive lib→heatshrink, JVM jolt→omit). Often a different processor sharing the name. |
| 🔴 **No**         | Cannot reasonably compile on ESP32 — either the dependency tree is too large (AWS/Azure/GCP SDKs, OpenCV, libssh2, librdkafka), the platform is wrong (Linux-only `/proc`, Windows-only PDH/WEL/SMB), the runtime is incompatible (CPython, JVM), or the size class is wrong by 1-2 orders of magnitude (LlamaCpp). |

**Priority column (column 4)** in the proposed-processors table:

| Code    | Meaning |
| :------ | :------ |
| **P0**  | Build now — blocking for the CSI/Physical-AI thesis or for minimum viable fabric. |
| **P1**  | Build soon — needed for a credible end-to-end demo. |
| **P2**  | Build later — useful but not on the critical path. |

---

## Part 1 — Apache MiNiFi C++ Processor Inventory (89 processors)

### Standard processors (`extensions/standard-processors`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| AppendHostInfo              | Appends host IP/hostname as an attribute to incoming FlowFiles. | 🟢 Yes |
| AttributesToJSON            | Serializes FlowFile attributes to JSON, written to an attribute or to content. | 🟢 Yes |
| AttributeRollingWindow      | Tracks rolling-window stats (count, sum, mean, variance, stddev, min, max) of an Expression Language value across FlowFiles. | 🟢 Yes |
| ConvertRecord               | Converts records between formats via a configured Record Reader and Record Writer controller service. | 🟡 Partial — needs record-infrastructure subset (CSV/JSON readers only) |
| DefragmentText              | Splits/merges FlowFiles so cohesive text messages aren't broken across FlowFile boundaries. | 🟢 Yes |
| EvaluateJsonPath            | Evaluates JsonPath expressions against content; writes results to attributes or back to content. | 🟡 Partial — depends on chosen JSON lib; full JsonPath spec is heavy |
| ExtractText                 | Extracts FlowFile content into an attribute. | 🟢 Yes |
| FetchFile                   | Reads a file from disk into FlowFile content; optionally moves/deletes the source. | 🟡 Partial — LittleFS only, small files |
| FetchModbusTcp              | Reads holding/input registers and coils from PLCs over Modbus TCP. | 🟢 Yes — Modbus is small and very ESP32-relevant |
| GenerateFlowFile            | Creates FlowFiles with random or custom content (load testing, simulation). | 🟢 Yes |
| GetFile                     | Creates FlowFiles from files in a directory. | 🟡 Partial — LittleFS scope |
| GetTCP                      | Acts as a TCP server, emitting a FlowFile per inbound message. | 🟢 Yes |
| HashContent                 | Computes a configurable hash (MD5/SHA-x) of content; stores as attribute. | 🟢 Yes — mbedTLS already linked |
| InvokeHTTP                  | HTTP client; attributes become headers, content becomes the body for POST/PUT/PATCH. | 🟢 Yes — rewrite atop esp_http_client (no libcurl) |
| JoltTransformJSON           | Applies a Jolt spec (shift/default/remove/cardinality/sort/modify) to transform JSON content. | 🔴 No — Jolt is a JVM-derived library, no C++ port worth porting |
| ListenSyslog                | Listens for Syslog over TCP/UDP, parses headers, emits FlowFiles with attributes. | 🟢 Yes |
| ListenTCP                   | Listens on a TCP port; one FlowFile per inbound message. | 🟢 Yes |
| ListenUDP                   | Listens on a UDP port; one FlowFile per datagram. | 🟢 Yes |
| ListFile                    | Lists files in a directory (paired with FetchFile). | 🟡 Partial — LittleFS scope |
| LogAttribute                | Logs attributes (and optionally content) at a configured log level. | 🟢 Yes |
| PutFile                     | Writes FlowFile content to a directory on the local filesystem. | 🟡 Partial — LittleFS scope |
| PutTCP                      | Sends FlowFile content to a TCP endpoint. | 🟢 Yes |
| PutUDP                      | Sends FlowFile content as a UDP datagram. | 🟢 Yes |
| ReplaceText                 | Regex or literal search-and-replace on FlowFile content. | 🟡 Partial — std::regex is huge; use a slim regex (re-lite, RE2 mini) or literal-only mode |
| RetryFlowFile               | Tracks a retry count attribute; routes to `retries_exceeded` after N attempts. | 🟢 Yes |
| RouteOnAttribute            | Routes FlowFiles based on Expression Language matches against attributes. | 🟡 Partial — needs Expression Language subset |
| RouteText                   | Routes textual content (per-line or whole) to relationships based on matching strategies. | 🟢 Yes |
| SegmentContent              | Splits content into fixed-byte-size segment FlowFiles. | 🟢 Yes |
| SplitContent                | Splits content on a configurable byte-sequence delimiter. | 🟢 Yes |
| SplitJson                   | Splits a JSON array/object into one FlowFile per element via JsonPath. | 🟡 Partial — depends on JSON lib |
| SplitRecord                 | Splits a record-oriented FlowFile into multiple FlowFiles using Record Reader/Writer. | 🟡 Partial — same as ConvertRecord |
| SplitText                   | Splits a text FlowFile into multiple FlowFiles by line count, with optional header. | 🟢 Yes |
| TailFile                    | Tails one file (or many matching regex), emits new lines as FlowFiles. | 🟡 Partial — LittleFS scope, single small file practical |
| UpdateAttribute             | Adds/updates/removes FlowFile attributes from configured dynamic properties. | 🟢 Yes |

### Archive (`extensions/libarchive`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| ApplyTemplate               | Renders a Mustache template using FlowFile attributes; output becomes content. | 🟡 Partial — tiny C++ Mustache library port |
| CompressContent             | Compress/decompress content (gzip, bzip2, lzma, xz); updates mime.type. | 🟡 Partial — gzip via miniz; replace bz/xz/lzma with heatshrink |
| FocusArchiveEntry           | Focuses on one entry inside a TAR-style archive for downstream ops. | 🔴 No — libarchive is too large |
| ManipulateArchive           | Add/remove/copy/move/touch entries inside an archive FlowFile. | 🔴 No — libarchive too large |
| MergeContent                | Merges groups of FlowFiles (binary, TAR, ZIP) per a configured strategy. | 🟡 Partial — binary/concat strategy only; no TAR/ZIP |
| UnfocusArchiveEntry         | Restores a focused archive FlowFile to the full archive. | 🔴 No |

### AWS (`extensions/aws`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| DeleteS3Object              | Deletes objects from an S3 bucket. | 🔴 No — could be Partial with a custom SigV4-signing HTTP client |
| FetchS3Object               | Retrieves an S3 object into FlowFile content. | 🔴 No (same caveat) |
| ListS3                      | Lists S3 objects; pairs with FetchS3Object. | 🔴 No (same caveat) |
| PutS3Object                 | Uploads FlowFile content as an S3 object. | 🔴 No (same caveat) |
| PutKinesisStream            | Publishes content to an AWS Kinesis Data Stream. | 🔴 No |

### Azure (`extensions/azure`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| DeleteAzureBlobStorage      | Deletes a blob from an Azure Storage container. | 🔴 No |
| DeleteAzureDataLakeStorage  | Deletes a file from Azure Data Lake Storage Gen 2. | 🔴 No |
| FetchAzureBlobStorage       | Retrieves an Azure blob into FlowFile content. | 🔴 No |
| FetchAzureDataLakeStorage   | Fetches an ADLS Gen 2 file into FlowFile content. | 🔴 No |
| ListAzureBlobStorage        | Lists Azure blobs and emits one FlowFile per blob. | 🔴 No |
| ListAzureDataLakeStorage    | Lists ADLS Gen 2 files and emits one FlowFile per file. | 🔴 No |
| PutAzureBlobStorage         | Uploads FlowFile content as an Azure blob. | 🔴 No |
| PutAzureDataLakeStorage     | Writes FlowFile content as an ADLS Gen 2 file. | 🔴 No |

### CivetWeb (`extensions/civetweb`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| ListenHTTP                  | Embedded CivetWeb HTTP server; each POST becomes a FlowFile. | 🟡 Partial — use esp_http_server, drop CivetWeb dep |

### Couchbase (`extensions/couchbase`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| GetCouchbaseKey             | Fetches a document from Couchbase by key. | 🔴 No — libcouchbase too heavy |
| PutCouchbaseKey             | Writes FlowFile content as a Couchbase document. | 🔴 No |

### Elasticsearch (`extensions/elasticsearch`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| PostElasticsearch           | Posts content as documents to an Elasticsearch index via the bulk API. | 🟡 Partial — bulk format + esp_http_client, small JSON serializer |

### GCP (`extensions/gcp`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| DeleteGCSObject             | Deletes a Google Cloud Storage object. | 🔴 No |
| FetchGCSObject              | Fetches a GCS object into FlowFile content. | 🔴 No |
| ListGCSBucket               | Lists objects in a GCS bucket. | 🔴 No |
| PutGCSObject                | Uploads FlowFile content as a GCS object. | 🔴 No |

### Grafana Loki (`extensions/grafana-loki`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| PushGrafanaLokiREST         | Pushes content as log lines to Loki via the HTTP/REST push API. | 🟡 Partial — just HTTP + JSON |
| PushGrafanaLokiGrpc         | Pushes content as log lines to Loki via the gRPC push API. | 🔴 No — gRPC stack too heavy |

### Kafka (`extensions/kafka`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| ConsumeKafka                | Consumes messages from Kafka topics with SSL/SASL and rebalancing support. | 🔴 No — librdkafka too large |
| PublishKafka                | Publishes content to a Kafka topic with keying, partitioning, transactions. | 🔴 No |

### Kubernetes (`extensions/kubernetes`, Linux only)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| CollectKubernetesPodMetrics | Collects pod metrics from the K8s metrics API; emits JSON. | 🔴 No — irrelevant and Linux-only |

### LlamaCpp (`extensions/llamacpp`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| RunLlamaCppInference        | Runs inference against a locally loaded llama.cpp LLM. | 🔴 No — wrong size class |

### Lua (`extensions/lua`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| ExecuteScript (Lua)         | Runs a user-provided Lua script against the process session. | 🔵 Yes (S3) — Lua VM is ~200 KB; tight on plain ESP32, comfortable on S3 with PSRAM |

### MQTT (`extensions/mqtt`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| ConsumeMQTT                 | Subscribes to MQTT topics; emits FlowFiles per message. | 🟢 Yes — ESP-IDF native MQTT client |
| PublishMQTT                 | Publishes content to an MQTT topic with QoS/retain. | 🟢 Yes |

### OPC (`extensions/opc`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| FetchOPCProcessor           | Reads a value from an OPC-UA node. | 🟡 Partial — open62541 has ESP32 builds but is large |
| PutOPCProcessor             | Writes a value to an OPC-UA node. | 🟡 Partial (same) |

### OpenCV (`extensions/opencv`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| CaptureRTSPFrame            | Connects to an RTSP camera; emits a FlowFile per frame. | 🔴 No — OpenCV won't fit |
| MotionDetector              | Detects motion in image FlowFiles against a baseline. | 🔴 No (see proposed `DetectMotionCSI` and `DetectMotionCamera` for ESP32-native alternatives) |

### PDH (`extensions/pdh`, Windows only)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| PerformanceDataMonitor      | Collects Windows PDH counters; emits JSON. | 🔴 No — Windows only |

### ProcFs (`extensions/procfs`, Linux only)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| ProcFsMonitor               | Reads `/proc` metrics (CPU/mem/net/disk/per-process); emits JSON. | 🔴 No — Linux only (see proposed `GetESP32SystemMetrics`) |

### Python (`extensions/python`)

The Python extension registers the same `ExecuteScript` class as the Lua extension (counted once above), plus the ability to load user-supplied Python processor classes at runtime. **CPython won't fit on ESP32**; MicroPython is a feasible but separate path that effectively replaces — not extends — the upstream Python extension.

### SMB (`extensions/smb`, Windows only)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| FetchSmb                    | Fetches files from an SMB share. | 🔴 No |
| ListSmb                     | Lists files in an SMB share. | 🔴 No |
| PutSmb                      | Writes FlowFile content to an SMB share. | 🔴 No |

### SFTP (`extensions/sftp`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| FetchSFTP                   | Fetches a file via SFTP into FlowFile content. | 🔴 No — libssh2 + crypto too heavy |
| ListSFTP                    | Lists files on a remote SFTP server. | 🔴 No |
| PutSFTP                     | Uploads FlowFile content via SFTP. | 🔴 No |

### SQL (`extensions/sql`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| ExecuteSQL                  | Executes a SQL query; emits a FlowFile per row with columns as attributes. | 🟡 Partial — SQLite3 on ESP32 (~600 KB flash, tight) |
| PutSQL                      | Executes the SQL in FlowFile content against a DB connection. | 🟡 Partial (same) |
| QueryDatabaseTable          | Periodically queries a table for new rows using a max-value column. | 🟡 Partial (same) |

### Splunk (`extensions/splunk`)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| PutSplunkHTTP               | Sends content to a Splunk HTTP Event Collector. | 🟡 Partial — HTTP-only, rewrite atop esp_http_client |
| QuerySplunkIndexingStatus   | Queries Splunk to confirm indexing of previously sent events. | 🟡 Partial (same) |

### Systemd (`extensions/systemd`, Linux only)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| ConsumeJournald             | Consumes systemd-journald messages with fields as attributes. | 🔴 No — Linux only |

### Windows Event Log (`extensions/windows-event-log`, Windows only)

| Processor                   | Description | ESP32 Feasibility |
| :-------------------------- | :---------- | :---------------- |
| ConsumeWindowsEventLog      | Subscribes to Windows Event Log channels with XPath filter. | 🔴 No — Windows only |
| TailEventLog                | Tails a legacy Windows Event Log. | 🔴 No |

### Inventory totals

Upstream PROCESSORS.md enumerates **89 processor classes** (counting `ExecuteScript` and `ListenHTTP` each once even though multiple extensions register them). The table above lists each class exactly once, in the extension where it primarily lives.

| Class                                              | Count | % of 89 |
| :------------------------------------------------- | ----: | ------: |
| 🟢 **Yes** (direct port viable)                    |    24 |     27% |
| 🔵 **Yes (S3)** (needs PSRAM)                      |     1 |      1% |
| 🟡 **Partial** (rewrite, lighter deps, or subset)  |    23 |     26% |
| 🔴 **No** (cannot reasonably port)                 |    41 |     46% |

Roughly **half** of MiNiFi C++'s processor catalog is plausibly buildable for ESP32 (Yes + Partial = 48 processors). That ceiling is consistent with the working memory estimate from the earlier conversation — 20-40 curated processors per binary, with link-time selection. Building all 48 in one binary is unrealistic; building 20-30 of them as a coherent edge-fabric subset is exactly right.

---

## Part 2 — Proposed MicroFi-original Processors for the Physical AI Data Fabric

These processors do not exist in MiNiFi C++ because MiNiFi was never designed to live on a sensing endpoint. They're where MicroFi's contribution to a Physical AI Data Fabric actually shows up. The CSI / WiFi-passive cluster is the load-bearing research contribution; the rest are the fabric primitives that make the CSI work usable end-to-end.

### CSI & WiFi-passive sensing (the research core)

| Processor                   | Description | Why It Matters | Priority |
| :-------------------------- | :---------- | :------------- | :------: |
| GetWiFiCSI                  | Registers the ESP32 CSI RX callback (`esp_wifi_set_csi_rx_cb`) and emits one FlowFile per captured CSI packet with subcarrier complex samples + metadata (RSSI, MAC, channel, RX timestamp). | Raw substrate for every downstream CSI flow. ESP32 hardware exposes CSI natively — this is the entry point to the fabric. | **P0** |
| GetWiFiRSSI                 | Collects RSSI samples from configurable WiFi sources (beacons, probe responses, monitored APs) at a configurable rate. | Lower-fidelity but lower-bandwidth alternative to CSI for coarse presence/localization. | P1 |
| GetProbeRequests            | Promiscuous capture of 802.11 probe requests; emits one FlowFile per probe with source MAC, SSID list, RSSI, timestamp. | Passive device counting / fingerprinting for occupancy and crowd-density features. | P1 |
| ExtractCSIAmplitudePhase    | Converts raw CSI complex samples into per-subcarrier amplitude and phase arrays; drops nulls/pilots per the 802.11 mapping. | The first useful transform — every published CSI paper operates in amplitude/phase space. | **P0** |
| CSISubcarrierSelect         | Selects active OFDM subcarriers (typically 52 of 64 in 20 MHz, 56 of 64 with HT) per a configurable mask. | Standard preprocessing; reduces downstream payload size 20-25%. | P1 |
| CSIDenoise                  | Applies a chosen denoiser (Hampel filter, moving-average, low-pass, PCA) to CSI amplitude/phase streams. | CSI is famously noisy on commodity radios; denoising is mandatory for clean labels. | P1 |
| WindowCSI                   | Buffers CSI packets into fixed time- or count-based windows with configurable overlap; emits one FlowFile per window as a (T × subcarriers × 2) tensor. | Windowing is *the* abstraction between packet-level CSI and feature-level CSI. Closest MiNiFi analogue (`MergeContent`) doesn't understand tensor semantics. | **P0** |
| ComputeDopplerSpectrum      | STFT or FFT over a CSI window's time axis to produce micro-Doppler features. | Doppler features are how CSI-based gesture/gait/breathing recognition is typically done. | P1 |
| DetectMotionCSI             | Variance / MAD / spectral-energy threshold on CSI windows for binary presence detection. | The smallest useful end-to-end CSI demo: WiFi-only presence sensing without a camera. | **P0** |
| EstimateDistanceFTM         | Uses 802.11mc Fine Timing Measurement (supported on ESP32-S2/S3) to range against a cooperative AP/peer. | Adds an actual distance ground truth channel — pairs naturally with CSI as a target label. | P2 |
| ScanBLEAdvertising          | Passive BLE 4.x/5.x scan; emits one FlowFile per advertisement with MAC, RSSI, payload. | BLE adds a second RF modality for fusion (people often carry BLE beacons / phones broadcasting). | P2 |
| RunBistaticPair             | Coordinates a TX/RX pair of ESP32 nodes to emit CSI windows that are jointly time-aligned and labeled with the rig geometry. | This *is* the bistatic CSI research contribution — a flow-graph-level abstraction over two devices acting as one sensor. | **P0** |

### Sensor I/O (Lonely Binary kit + common Physical AI sensors)

| Processor                   | Description | Why It Matters | Priority |
| :-------------------------- | :---------- | :------------- | :------: |
| GetI2C                      | Generic I2C read with configurable address, register, length, sample rate; emits raw bytes. | Most kit sensors (BME280, MPU6050, VL53L0X, INA219, etc.) are I2C. | **P0** |
| GetSPI                      | Generic SPI read with configurable bus parameters. | SPI for higher-rate sensors (IMUs at 1 kHz+, some ADCs, displays). | P1 |
| GetAnalog                   | ADC sample at configurable rate; emits batches of raw samples. | Strain gauges, microphones with analog out, generic transducers. | P1 |
| GetGPIO                     | Digital input on edge / level / polled; emits a FlowFile per event. | PIR motion, buttons, magnetic-reed door sensors. | **P0** |
| GetIMU                      | Pre-built 6/9-axis IMU driver (MPU6050/MPU9250/BMI270); emits FlowFiles of (T × 6 or 9) samples at a configurable rate. | IMU is the most common ground-truth signal for activity / gait labels. | **P0** |
| GetEnvironmental            | Pre-built driver for BME280/SHT31/DHT22; emits temperature/humidity/pressure FlowFiles. | Environmental context features (temperature affects CSI). | P2 |
| GetMicrophone               | I2S microphone (INMP441 or similar); emits PCM frames at configurable rate/duration. | Acoustic modality — directly fusable with CSI for activity classification. | P1 |
| GetCamera                   | ESP32-CAM frame capture; JPEG or raw RGB565. | Camera-based ground truth for CSI auto-labeling. **Requires S3 + PSRAM.** | P1 (S3 only) |
| GetUltrasonic               | HC-SR04 ranging trigger/echo; emits distance reading. | Cheap ranging modality. | P2 |
| GetTOF                      | VL53L0X/L1X time-of-flight ranging. | Better-than-ultrasonic ranging for ground truth. | P2 |
| GetPMSensor                 | Particulate matter sensor (PMS5003/SDS011) over UART. | Air-quality data points for environmental fabric demos. | P2 |

### Physical AI feature / transform processors

| Processor                   | Description | Why It Matters | Priority |
| :-------------------------- | :---------- | :------------- | :------: |
| FFTContent                  | Apply esp-dsp FFT (real or complex) of configurable size to FlowFile content; emits magnitude/phase. | Foundational DSP primitive used by Doppler, audio features, vibration analysis. | **P0** |
| WindowAggregate             | Time- or count-based windowed aggregation: mean, max, RMS, stddev, peak-to-peak; configurable per channel. | Universal feature reducer; the "MergeContent for tensors" gap. | **P0** |
| DownsampleContent           | Decimation with optional anti-alias filter; configurable factor. | Bandwidth control before egress. | P1 |
| NormalizeContent            | Min-max or z-score normalization with running statistics. | ML pipelines expect normalized input; doing it on-device reduces ambiguity. | P1 |
| QuantizeContent             | INT8 / INT16 quantization with configurable scale/zero-point. | Halves or quarters egress bytes; matches TFLite Micro expectations. | P1 |
| AttachLabel                 | Stamps FlowFile content/attributes with a ground-truth label sourced from a sync signal (camera trigger, IMU event, time bucket, peer-broadcast event). | The auto-labeling primitive — the entire bistatic-CSI thesis depends on getting this right. | **P0** |
| AttachSchema                | Adds a structured schema descriptor (Avro/Arrow-style) as an attribute so downstream consumers can deserialize content without out-of-band knowledge. | Makes the data fabric self-describing — critical for "drop a node in, it just works." | P1 |
| AttachProvenance            | Adds node ID, firmware version, sensor calibration ID, sync-clock offset to every FlowFile. | Without this, multi-node datasets are unreproducible. | **P0** |

### Distributed flow-graph primitives

| Processor                   | Description | Why It Matters | Priority |
| :-------------------------- | :---------- | :------------- | :------: |
| SiteToSiteLite              | Peer-to-peer FlowFile transfer between MicroFi nodes over TCP using the FlowFile v3 framing. | The minimum-viable substrate for *distributed* flow graphs — your research moat. | **P0** |
| PublishFlowESPNow           | ESP-NOW peer transfer; brokerless, ad-hoc, sub-millisecond, no WiFi association needed. | Lets two devices coordinate without an AP — essential for the bistatic rig in unfamiliar environments. | **P0** |
| PublishFlowBLE              | BLE GATT-based FlowFile transfer for low-bandwidth coordination. | Useful when WiFi isn't available; pairs with BLE-only sensors. | P2 |
| EnforceOrder                | Stamps a monotonic sequence number; downstream reorders or drops out-of-window FlowFiles. | Distributed flows produce out-of-order data; this is the consistency primitive. | P1 |
| LoadBalanceQueue            | Round-robin / hash / least-loaded distribution to a set of downstream peers. | Enables horizontal scale across MicroFi nodes. | P1 |
| SyncClockPTP                | Software PTP/NTP-style time sync; updates a system offset attribute usable by AttachProvenance. | Bistatic CSI is meaningless without sub-millisecond time alignment. | **P0** |

### Egress / storage

| Processor                   | Description | Why It Matters | Priority |
| :-------------------------- | :---------- | :------------- | :------: |
| PutMQTTBatch                | MQTT publish with batching, compression, and explicit backpressure. | Production-grade egress to a broker. (Plain `PublishMQTT` from MiNiFi is fine for single messages.) | **P0** |
| PutHTTPBulk                 | HTTP POST with batching + retry + backpressure (esp_http_client). | Alternative egress to any HTTP-fronted lake (S3 via signed URL, custom API). | P1 |
| PutInfluxDB                 | InfluxDB line-protocol egress for time-series storage. | Most observability dashboards already speak Influx. | P2 |
| PutNATS                     | NATS publish (JetStream-aware). | Lighter than Kafka, more peer-friendly than MQTT for some topologies. | P2 |
| PutParquetS3                | Buffers FlowFiles into Parquet row groups, uploads to S3 (or S3-compatible) on rotation. | Direct path from edge to data-lake-format-of-record. **Yes (S3 + PSRAM only.)** | P2 (S3 only) |
| RotateLittleFS              | Local ring-buffer storage on LittleFS for offline-tolerant flows; replays on reconnect. | Makes the fabric tolerant of WiFi outages without dropping data. | **P0** |

### Health / observability

| Processor                   | Description | Why It Matters | Priority |
| :-------------------------- | :---------- | :------------- | :------: |
| GetESP32SystemMetrics       | ESP-IDF heap free, min-ever-free, task watermarks, WiFi RSSI/throughput, CPU load; emits JSON FlowFiles. | The ProcFs equivalent for ESP32; needed to diagnose the fragmentation/RAM failure modes specific to this platform. | **P0** |
| MonitorActivity             | Watchdog: fires a FlowFile (or resets) if no upstream activity within a window. | Catches sensor disconnects and frozen tasks; pairs with OTA recovery. | P1 |
| EmitHeartbeat               | Periodic heartbeat aligned to the EFM Monitor schema (matches `[[project_microfi_efm_monitor_schema]]`). | Keeps EFM Monitor mode happy so the per-processor counter overlay and green connector badge work. | **P0** |

### Edge inference (later, but worth scoping)

| Processor                   | Description | Why It Matters | Priority |
| :-------------------------- | :---------- | :------------- | :------: |
| RunTFLiteMicro              | Loads a TFLite Micro model from flash; runs inference per FlowFile; emits prediction + confidence. | First on-device inference primitive; closes the loop from sensor → feature → decision without leaving the MCU. | P1 |
| RunESPDLModel               | ESP-DL accelerated inference for ESP32-S3 (Espressif's in-house DL runtime). | S3 has dedicated SIMD-style ops; ESP-DL exploits them. | P2 (S3 only) |

### Summary of proposed MicroFi-original processors

| Category                    | Count | P0 Count |
| :-------------------------- | ----: | -------: |
| CSI & WiFi-passive sensing  |    12 |        5 |
| Sensor I/O                  |    11 |        3 |
| Feature / transform         |     8 |        4 |
| Distributed flow-graph      |     6 |        3 |
| Egress / storage            |     6 |        2 |
| Health / observability      |     3 |        2 |
| Edge inference              |     2 |        0 |
| **Total**                   | **48** |  **19** |

The P0 set (19 processors) plus the **23 directly portable MiNiFi processors** plus the most useful **5-10 Partial MiNiFi processors** (UpdateAttribute, RouteOnAttribute, MergeContent-binary, InvokeHTTP, ListenHTTP, CompressContent-gzip, ConvertRecord-CSV) gives you a build target of roughly **45-50 processors** — squarely in the practical envelope for a single ESP32-S3 binary, and a substantial superset of anything MiNiFi can offer at the sensing edge.

---

## Part 3 — Recommended build order

**Wave 1 (foundations — already partly built, finish first):**
UpdateAttribute, RouteOnAttribute, MergeContent (binary), PublishMQTT, InvokeHTTP, LogAttribute, GenerateFlowFile, EmitHeartbeat, GetESP32SystemMetrics.

**Wave 2 (CSI minimum viable demo):**
GetWiFiCSI, ExtractCSIAmplitudePhase, WindowCSI, DetectMotionCSI, AttachProvenance, AttachLabel, PutMQTTBatch.

**Wave 3 (bistatic rig — the research contribution):**
RunBistaticPair, SyncClockPTP, SiteToSiteLite, PublishFlowESPNow, EnforceOrder.

**Wave 4 (sensor fusion + feature richness):**
GetI2C, GetGPIO, GetIMU, FFTContent, WindowAggregate, ComputeDopplerSpectrum, RotateLittleFS.

**Wave 5 (production polish):**
QuantizeContent, NormalizeContent, AttachSchema, MonitorActivity, RunTFLiteMicro, PutHTTPBulk, GetCamera (S3 only).

---

## Sources

- [apache/nifi-minifi-cpp on GitHub](https://github.com/apache/nifi-minifi-cpp)
- [PROCESSORS.md (upstream)](https://github.com/apache/nifi-minifi-cpp/blob/main/PROCESSORS.md)
- [Extensions.md (upstream)](https://github.com/apache/nifi-minifi-cpp/blob/main/Extensions.md)
- [README.md (upstream, extension-to-CMAKE-flag mapping)](https://github.com/apache/nifi-minifi-cpp/blob/main/README.md)
