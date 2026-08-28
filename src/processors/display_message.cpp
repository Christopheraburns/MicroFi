// processors/display_message.cpp
//
// DisplayMessage (#227): the first sink that answers back on the glass.
// Takes the incoming FlowFile's content (or a literal `Message` property)
// and posts it to the display-message mailbox (microfi/display_message.h);
// the board's GUI renders whatever is in the mailbox -- on the AMOLED that
// is the agent status tile's 1 s refresh. The processor never touches the
// GUI itself: it only writes the mailbox, which keeps it engine-task-only
// and free of any Brookesia dependency.
//
// Typical class flow (fits the kMaxFlowNodes=4 cap next to a 2-node source
// chain): ListenHTTP -> DisplayMessage, driven by a NiFi InvokeHTTP.
//
// This file is entirely conditional on MICROFI_BOARD_DISPLAY_MESSAGE, defined
// only by the AMOLED overlay's CMakeLists.txt -- the XIAO PlatformIO builds
// compile it as an empty translation unit and the processor never reaches
// their manifest (same pattern as get_imu.cpp / MICROFI_BOARD_QMI8658).

#ifdef MICROFI_BOARD_DISPLAY_MESSAGE

#include "microfi/display_message.h"
#include "microfi/flowfile.h"
#include "microfi/processor.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/types.h"

#include "esp_log.h"

#include <cstring>

namespace microfi {
namespace displaymessage {

namespace {

static const char* TAG = "microfi.proc.display";

struct State {
    char message[kDisplayMessageMaxLen + 1];  // literal from the property; "" = use content
};
static_assert(sizeof(State) <= 256, "State larger than engine slab");

static const PropertyDescriptor kProperties[] = {
    {
        /* name          */ "Message",
        /* description   */ "Text to show. Leave blank to show the incoming "
                            "FlowFile's content (e.g. the body of a ListenHTTP POST).",
        /* default_value */ nullptr,
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
};
static constexpr size_t kPropertyCount =
    sizeof(kProperties) / sizeof(kProperties[0]);

Status on_init(void* state) {
    auto* s = static_cast<State*>(state);
    s->message[0] = '\0';
    return Status::Ok;
}

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s = static_cast<State*>(state);
    for (size_t i = 0; i < count; ++i) {
        const NodeProperty& p = props[i];
        if (std::strcmp(p.key, "Message") == 0) {
            std::strncpy(s->message, p.value, sizeof(s->message) - 1);
            s->message[sizeof(s->message) - 1] = '\0';
        }
    }
}

Status on_trigger(Session& session, void* state) {
    const FlowFile* in = session.input();
    if (in == nullptr) return Status::Again;

    const auto* s = static_cast<const State*>(state);

    if (s->message[0] != '\0') {
        display_message_post(s->message, std::strlen(s->message));
        ESP_LOGI(TAG, "posted literal (%u bytes) for FlowFile id=%llu",
                 static_cast<unsigned>(std::strlen(s->message)),
                 static_cast<unsigned long long>(in->id()));
        return Status::Ok;
    }

    // Content may be any bytes; the mailbox stores it as text and the
    // renderer treats it as UTF-8. Trim one trailing newline so a
    // `curl -d "$(echo hi)"` style body doesn't show a blank line.
    size_t len = in->content_size();
    const uint8_t* data = in->content();
    while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r')) --len;

    display_message_post(reinterpret_cast<const char*>(data), len);
    ESP_LOGI(TAG, "posted content (%u bytes) for FlowFile id=%llu",
             static_cast<unsigned>(len),
             static_cast<unsigned long long>(in->id()));
    return Status::Ok;
}

ProcessorDescriptor descriptor = {
    "DisplayMessage",
    "Shows the incoming FlowFile's content (or a literal Message) on the "
    "device's display. Sink: the message is rendered by the board's GUI.",
    &on_trigger,
    &on_init,
    &on_configure,
    sizeof(State),
    "INPUT_REQUIRED", // sink: must have an incoming connection
    kProperties,
    kPropertyCount,
    nullptr,          // on_stop -- no external resources
};

}  // namespace
}  // namespace displaymessage
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::displaymessage::descriptor)

#endif  // MICROFI_BOARD_DISPLAY_MESSAGE
