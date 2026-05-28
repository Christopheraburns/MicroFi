// queue.cpp -- ring buffer of FlowFiles backed by a FreeRTOS mutex.

#include "microfi/queue.h"

namespace microfi {

Queue::Queue() {
    mtx_ = xSemaphoreCreateMutex();
    // If mutex creation fails (extreme OOM at boot) the queue is unusable
    // and try_push / try_pop will return Internal. We don't abort here so a
    // failure surfaces through the normal status channel.
}

Queue::~Queue() {
    if (mtx_ != nullptr) {
        vSemaphoreDelete(mtx_);
        mtx_ = nullptr;
    }
}

Status Queue::try_push(const FlowFile& f) {
    if (mtx_ == nullptr) return Status::Internal;
    xSemaphoreTake(mtx_, portMAX_DELAY);
    Status rc;
    if (n_ == kQueueCapacity) {
        rc = Status::Full;
    } else {
        slots_[tail_] = f;
        tail_ = (tail_ + 1) % kQueueCapacity;
        ++n_;
        rc = Status::Ok;
    }
    xSemaphoreGive(mtx_);
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
        slots_[head_].clear();   // erase the slot so we don't keep stale content
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
