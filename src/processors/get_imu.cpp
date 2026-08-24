// processors/get_imu.cpp
//
// Source processor: reads the onboard QMI8658 6-axis accel/gyro on every
// tick and emits a FlowFile. MicroFi-original -- no upstream MiNiFi C++
// equivalent (same "genuinely new sense" category as GetGPIO). Built for the
// Waveshare AMOLED 1.8 V2 (ESP-Brookesia guest, #191): the IMU is not owned
// by Brookesia anywhere in the tree, so this processor adopts the shared I2C
// bus the same way the board's own AXP2101/TCA9554 drivers do
// (esp_board_periph_get_handle("i2c_master", ...) + the driver's own
// i2c_master_bus_add_device()) and owns the sensor outright.
//
// This file is entirely conditional on MICROFI_BOARD_QMI8658, defined only
// by the AMOLED overlay's CMakeLists.txt. The XIAO PlatformIO builds compile
// every file under src/ unconditionally (no src_filter), so on those targets
// this translation unit is empty and the processor is simply absent from the
// registry -- exactly like CaptureImage is absent from the AMOLED build.
//
// Polled, not interrupt-driven: the engine schedules on_trigger on the
// node's own timer, so "Read Interval" is not a hardware sample rate -- it's
// a minimum gap between emitted FlowFiles, enforced with esp_timer_get_time().
// Leaving it at the default "1 s" matches the engine's own default scheduling
// period, so the common case is "emit once per tick" with no throttling.
//
// The qmi8658_dev_t handle is a file-scope static, not part of the per-node
// State slab: State must fit the engine's fixed 256-byte state slab, and the
// device handle rides the same `* (extram_bss)` PSRAM mapping as the rest of
// libmicrofi_agent.a automatically (linker.lf), same as GenerateFlowFile's
// existing statics.

#ifdef MICROFI_BOARD_QMI8658

#include "microfi/flowfile.h"
#include "microfi/flow_engine.h"
#include "microfi/processor.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/types.h"

