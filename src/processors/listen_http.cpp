// processors/listen_http.cpp
//
// Inbound HTTP ingress: starts a lightweight esp_http_server listener and
// turns each POST to the configured Base Path into a FlowFile. MiNiFi C++
// compatible property names (Listening Port, Base Path). Fire-and-forget
// ack, matching MiNiFi C++'s real ListenHTTP behavior -- not the synchronous
// HandleHttpRequest/HandleHttpResponse pairing MiNiFi Java has, which needs
// a request/response correlation model this single-task engine doesn't have
// yet. A natural follow-up once this proves out on hardware.
//
// The httpd server's own worker task runs on a *different* FreeRTOS task
// than the flow engine (flow_engine.h's thread-safety contract: engine
// state is single-task-owned). A URI handler can't safely touch Session/
// Queue objects directly, so it only ever pushes a fixed-size item onto a
// FreeRTOS queue; on_trigger (engine task, called every tick) drains that
// queue and does the real FlowFile creation + transfer -- same cross-task
// bridge shape FlowEngine::apply() already uses (pending_def_ + mutex_),
// just simpler since xQueueSend/Receive owns the hand-off.
//
// Teardown (#150): on_stop stops the httpd server and deletes the inbox
// queue when the engine rebuilds the graph, so a republish never orphans
// the port or leaks the queue. httpd_stop() blocks until the server task
// exits, so the inbox is deleted only after no handler can touch it.

#include "microfi/flowfile.h"
#include "microfi/flow_engine.h"
#include "microfi/processor.h"
#include "microfi/registry.h"
#include "microfi/session.h"
#include "microfi/types.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <cstdlib>
#include <cstring>

namespace microfi {
namespace listenhttp {

namespace {

static const char* TAG = "microfi.proc.listenhttp";

// Fixed-size item crossing the httpd task -> engine task boundary.
struct InboxItem {
    uint8_t content[kInlineContentBytes];
    size_t  content_len;
};

struct State {
    httpd_handle_t server;
    QueueHandle_t  inbox;
    bool           started;
    int32_t        port;
    char           base_path[32];
};
static_assert(sizeof(State) <= 256, "State larger than engine slab");

static const PropertyDescriptor kProperties[] = {
    {
        /* name          */ "Listening Port",
        /* description   */ "TCP port to listen for inbound HTTP POSTs on.",
        /* default_value */ nullptr,
        /* required      */ true,
        /* allowable     */ nullptr, 0,
    },
    {
        /* name          */ "Base Path",
        /* description   */ "URI path this listener answers on (e.g. /contentListener).",
        /* default_value */ "/",
        /* required      */ false,
        /* allowable     */ nullptr, 0,
    },
};
static constexpr size_t kPropertyCount =
    sizeof(kProperties) / sizeof(kProperties[0]);

Status on_init(void* state) {
    auto* s = static_cast<State*>(state);
    s->server = nullptr;
    s->inbox  = nullptr;
    s->started = false;
    s->port = -1;
    std::strncpy(s->base_path, "/", sizeof(s->base_path) - 1);
    s->base_path[sizeof(s->base_path) - 1] = '\0';
    return Status::Ok;
}

void on_configure(void* state, const NodeProperty* props, size_t count) {
    auto* s = static_cast<State*>(state);

    for (size_t i = 0; i < count; ++i) {
        const NodeProperty& p = props[i];
        if (std::strcmp(p.key, "Listening Port") == 0 && p.value[0] != '\0') {
            s->port = static_cast<int32_t>(atoi(p.value));
        }
        else if (std::strcmp(p.key, "Base Path") == 0 && p.value[0] != '\0') {
            std::strncpy(s->base_path, p.value, sizeof(s->base_path) - 1);
            s->base_path[sizeof(s->base_path) - 1] = '\0';
        }
    }
}

// Runs on the httpd server's own worker task -- see file header. Only ever
// touches the FreeRTOS queue, never Session/Queue/FlowEngine state directly.
esp_err_t post_handler(httpd_req_t* req) {
    auto* s = static_cast<State*>(req->user_ctx);

    const size_t to_read = req->content_len;
    if (to_read > kInlineContentBytes) {
        // Oversized body: don't try to read it, just close cleanly with an
        // honest status rather than queueing a silently truncated FlowFile.
        ESP_LOGW(TAG, "body %u bytes exceeds %u-byte inline limit; rejecting",
                 static_cast<unsigned>(to_read),
                 static_cast<unsigned>(kInlineContentBytes));
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, nullptr, 0);
        return ESP_OK;
    }

