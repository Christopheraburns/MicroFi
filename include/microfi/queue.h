// queue.h
//
// Fixed-capacity ring buffer of FlowFiles. Used to connect two processors.
//
// Single-producer / single-consumer is the working assumption (one engine
// task drives all processors). A FreeRTOS mutex guards the few fields that
// have to be coherent if we ever cross task boundaries; under SPSC use the
// mutex is uncontended.

#pragma once

#include "microfi/flowfile.h"
#include "microfi/types.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstddef>

namespace microfi {

constexpr size_t kQueueCapacity = 8;

class Queue {
public:
    Queue();
    ~Queue();

    // Non-copyable / non-movable -- queues are owned by their connection.
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    const char* name() const { return name_; }
    void set_name(const char* n) { name_ = n; }

    // Push by value (FlowFile is small enough to copy cheaply, ~600 bytes).
    Status try_push(const FlowFile& f);

    // Pop into the caller's slot. Returns Again when empty.
    Status try_pop(FlowFile& out);

    size_t size() const;
    bool empty() const { return size() == 0; }
    bool full()  const { return size() == kQueueCapacity; }

private:
    FlowFile             slots_[kQueueCapacity];
    size_t               head_ = 0;   // next read
    size_t               tail_ = 0;   // next write
    size_t               n_    = 0;
    mutable SemaphoreHandle_t mtx_ = nullptr;
    const char*          name_ = "(anonymous)";
};

}  // namespace microfi
