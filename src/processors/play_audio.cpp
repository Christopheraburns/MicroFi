// processors/play_audio.cpp
//
// PlayAudio (#191, rung 5): the speaker as a sink. Takes an audio URL --
// from the incoming FlowFile's content, or the literal `Audio URL`
// property -- and hands it to Brookesia's AudioPlayback service, which
// fetches, decodes (mp3/wav) and plays it through the ES8311 codec. The
// player accepts `http(s)://…` and `file://littlefs/…` URLs.
//
// Why a URL and not the audio bytes: a MicroFi FlowFile carries at most
// kInlineContentBytes (256) of content, so audio can never ride the flow
// itself. A NiFi flow on the array POSTs a URL to the board's ListenHTTP
// and the board pulls the clip -- which also means the array never has to
// know the codec's sample format. Typical class flow (fits the
// kMaxFlowNodes=4 cap next to a 2-node source chain):
//   ListenHTTP(/play) -> PlayAudio
//
// Codec arbitration is Brookesia's, not ours: the AudioPlayback service
// shares the codec with the live AFE/wake-word pipeline through the HAL's
// ref-counted AudioProcessorCore and a hardware mixer (with ducking), and
// the ES8311's DAC reference channel feeds AEC -- so the mic stays live
// while a clip plays and a guest never touches esp_codec_dev directly.
// `Interrupt` (the service's own play option) decides whether a new URL
// cuts off whatever is already playing.
//
// The helper call is a submit, not a wait: function_play() queues a
// PlaybackRequest on the service and returns, so on_trigger never blocks
// the engine task for the length of a clip.
//
// Whole file conditional on MICROFI_BOARD_PLAY_AUDIO, defined only by the
// AMOLED overlay's CMakeLists.txt (XIAO PlatformIO builds compile it as an
// empty translation unit, same as get_imu.cpp / get_touch.cpp).

#ifdef MICROFI_BOARD_PLAY_AUDIO

#include "microfi/flowfile.h"
#include "microfi/processor.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/types.h"

#include "brookesia/service_helper/media/audio.hpp"

#include "esp_log.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace microfi {
namespace playaudio {

namespace {

static const char* TAG = "microfi.proc.audio";

using PlaybackHelper = esp_brookesia::service::helper::AudioPlayback;
using PlayUrlConfig  = esp_brookesia::service::helper::AudioPlayUrlConfig;

constexpr uint32_t kCallTimeoutMs = 2000;   // service RPC, not clip length
constexpr size_t   kUrlMax        = kInlineContentBytes;  // 256 -- content-sized

struct State {
    char    url[64];        // literal from `Audio URL` (NodeProperty value cap); "" = content
    int16_t volume;         // 0..100, -1 = leave the service's volume alone
    bool    interrupt;      // cut current playback for a new URL
    bool    volume_applied; // SetVolume once per flow apply, not per FlowFile
};
static_assert(sizeof(State) <= 256, "State larger than engine slab");

static const AllowableValue kInterruptValues[] = {
    { "true",  nullptr },
    { "false", nullptr },
};

static const PropertyDescriptor kProperties[] = {
    {
        /* name          */ "Audio URL",
        /* description   */ "URL to play: http(s)://… or file://littlefs/…  (mp3/wav). "
                            "Leave blank to play the URL carried in the incoming "
                            "FlowFile's content (e.g. the body of a ListenHTTP POST).",
        /* default_value */ nullptr,
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Volume",
        /* description   */ "Speaker volume 0-100 applied before playing. Leave blank "
                            "to keep the device's current volume.",
        /* default_value */ nullptr,
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Interrupt",
        /* description   */ "true: a new URL stops whatever is playing. false: the "
                            "request is dropped by the service if something is playing.",
        /* default_value */ "true",
        /* required      */ false,
        /* allowable     */ kInterruptValues, 2,
    },
};
static constexpr size_t kPropertyCount =
    sizeof(kProperties) / sizeof(kProperties[0]);

Status on_init(void* state) {
    auto* s = static_cast<State*>(state);
    s->url[0]         = '\0';
    s->volume         = -1;
    s->interrupt      = true;
    s->volume_applied = false;
    return Status::Ok;
}

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s = static_cast<State*>(state);
    for (size_t i = 0; i < count; ++i) {
        const NodeProperty& p = props[i];
        if (std::strcmp(p.key, "Audio URL") == 0) {
            std::strncpy(s->url, p.value, sizeof(s->url) - 1);
            s->url[sizeof(s->url) - 1] = '\0';
        } else if (std::strcmp(p.key, "Volume") == 0 && p.value[0] != '\0') {
            char* end = nullptr;
            const long v = std::strtol(p.value, &end, 10);
            if (end != p.value && v >= 0 && v <= 100) s->volume = static_cast<int16_t>(v);
        } else if (std::strcmp(p.key, "Interrupt") == 0 && p.value[0] != '\0') {
            s->interrupt = (std::strcmp(p.value, "false") != 0);
        }
    }
    s->volume_applied = false;
}

// Copy the URL out of the property or the FlowFile content, trimmed of
// surrounding whitespace/newlines (a `curl -d "$(echo url)"` body ends in
// one). Returns the length, 0 if there is nothing usable.
size_t resolve_url(const State* s, const FlowFile* in, char* out, size_t cap) {
    const char* src;
    size_t len;
    if (s->url[0] != '\0') {
        src = s->url;
        len = std::strlen(s->url);
    } else {
        src = reinterpret_cast<const char*>(in->content());
        len = in->content_size();
    }
    while (len > 0 && (src[0] == ' ' || src[0] == '\t' || src[0] == '\r' || src[0] == '\n')) {
        ++src; --len;
    }
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' ||
                       src[len - 1] == '\r' || src[len - 1] == '\n' || src[len - 1] == '\0')) {
        --len;
    }
    if (len == 0 || len >= cap) return 0;
    std::memcpy(out, src, len);
    out[len] = '\0';
    return len;
}

