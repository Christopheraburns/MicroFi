// queue.cpp -- ring buffer of FlowFiles backed by a FreeRTOS mutex.
//
// Persistence path (when attach_repo() has been called):
//   try_push()  -- serializes the FlowFile to flash BEFORE enqueueing so a
//                  power-loss between push and the matching pop still leaves
//                  a persisted record that replay_from_repository() will pick
//                  up on the next boot.
//   try_pop()   -- just returns the slot; the caller (engine sink path) is
//                  responsible for calling IRepository::erase() after
//                  successful downstream processing.

#include "microfi/queue.h"

#include "microfi/flowfile_store.h"

#include "esp_log.h"

#include <cstring>

namespace microfi {

static const char* TAG = "microfi.queue";

Queue::Queue() {
    mtx_ = xSemaphoreCreateMutex();
}

Queue::~Queue() {
    if (mtx_ != nullptr) {
        vSemaphoreDelete(mtx_);
        mtx_ = nullptr;
    }
}

void Queue::attach_repo(IRepository* repo, const char* conn_id) {
    repo_ = repo;
    if (repo != nullptr && conn_id != nullptr) {
        std::strncpy(conn_id_, conn_id, 36);
        conn_id_[36] = '\0';
    } else {
        conn_id_[0] = '\0';
    }
}

Status Queue::try_push(FlowFile f) {
    // ---- Step 1: Persist to flash (outside mutex; flash writes can be slow) ----
    // Skip if already persisted (record_id > 0 = replayed FlowFile; already on flash).
    if (repo_ != nullptr && conn_id_[0] != '\0' && f.record_id() == 0) {
        // static: removes 1400 bytes from the hot-path call stack.
        // Safe because the engine is single-threaded (SPSC); the buffer is
        // never accessed re-entrantly.
        static uint8_t ser_buf[kFlowFileRecordMaxBytes];
        size_t   ser_len = 0;
        RecordId rid     = 0;

        const Status ser_rc = flowfile_serialize(f, conn_id_,
                                                  ser_buf, sizeof(ser_buf),
                                                  &ser_len);
        if (ser_rc == Status::Ok) {
            const Status wr_rc = repo_->write(ser_buf, ser_len, &rid);
            if (wr_rc == Status::Ok && rid > 0) {
                f.set_record_id(rid);
            } else if (wr_rc != Status::Ok) {
                ESP_LOGW(TAG, "repo write failed (%s) -- FlowFile not persisted",
                         to_string(wr_rc));
            }
        } else {
            ESP_LOGW(TAG, "flowfile_serialize failed -- FlowFile not persisted");
        }
    }

    // ---- Step 2: Enqueue under mutex ----------------------------------------
    if (mtx_ == nullptr) return Status::Internal;
    xSemaphoreTake(mtx_, portMAX_DELAY);
    Status rc;
    if (n_ == kQueueCapacity) {
        rc = Status::Full;
    } else {
        slots_[tail_] = f;   // record_id stamped on the stored copy
        tail_ = (tail_ + 1) % kQueueCapacity;
        ++n_;
        rc = Status::Ok;
    }
    xSemaphoreGive(mtx_);

    // ---- Step 3: Clean up orphan record if queue was full -------------------
    if (rc != Status::Ok && f.record_id() > 0 && repo_ != nullptr) {
        repo_->erase(f.record_id());
    }
    return rc;
}

Status Queue::try_pop(FlowFile& out) {
    if (mtx_ == nullptr) return Status::Internal;
    xSemaphoreTake(mtx_, portMAX_DELAY);
    Status rc;
    if (n_ == 0) {
        rc = Status::Again;
    } else {
        out = slots_[head_];
        slots_[head_].clear();   // erase slot to avoid stale content
        head_ = (head_ + 1) % kQueueCapacity;
        --n_;
        rc = Status::Ok;
    }
    xSemaphoreGive(mtx_);
    return rc;
}

size_t Queue::size() const {
    if (mtx_ == nullptr) return 0;
    xSemaphoreTake(mtx_, portMAX_DELAY);
    const size_t n = n_;
    xSemaphoreGive(mtx_);
    return n;
}

}  // namespace microfi