    InboxItem item;
    size_t received = 0;
    while (received < to_read) {
        const int r = httpd_req_recv(req, reinterpret_cast<char*>(item.content) + received,
                                      to_read - received);
        if (r <= 0) {
            ESP_LOGW(TAG, "httpd_req_recv failed mid-body (got %u/%u bytes)",
                     static_cast<unsigned>(received), static_cast<unsigned>(to_read));
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, nullptr, 0);
            return ESP_OK;
        }
        received += static_cast<size_t>(r);
    }
    item.content_len = received;

    if (xQueueSend(s->inbox, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "inbox full; dropping POST (%u bytes)",
                 static_cast<unsigned>(received));
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, nullptr, 0);
        return ESP_OK;
    }

    // Fire-and-forget ack, matching MiNiFi C++'s real ListenHTTP behavior --
    // the caller gets a 200 immediately; on_trigger processes the FlowFile
    // asynchronously on the engine's own schedule.
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

void start_server(State* s) {
    if (s->port <= 0) {
        ESP_LOGE(TAG, "Listening Port not configured or invalid; not starting");
        return;
    }

    // Reuse an inbox from a prior failed start -- recreating it on every
    // retry tick leaks a queue per second once httpd_start is failing.
    if (s->inbox == nullptr) {
        s->inbox = xQueueCreate(4, sizeof(InboxItem));
    }
    if (s->inbox == nullptr) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = static_cast<uint16_t>(s->port);
    // Distinct control port per listener -- avoids a clash if a flow ever
    // wires up two ListenHTTP nodes on different Listening Ports.
    cfg.ctrl_port = static_cast<uint16_t>(32768 + (s->port % 16384));

    if (httpd_start(&s->server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed (port=%d)", static_cast<int>(s->port));
        return;
    }

    httpd_uri_t uri = {};
    uri.uri      = s->base_path;
    uri.method   = HTTP_POST;
    uri.handler  = &post_handler;
    uri.user_ctx = s;
    httpd_register_uri_handler(s->server, &uri);

    s->started = true;
    ESP_LOGI(TAG, "listening on :%d%s", static_cast<int>(s->port), s->base_path);
}

// Engine task, on graph rebuild (#150). Stop the server first -- httpd_stop
// blocks until the server task is gone -- then delete the inbox no handler
// can be using anymore.
void on_stop(void* state) {
    auto* s = static_cast<State*>(state);
    if (s->server != nullptr) {
        httpd_stop(s->server);
        s->server = nullptr;
        ESP_LOGI(TAG, "listener on :%d stopped", static_cast<int>(s->port));
    }
    if (s->inbox != nullptr) {
        vQueueDelete(s->inbox);
        s->inbox = nullptr;
    }
    s->started = false;
}

Status on_trigger(Session& session, void* state) {
    auto* s = static_cast<State*>(state);

    if (!s->started) start_server(s);
    if (!s->started) return Status::InvalidArg;

    InboxItem item;
    if (xQueueReceive(s->inbox, &item, 0) != pdTRUE) {
        return Status::Again;  // nothing waiting this tick
    }

    FlowFile f;
    f.assign_id(FlowEngine::instance().next_id());

    Status rc = f.set_attribute("source", "ListenHTTP");
    if (rc != Status::Ok) return rc;
    rc = f.set_content(item.content, item.content_len);
    if (rc != Status::Ok) return rc;

    rc = session.transfer(f, "success");
    if (rc != Status::Ok) return rc;

    return Status::Ok;
}

ProcessorDescriptor descriptor = {
    "ListenHTTP",
    "Starts an HTTP listener; each POST to the configured Base Path becomes "
    "a FlowFile. Fire-and-forget ack (matches MiNiFi C++'s ListenHTTP, not "
    "the synchronous HandleHttpRequest/HandleHttpResponse pairing).",
    &on_trigger,
    &on_init,
    &on_configure,
    sizeof(State),
    "INPUT_FORBIDDEN", // source: no incoming connections
    kProperties,
    kPropertyCount,
    &on_stop,
};

}  // namespace
}  // namespace listenhttp
}  // namespace microfi

MICROFI_REGISTER_PROCESSOR(::microfi::listenhttp::descriptor)