Status on_trigger(Session& session, void* state) {
    const FlowFile* in = session.input();
    if (in == nullptr) return Status::Again;

    auto* s = static_cast<State*>(state);

    char url[kUrlMax + 1];
    const size_t len = resolve_url(s, in, url, sizeof(url));
    if (len == 0) {
        ESP_LOGW(TAG, "FlowFile id=%llu: no URL (empty content and no Audio URL) -- dropped",
                 static_cast<unsigned long long>(in->id()));
        return Status::Ok;
    }
    if (std::strstr(url, "://") == nullptr) {
        ESP_LOGW(TAG, "FlowFile id=%llu: '%s' is not a URL -- dropped",
                 static_cast<unsigned long long>(in->id()), url);
        return Status::Ok;
    }

    if (!PlaybackHelper::is_available() || !PlaybackHelper::is_running()) {
        ESP_LOGW(TAG, "AudioPlayback service not running -- cannot play '%s'", url);
        return Status::Ok;
    }

    if (s->volume >= 0 && !s->volume_applied) {
        auto vr = PlaybackHelper::call_function_sync(
            PlaybackHelper::FunctionId::SetVolume, static_cast<double>(s->volume),
            esp_brookesia::service::helper::Timeout(kCallTimeoutMs));
        if (vr) {
            s->volume_applied = true;
            ESP_LOGI(TAG, "volume set to %d", static_cast<int>(s->volume));
        } else {
            ESP_LOGW(TAG, "SetVolume(%d) failed: %s", static_cast<int>(s->volume),
                     vr.error().c_str());
        }
    }

    PlayUrlConfig cfg;
    cfg.interrupt = s->interrupt;
    auto cfg_json = BROOKESIA_DESCRIBE_TO_JSON(cfg).as_object();

    auto pr = PlaybackHelper::call_function_sync(
        PlaybackHelper::FunctionId::Play, std::string(url), cfg_json,
        esp_brookesia::service::helper::Timeout(kCallTimeoutMs));
    if (!pr) {
        ESP_LOGW(TAG, "Play('%s') rejected: %s", url, pr.error().c_str());
        return Status::Ok;  // the FlowFile is consumed either way; nothing to retry
    }
    ESP_LOGI(TAG, "playing '%s' (interrupt=%s) for FlowFile id=%llu",
             url, s->interrupt ? "true" : "false",
             static_cast<unsigned long long>(in->id()));
    return Status::Ok;
}

ProcessorDescriptor descriptor = {
    "PlayAudio",
    "Plays an audio URL (http(s):// or file://littlefs/…, mp3/wav) through "
    "the device's speaker. The URL is the incoming FlowFile's content, or "
    "the literal Audio URL property. Sink.",
    &on_trigger,
    &on_init,
    &on_configure,
    sizeof(State),
    "INPUT_REQUIRED", // sink: must have an incoming connection
    kProperties,
    kPropertyCount,
    nullptr,          // on_stop -- playback is owned by the service, not us
};

}  // namespace
}  // namespace playaudio
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::playaudio::descriptor)

#endif  // MICROFI_BOARD_PLAY_AUDIO
