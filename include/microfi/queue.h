// queue.h
//
// Fixed-capacity ring buffer of FlowFiles. Used to connect two processors.
//
// Single-producer / single-consumer is the working assumption (one engine
// task drives all processors). A FreeRTOS mutex guards the few fields that
// have to be coherent if we ever cross task boundaries; under SPSC use the
// mutex is uncontended.
//
// Persistence: if attach_repo() has been called with a non-null repository,
// try_push() serializes each FlowFile and writes it to flash before enqueueing
// it, stamping the assigned RecordId onto the stored slot.  The engine's sink
// commit path calls IRepository::erase() with that RecordId after successful
// downstream processing, giving exactly-once delivery semantics across reboots.
// FlowFiles that arrive with record_id() > 0 (replayed on boot) bypass the
// write step -- they are already on flash.

#pragma once

#include "microfi/flowfile.h"
#include "microfi/repository.h"
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

    // Non-copyable / non-movable -- queues are owned by their connection slot.
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    const char* name() const { return name_; }
    void set_name(const char* n) { name_ = n; }

    // Attach a durable repository so FlowFiles are persisted on push.
    // conn_id is the 36-char UUID of the owning connection; it is embedded in
    // every persisted record as the replay routing key.
    // Call with repo = nullptr to revert to volatile-only mode.
    void attach_repo(IRepository* repo, const char* conn_id);

    // Push a FlowFile into the queue (by value).
    // If a repository is attached and ff.record_id() == 0, the FlowFile is
    // serialized to flash before enqueueing and the returned RecordId is
    // stamped onto the stored slot copy.  If the queue is full the persisted
    // record is immediately erased to avoid orphans.
    Status try_push(FlowFile f);

    // Pop into the caller's slot.  Returns Again when empty.
    // The popped copy carries the record_id from the stored slot; the engine's
    // sink path uses it to erase the record after successful processing.
    Status try_pop(FlowFile& out);

    size_t size() const;
    bool empty() const { return size() == 0; }
    bool full()  const { return size() == kQueueCapacity; }

private:
    FlowFile              slots_[kQueueCapacity];
    size_t                head_    = 0;
    size_t                tail_    = 0;
    size_t                n_       = 0;
    mutable SemaphoreHandle_t mtx_ = nullptr;
    const char*           name_    = "(anonymous)";

    // Persistence context; nullptr = volatile-only.
    IRepository*          repo_    = nullptr;
    char                  conn_id_[37] = {0};   // 36-char UUID + NUL
};

}  // namespace microfi
