// flow_store.cpp -- Persistent flow-definition store.
//
// Uses a two-field binary envelope so load() can detect a truncated write:
//
//   Offset  Size  Field
//   0       4     magic: {'M','F','D','F'}  (MicroFi Definition File)
//   4       4     body_len: uint32_t LE     (number of body bytes that follow)
//   8       N     body: raw JSON or YAML
//
// On load, if the file is shorter than 8 + body_len we know a power-loss
// truncated the write and we return NotFound so the engine falls back to
// the boot-default graph rather than feeding a corrupt definition to the
// parser.

#include "microfi/flow_store.h"

#include "esp_log.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace microfi {

namespace {

const char* TAG = "microfi.flowstore";

constexpr uint8_t kMagic[4] = {'M', 'F', 'D', 'F'};
constexpr size_t  kHeaderSize = 8;  // 4 magic + 4 length

static void write_u32_le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >>  8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t read_u32_le(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

}  // namespace

Status flow_def_save(const char* body, size_t len) {
    if (body == nullptr || len == 0) return Status::InvalidArg;
    if (len > kFlowDefMaxBytes) {
        ESP_LOGW(TAG, "flow def too large (%u bytes > %u); not saving",
                 (unsigned)len, (unsigned)kFlowDefMaxBytes);
        return Status::InvalidArg;
    }

    FILE* f = fopen(kFlowDefPath, "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "fopen(%s, wb) failed -- LittleFS not mounted?", kFlowDefPath);
        return Status::IoError;
    }

    // Write header
    uint8_t hdr[kHeaderSize];
    std::memcpy(hdr, kMagic, 4);
    write_u32_le(hdr + 4, (uint32_t)len);

    bool ok = (fwrite(hdr, 1, kHeaderSize, f) == kHeaderSize);
    if (ok) ok = (fwrite(body, 1, len, f) == len);
    fclose(f);

    if (!ok) {
        ESP_LOGE(TAG, "flow def write failed (disk full or I/O error)");
        return Status::IoError;
    }

    ESP_LOGI(TAG, "flow def saved: %u bytes -> %s", (unsigned)len, kFlowDefPath);
    return Status::Ok;
}

Status flow_def_load(char* buf, size_t buf_cap, size_t* out_len) {
    if (buf == nullptr || buf_cap == 0) return Status::InvalidArg;

    struct stat st = {};
    if (stat(kFlowDefPath, &st) != 0) {
        ESP_LOGI(TAG, "no saved flow def (%s not found)", kFlowDefPath);
        return Status::NotFound;
    }

    const size_t file_size = (size_t)st.st_size;
    if (file_size < kHeaderSize) {
        ESP_LOGW(TAG, "saved flow def is too short (%u bytes) -- truncated write; ignoring",
                 (unsigned)file_size);
        return Status::NotFound;
    }

    FILE* f = fopen(kFlowDefPath, "rb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "fopen(%s, rb) failed", kFlowDefPath);
        return Status::IoError;
    }

    // Read and validate header
    uint8_t hdr[kHeaderSize];
    if (fread(hdr, 1, kHeaderSize, f) != kHeaderSize) {
        fclose(f);
        return Status::IoError;
    }
    if (std::memcmp(hdr, kMagic, 4) != 0) {
        fclose(f);
        ESP_LOGW(TAG, "flow def has wrong magic -- ignoring");
        return Status::NotFound;
    }

    const uint32_t body_len = read_u32_le(hdr + 4);
    if (file_size < kHeaderSize + body_len) {
        fclose(f);
        ESP_LOGW(TAG, "flow def truncated: file=%u expected=%u -- ignoring",
                 (unsigned)file_size, (unsigned)(kHeaderSize + body_len));
        return Status::NotFound;
    }
    if (body_len == 0) {
        fclose(f);
        ESP_LOGW(TAG, "flow def has zero-length body -- ignoring");
        return Status::NotFound;
    }
    if (body_len > buf_cap - 1) {
        fclose(f);
        ESP_LOGW(TAG, "flow def (%u bytes) too large for buffer (%u)", body_len, (unsigned)buf_cap);
        if (out_len) *out_len = body_len;
        return Status::Full;
    }

    const size_t read = fread(buf, 1, body_len, f);
    fclose(f);

    if (read != body_len) {
        ESP_LOGE(TAG, "flow def body read incomplete: %u / %u", (unsigned)read, body_len);
        return Status::IoError;
    }

    buf[read] = '\0';   // NUL-terminate for the JSON/YAML parser
    if (out_len) *out_len = read;

    ESP_LOGI(TAG, "flow def loaded: %u bytes from %s", (unsigned)read, kFlowDefPath);
    return Status::Ok;
}

void flow_def_clear() {
    if (remove(kFlowDefPath) == 0) {
        ESP_LOGI(TAG, "flow def cleared");
    } else {
        ESP_LOGD(TAG, "flow_def_clear: nothing to remove");
    }
}

Status flow_id_save(const char* flow_id) {
    if (flow_id == nullptr || flow_id[0] == '\0') return Status::InvalidArg;
    FILE* f = fopen(kFlowIdPath, "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "fopen(%s, wb) failed", kFlowIdPath);
        return Status::IoError;
    }
    // Write 36 chars + NUL = 37 bytes.
    const size_t written = fwrite(flow_id, 1, 36, f);
    fputc('\0', f);
    fclose(f);
    if (written != 36) {
        ESP_LOGE(TAG, "flow_id write failed");
        return Status::IoError;
    }
    ESP_LOGI(TAG, "flow_id saved: %.36s", flow_id);
    return Status::Ok;
}

Status flow_id_load(char out[37]) {
    if (out == nullptr) return Status::InvalidArg;
    FILE* f = fopen(kFlowIdPath, "rb");
    if (f == nullptr) return Status::NotFound;
    const size_t n = fread(out, 1, 36, f);
    fclose(f);
    out[n] = '\0';
    if (n != 36) {
        ESP_LOGW(TAG, "flow_id file too short (%u bytes) -- ignoring", (unsigned)n);
        return Status::NotFound;
    }
    ESP_LOGI(TAG, "flow_id loaded: %.36s", out);
    return Status::Ok;
}

}  // namespace microfi