#include "esp_board_periph.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "qmi8658.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace microfi {
namespace getimu {

namespace {

static const char* TAG = "microfi.proc.imu";

// I2C address probe timeout, matching the vendor example.
constexpr int kProbeTimeoutMs = 100;

constexpr uint8_t kOutputJson       = 0;
constexpr uint8_t kOutputAttributes = 1;

struct State {
    bool     initialized;          // qmi8658_init + WHO_AM_I succeeded
    bool     init_failed;          // init failed once -- stop retrying/spamming
    uint8_t  output_format;        // kOutputJson | kOutputAttributes
    uint8_t  accel_range;          // qmi8658_accel_range_t, cast to uint8_t
    uint8_t  gyro_range;           // qmi8658_gyro_range_t, cast to uint8_t
    int64_t  min_gap_us;           // from "Read Interval"
    int64_t  last_emit_us;         // esp_timer_get_time() of last successful read; -1 = never
    float    motion_threshold_g;   // 0 = every read emits
    uint32_t tick;
};
static_assert(sizeof(State) <= 256, "State larger than engine slab");

// The sensor handle itself: shared-bus device state, not per-node. Lands in
// PSRAM via the agent's existing extram_bss mapping (zero-initialized).
static qmi8658_dev_t         s_imu_dev  = {};
static i2c_master_bus_handle_t s_i2c_bus = nullptr;

static const AllowableValue kOutputFormatValues[] = {
    { "JSON",       nullptr },
    { "Attributes", nullptr },
};

static const AllowableValue kAccelFullScaleValues[] = {
    { "2g",  nullptr },
    { "4g",  nullptr },
    { "8g",  nullptr },
    { "16g", nullptr },
};

// The driver's real range set is 32..4096 dps (confirmed against the
// downloaded waveshare/qmi8658 header at build time) -- not 16..2048 as
// first guessed in efm-amoled-capabilities.md. Trimmed here; "512dps"
// stays valid as the default either way.
static const AllowableValue kGyroFullScaleValues[] = {
    { "32dps",   nullptr },
    { "64dps",   nullptr },
    { "128dps",  nullptr },
    { "256dps",  nullptr },
    { "512dps",  nullptr },
    { "1024dps", nullptr },
    { "2048dps", nullptr },
    { "4096dps", nullptr },
};

static const PropertyDescriptor kProperties[] = {
    {
        /* name          */ "Read Interval",
        /* description   */ "Minimum time between emitted FlowFiles (e.g. \"1 s\", "
                            "\"500 ms\"). The engine already schedules this node on its "
                            "own tick period, so leaving this at the default matches that "
                            "schedule; raise it to throttle a faster-scheduled node.",
        /* default_value */ "1 s",
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Output Format",
        /* description   */ "JSON: accel/gyro/temp as FlowFile content. Attributes: "
                            "values as imu.* attributes (ax/ay/az/gx/gy/gz) and empty "
                            "content -- imu.temp is JSON-only (FlowFile attribute cap).",
        /* default_value */ "JSON",
        /* required      */ false,
        /* allowable     */ kOutputFormatValues, 2,
    },
    {
        /* name          */ "Accel Full Scale",
        /* description   */ "Accelerometer full-scale range.",
        /* default_value */ "4g",
        /* required      */ false,
        /* allowable     */ kAccelFullScaleValues, 4,
    },
    {
        /* name          */ "Gyro Full Scale",
        /* description   */ "Gyroscope full-scale range.",
        /* default_value */ "512dps",
        /* required      */ false,
        /* allowable     */ kGyroFullScaleValues, 8,
    },
    {
        /* name          */ "Motion Threshold (g)",
        /* description   */ "0 = every read emits. >0: emit only when the accel "
                            "magnitude departs from 1g (rest) by more than this many g "
                            "-- the shake-as-trigger variant.",
        /* default_value */ "0",
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
};
static constexpr size_t kPropertyCount =
    sizeof(kProperties) / sizeof(kProperties[0]);

// ---- property parsing ----------------------------------------------------

int64_t parse_read_interval_us(const char* v, int64_t fallback_us) {
    if (v == nullptr || v[0] == '\0') return fallback_us;
    char* end = nullptr;
    const float num = std::strtof(v, &end);
    if (end == v) return fallback_us;
    while (*end == ' ') ++end;
    int64_t unit_us = 1000000;  // default: seconds
    if (std::strncmp(end, "ms", 2) == 0) unit_us = 1000;
    else if (*end == 's' || *end == '\0') unit_us = 1000000;
    const float total = num * static_cast<float>(unit_us);
    if (!(total > 0.0f)) return fallback_us;
    return static_cast<int64_t>(total);
}

uint8_t parse_accel_range(const char* v, uint8_t fallback) {
    if (std::strcmp(v, "2g")  == 0) return static_cast<uint8_t>(QMI8658_ACCEL_RANGE_2G);
    if (std::strcmp(v, "4g")  == 0) return static_cast<uint8_t>(QMI8658_ACCEL_RANGE_4G);
    if (std::strcmp(v, "8g")  == 0) return static_cast<uint8_t>(QMI8658_ACCEL_RANGE_8G);
    if (std::strcmp(v, "16g") == 0) return static_cast<uint8_t>(QMI8658_ACCEL_RANGE_16G);
    return fallback;
}

uint8_t parse_gyro_range(const char* v, uint8_t fallback) {
    if (std::strcmp(v, "32dps")   == 0) return static_cast<uint8_t>(QMI8658_GYRO_RANGE_32DPS);
    if (std::strcmp(v, "64dps")   == 0) return static_cast<uint8_t>(QMI8658_GYRO_RANGE_64DPS);
    if (std::strcmp(v, "128dps")  == 0) return static_cast<uint8_t>(QMI8658_GYRO_RANGE_128DPS);
    if (std::strcmp(v, "256dps")  == 0) return static_cast<uint8_t>(QMI8658_GYRO_RANGE_256DPS);
    if (std::strcmp(v, "512dps")  == 0) return static_cast<uint8_t>(QMI8658_GYRO_RANGE_512DPS);
    if (std::strcmp(v, "1024dps") == 0) return static_cast<uint8_t>(QMI8658_GYRO_RANGE_1024DPS);
    if (std::strcmp(v, "2048dps") == 0) return static_cast<uint8_t>(QMI8658_GYRO_RANGE_2048DPS);
    if (std::strcmp(v, "4096dps") == 0) return static_cast<uint8_t>(QMI8658_GYRO_RANGE_4096DPS);
    return fallback;
}

Status on_init(void* state) {
    auto* s = static_cast<State*>(state);
    s->initialized         = false;
    s->init_failed         = false;
    s->output_format        = kOutputJson;
    s->accel_range          = static_cast<uint8_t>(QMI8658_ACCEL_RANGE_4G);
    s->gyro_range           = static_cast<uint8_t>(QMI8658_GYRO_RANGE_512DPS);
    s->min_gap_us           = 1000000;  // 1 s
    s->last_emit_us         = -1;
    s->motion_threshold_g   = 0.0f;
    s->tick                 = 0;
    return Status::Ok;
}

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s = static_cast<State*>(state);

    for (size_t i = 0; i < count; ++i) {
        const NodeProperty& p = props[i];
        if (std::strcmp(p.key, "Read Interval") == 0 && p.value[0] != '\0') {
            s->min_gap_us = parse_read_interval_us(p.value, s->min_gap_us);
        }
        else if (std::strcmp(p.key, "Output Format") == 0 && p.value[0] != '\0') {
            if (std::strcmp(p.value, "Attributes") == 0) s->output_format = kOutputAttributes;
            else                                          s->output_format = kOutputJson;
        }
        else if (std::strcmp(p.key, "Accel Full Scale") == 0 && p.value[0] != '\0') {
            s->accel_range = parse_accel_range(p.value, s->accel_range);
        }
        else if (std::strcmp(p.key, "Gyro Full Scale") == 0 && p.value[0] != '\0') {
            s->gyro_range = parse_gyro_range(p.value, s->gyro_range);
        }
        else if (std::strcmp(p.key, "Motion Threshold (g)") == 0 && p.value[0] != '\0') {
            char* end = nullptr;
            const float v = std::strtof(p.value, &end);
            s->motion_threshold_g = (end != p.value && v > 0.0f) ? v : 0.0f;
        }
    }
}

// Address probe, mirroring the vendor qmi8658_imu example: try both possible
// 7-bit addresses since board strapping of the SA0 pin isn't documented here.
bool detect_address(i2c_master_bus_handle_t bus, uint8_t* out_addr) {
    const uint8_t candidates[] = { QMI8658_ADDRESS_HIGH, QMI8658_ADDRESS_LOW };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (i2c_master_probe(bus, candidates[i], kProbeTimeoutMs) == ESP_OK) {
            *out_addr = candidates[i];
            return true;
        }
    }
    return false;
}

// Lazy init on first trigger: adopt the shared bus, add the QMI8658 as a
// guest device, confirm it's really there via WHO_AM_I, then apply the
// configured ranges. Never crashes on failure -- logs once and leaves
// State::init_failed set so on_trigger stops retrying.
bool ensure_imu_ready(State* s) {
    void* bus_handle = nullptr;
    esp_err_t err = esp_board_periph_get_handle("i2c_master", &bus_handle);
    if (err != ESP_OK || bus_handle == nullptr) {
        ESP_LOGW(TAG, "esp_board_periph_get_handle(\"i2c_master\") failed: %s",
                 esp_err_to_name(err));
        return false;
    }
    s_i2c_bus = static_cast<i2c_master_bus_handle_t>(bus_handle);

    uint8_t addr = 0;
    if (!detect_address(s_i2c_bus, &addr)) {
        ESP_LOGW(TAG, "QMI8658 not found on the shared I2C bus (probed 0x%02x/0x%02x)",
                 QMI8658_ADDRESS_HIGH, QMI8658_ADDRESS_LOW);
        return false;
    }

    err = qmi8658_init(&s_imu_dev, s_i2c_bus, addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "qmi8658_init failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t who_am_i = 0;
    err = qmi8658_get_who_am_i(&s_imu_dev, &who_am_i);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "qmi8658_get_who_am_i failed: %s", esp_err_to_name(err));
        return false;
    }

