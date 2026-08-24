// microfi/display_message.h
//
// The DisplayMessage mailbox (#227): the seam between the DisplayMessage
// sink processor (engine task, writes) and whatever renders the text on the
// device (GUI task, reads -- on the AMOLED that is the #185 agent status
// tile's 1 s refresh). Same additive-getter shape as the c2_client heartbeat
// getters: no engine or GUI dependency in either direction, so a board with
// no display can still compile the mailbox and simply never read it.
//
// Single-slot, last-writer-wins: a flow that answers back faster than the
// glass refreshes only ever shows the newest message, which is the right
// behaviour for a billboard.
#pragma once

#include <cstddef>
#include <cstdint>

namespace microfi {

constexpr size_t kDisplayMessageMaxLen = 120;  // bytes, excluding NUL

// Replace the current message. `len` bytes of `text` are copied (truncated
// to kDisplayMessageMaxLen); the sequence number increments on every call.
void display_message_post(const char* text, size_t len);

// Copy the current message into `out` (NUL-terminated, at most `cap` bytes
// including the NUL). Returns the sequence number of the copied message
// (0 = nothing posted yet, `out` is set to ""). `age_ms`, if non-null,
// receives milliseconds since that message was posted (-1 if none).
uint32_t display_message_copy(char* out, size_t cap, int64_t* age_ms);

}  // namespace microfi
