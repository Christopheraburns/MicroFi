// display_message.cpp -- see microfi/display_message.h.
//
// A critical section (portMUX) rather than a mutex: both sides are a
// sub-microsecond memcpy, and the GUI-side reader must never block on the
// engine task.

#include "microfi/display_message.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <cstring>

namespace microfi {

namespace {

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
char     s_text[kDisplayMessageMaxLen + 1] = {0};
uint32_t s_seq = 0;
int64_t  s_posted_us = -1;

}  // namespace

void display_message_post(const char* text, size_t len) {
    if (text == nullptr) len = 0;
    if (len > kDisplayMessageMaxLen) len = kDisplayMessageMaxLen;

    taskENTER_CRITICAL(&s_lock);
    std::memcpy(s_text, text, len);
    s_text[len] = '\0';
    ++s_seq;
    s_posted_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_lock);
}

uint32_t display_message_copy(char* out, size_t cap, int64_t* age_ms) {
    if (out == nullptr || cap == 0) return 0;

    taskENTER_CRITICAL(&s_lock);
    const uint32_t seq = s_seq;
    const int64_t posted = s_posted_us;
    size_t n = std::strlen(s_text);
    if (n >= cap) n = cap - 1;
    std::memcpy(out, s_text, n);
    taskEXIT_CRITICAL(&s_lock);

    out[n] = '\0';
    if (age_ms != nullptr) {
        *age_ms = (posted < 0) ? -1 : (esp_timer_get_time() - posted) / 1000;
    }
    return seq;
}

}  // namespace microfi