    qmi8658_set_accel_range(&s_imu_dev, static_cast<qmi8658_accel_range_t>(s->accel_range));
    qmi8658_set_accel_odr(&s_imu_dev, QMI8658_ACCEL_ODR_250HZ);
    qmi8658_set_gyro_range(&s_imu_dev, static_cast<qmi8658_gyro_range_t>(s->gyro_range));
    qmi8658_set_gyro_odr(&s_imu_dev, QMI8658_GYRO_ODR_250HZ);
    qmi8658_set_accel_unit_mps2(&s_imu_dev, false);  // report g, not m/s^2
    qmi8658_set_gyro_unit_dps(&s_imu_dev, true);
    qmi8658_enable_sensors(&s_imu_dev, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO);

    ESP_LOGI(TAG, "QMI8658 ready at 0x%02x (WHO_AM_I=0x%02x)",
             static_cast<unsigned>(addr), static_cast<unsigned>(who_am_i));
    return true;
}

Status on_trigger(Session& session, void* state) {
    auto* s = static_cast<State*>(state);

    if (s->init_failed) return Status::Ok;  // give up quietly -- no crash loop

    if (!s->initialized) {
        if (!ensure_imu_ready(s)) {
            s->init_failed = true;
            ESP_LOGW(TAG, "GetIMU disabled until the next flow apply (init failed)");
            return Status::Ok;
        }
        s->initialized = true;
    }

    const int64_t now = esp_timer_get_time();
    if (s->last_emit_us >= 0 && (now - s->last_emit_us) < s->min_gap_us) {
        return Status::Again;  // Read Interval not elapsed yet
    }

    bool ready = false;
    if (qmi8658_is_data_ready(&s_imu_dev, &ready) != ESP_OK || !ready) {
        return Status::Again;
    }

    qmi8658_data_t data = {};
    if (qmi8658_read_sensor_data(&s_imu_dev, &data) != ESP_OK) {
        return Status::Again;
    }

    if (s->motion_threshold_g > 0.0f) {
        const float mag = std::sqrt(data.accelX * data.accelX +
                                     data.accelY * data.accelY +
                                     data.accelZ * data.accelZ);
        if (std::fabs(mag - 1.0f) <= s->motion_threshold_g) {
            return Status::Again;  // below threshold -- not a "shake" event
        }
    }

    FlowFile f;
    f.assign_id(FlowEngine::instance().next_id());

    char tick_buf[16];
    std::snprintf(tick_buf, sizeof(tick_buf), "%u", static_cast<unsigned>(s->tick));

    Status rc = f.set_attribute("source", "GetIMU");
    if (rc != Status::Ok) return rc;
    rc = f.set_attribute("tickIndex", tick_buf);
    if (rc != Status::Ok) return rc;

    if (s->output_format == kOutputAttributes) {
        // source + tickIndex + 6 axes = 8 = kMaxAttributes exactly.
        // imu.temp is intentionally NOT set here -- there is no room left
        // under FlowFile's 8-attribute cap (microfi/flowfile.h); it remains
        // available in JSON mode.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", data.accelX);
        rc = f.set_attribute("imu.ax", buf); if (rc != Status::Ok) return rc;
        std::snprintf(buf, sizeof(buf), "%.2f", data.accelY);
        rc = f.set_attribute("imu.ay", buf); if (rc != Status::Ok) return rc;
        std::snprintf(buf, sizeof(buf), "%.2f", data.accelZ);
        rc = f.set_attribute("imu.az", buf); if (rc != Status::Ok) return rc;
        std::snprintf(buf, sizeof(buf), "%.2f", data.gyroX);
        rc = f.set_attribute("imu.gx", buf); if (rc != Status::Ok) return rc;
        std::snprintf(buf, sizeof(buf), "%.2f", data.gyroY);
        rc = f.set_attribute("imu.gy", buf); if (rc != Status::Ok) return rc;
        std::snprintf(buf, sizeof(buf), "%.2f", data.gyroZ);
        rc = f.set_attribute("imu.gz", buf); if (rc != Status::Ok) return rc;

        rc = f.set_content(nullptr, 0);
        if (rc != Status::Ok) return rc;
    } else {
        // ts: microseconds since boot (esp_timer_get_time) -- this board has
        // no adopted wall clock (PCF85063 RTC is a separate, un-adopted
        // sense; see efm-amoled-capabilities.md), so there is no epoch time
        // to report here.
        char content[160];
        const int len = std::snprintf(content, sizeof(content),
            "{\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,"
            "\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f,"
            "\"temp\":%.2f,\"ts\":%lld}",
            static_cast<double>(data.accelX), static_cast<double>(data.accelY),
            static_cast<double>(data.accelZ),
            static_cast<double>(data.gyroX), static_cast<double>(data.gyroY),
            static_cast<double>(data.gyroZ),
            static_cast<double>(data.temperature),
            static_cast<long long>(now));
        const size_t content_len =
            (len > 0) ? ((static_cast<size_t>(len) < sizeof(content))
                             ? static_cast<size_t>(len)
                             : sizeof(content) - 1)
                      : 0;
        rc = f.set_content(reinterpret_cast<const uint8_t*>(content), content_len);
        if (rc != Status::Ok) return rc;
    }

    rc = session.transfer(f, "success");
    if (rc != Status::Ok) return rc;

    s->last_emit_us = now;
    ++s->tick;
    return Status::Ok;
}

ProcessorDescriptor descriptor = {
    "GetIMU",
    "Reads the onboard QMI8658 accelerometer/gyroscope on every tick and "
    "emits accel (g) / gyro (dps) / temp (C) as FlowFile content or "
    "attributes.",
    &on_trigger,
    &on_init,
    &on_configure,
    sizeof(State),
    "INPUT_FORBIDDEN",  // source: no incoming connections
    kProperties,
    kPropertyCount,
    nullptr,          // on_stop -- the I2C bus is shared/adopted, not owned
};

}  // namespace
}  // namespace getimu
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::getimu::descriptor)

#endif  // MICROFI_BOARD_QMI8658
