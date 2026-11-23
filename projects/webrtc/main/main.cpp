#include "common/application.h"
#include "provisioning.h"
#include "local_http.h"
#include "tls_identity.h"
#include "camera_controls.h"
#include "motion.h"
#include "recorder.h"
#include "remote_signaling.h"
#include "esp_peer_signaling.h"
#include "remote_auth.h"
#include "peer_receive.h"
#include "peer_send.h"
#include "send_pacer.h"
#include "local_candidates.h"
#include "lwip/sockets.h"
#include "esp_camera.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "media_lib_adapter.h"
#include "esp_https_server.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <string>
#include <sys/time.h>
#include <unistd.h>

namespace {
constexpr char TAG[] = "local_webrtc";
constexpr size_t CHUNK = 2048;
// A viewer pings every 5 seconds. Browsers throttle timers in background tabs
// and on sleeping phones, so the camera keeps the session through up to five
// minutes without any message and only then disconnects it. Recording
// transfers count as activity too. Closed tabs disconnect at once.
constexpr int64_t PEER_IDLE_US = 5 * 60 * 1000000LL;
// Bulk traffic (video, recording downloads) follows the link's capacity; see
// send_pacer.h. Control replies and pings bypass the pacer so a full video
// queue cannot starve them. The ceiling allows for each chip's CPU and RAM.
constexpr SendPacer::Config PACING = {
    .floor = 128 * 1024, .start = 256 * 1024,
#if CONFIG_IDF_TARGET_ESP32S3
    .ceiling = 1536 * 1024,
#else
    .ceiling = 1024 * 1024,
#endif
    .step = 32 * 1024, .increase_us = 100000, .cooldown_us = 200000, .chunk = CHUNK,
};
// Chunks one worker pass may send; the pacer's credit is the real limit.
constexpr int BULK_CHUNKS_PER_PASS = 12;
// Internal RAM below this slows bulk traffic before video must pause (12 KiB).
constexpr size_t PACING_LOW_MEMORY = 20480;
SendPacer pacer(PACING);
// Why the pacer slowed down, for the periodic log: a full SCTP send cache,
// a full Wi-Fi TX queue, waiting control replies, low internal RAM.
uint32_t slow_cache, slow_queue, slow_reply, slow_memory;
esp_peer_default_cfg_t peer_defaults(uint8_t candidates) {
    esp_peer_default_cfg_t cfg{};
    cfg.agent_recv_timeout = CAMERA_ICE_RECEIVE_MS;
    // Encrypted application pings already maintain NAT mappings and update
    // activity; the worker also counts recording transfer progress. Avoid
    // a second STUN keepalive checker dropping an otherwise active channel.
    cfg.alive_binding_retries = 0xff;
    // The browser creates a reliable, ordered channel. Retain unacknowledged
    // packets for 60 s rather than the library's 5 s, so temporary backpressure
    // does not drop ordered messages. The field is 16-bit milliseconds (65.5 s
    // at most), so it cannot follow the 5-minute inactivity deadline.
    cfg.data_ch_cfg.cache_timeout = 60000;
    // Unacknowledged data the channel may have in flight. 16 KiB limited video
    // to one window per browser acknowledgement (about 190 KiB/s on a strong
    // link); 64 KiB stops that. esp_peer allocates it from PSRAM through the
    // media_lib allocator, so internal RAM is unaffected.
    cfg.data_ch_cfg.send_cache_size = 65536;
    cfg.data_ch_cfg.recv_cache_size = 8192;
    cfg.max_candidates = candidates;
    return cfg;
}
std::deque<std::string> commands;
SemaphoreHandle_t mutex;
esp_peer_handle_t peer;
httpd_handle_t server;
std::string answer, peer_offer;
// Replies and status pushes waiting for the data channel, oldest first.
std::deque<std::string> replies;
// Every viewer, local or remote, proves the camera password before it
// receives frames, settings or recordings.
RemoteAuth peer_auth;
bool channel_ready = false, failed = false, remote_peer = false;
uint16_t stream_id;
int64_t activity = 0;
extern const unsigned char page_start[] asm("_binary_viewer_html_start");
extern const unsigned char page_end[] asm("_binary_viewer_html_end");
extern const unsigned char settings_start[] asm("_binary_settings_js_start");
extern const unsigned char settings_end[] asm("_binary_settings_js_end");
extern const unsigned char motion_start_js[] asm("_binary_motion_js_start");
extern const unsigned char motion_end_js[] asm("_binary_motion_js_end");
// One recording download at a time; its chunks replace video frames until done.
// `buffered` is the offset of the chunk already read into the packet buffer,
// kept for a retry after backpressure instead of reading the card again.
struct Transfer { int fd = -1; uint32_t id = 0, size = 0, offset = 0, buffered = UINT32_MAX; } transfer;
uint32_t transfer_ids = 0;

struct Lock {
    Lock() { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(mutex); }
};
int send_text(cJSON *json) {
    if (!json) return -1;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    // Telemetry can wait; never crowd out a setting acknowledgement while
    // the reliable channel is waiting for memory or SCTP send capacity.
    if (cJSON_IsString(type) && !strcmp(type->valuestring, "motion_status") && !replies.empty()) {
        cJSON_Delete(json);
        return 0;
    }
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return -1;
    // Bound the queue; a stalled channel drops the oldest messages.
    if (replies.size() >= 16) replies.pop_front();
    replies.emplace_back(text);
    cJSON_free(text);
    return 0;
}
void stop_transfer() {
    if (transfer.fd >= 0) close(transfer.fd);
    transfer.fd = -1;
    transfer.buffered = UINT32_MAX;
}
void start_transfer(const cJSON *request) {
    stop_transfer();
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(request, "name");
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "download");
    int fd = -1;
    uint32_t size = 0;
    const char *error = cJSON_IsString(name) ? recorder_open(name->valuestring, fd, size) : "Missing recording name";
    if (cJSON_IsString(name)) cJSON_AddStringToObject(reply, "name", name->valuestring);
    if (error) cJSON_AddStringToObject(reply, "error", error);
    else {
        transfer = {fd, ++transfer_ids, size, 0};
        cJSON_AddNumberToObject(reply, "id", transfer.id);
        cJSON_AddNumberToObject(reply, "size", size);
    }
    send_text(reply);
}
// Viewers send their clock; it names clips until SNTP synchronizes.
void set_clock(const cJSON *request) {
    const cJSON *now = cJSON_GetObjectItemCaseSensitive(request, "now");
    if (time(nullptr) > 1700000000 || !cJSON_IsNumber(now) || !(now->valuedouble > 1.7e12 && now->valuedouble < 4.1e12)) return;
    timeval value{time_t(now->valuedouble / 1000), suseconds_t(std::fmod(now->valuedouble, 1000.0) * 1000)};
    if (settimeofday(&value, nullptr) == 0) ESP_LOGI(TAG, "Clock set from the viewer");
}
int on_state(esp_peer_state_t state, void *) {
    ESP_LOGI(TAG, "Peer state %d", int(state));
    if (state == ESP_PEER_STATE_DISCONNECTED || state == ESP_PEER_STATE_CONNECT_FAILED ||
        state == ESP_PEER_STATE_DATA_CHANNEL_CLOSED || state == ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED) {
        channel_ready = false;
        failed = true;
    }
    return 0;
}
int on_message(esp_peer_msg_t *msg, void *) {
    if (msg->type == ESP_PEER_MSG_TYPE_SDP && msg->size > 0 && msg->size <= 16384) {
        if (remote_peer) remote_send(1, reinterpret_cast<char *>(msg->data));
        answer.assign(reinterpret_cast<char *>(msg->data), strnlen(reinterpret_cast<char *>(msg->data), msg->size));
    }
    return 0;
}
int on_channel(esp_peer_data_channel_info_t *info, void *) {
    // SCTP may retransmit DCEP OPEN. One camera channel is already active;
    // generating another challenge would reset and disconnect its browser.
    if (channel_ready) return 0;
    stream_id = info->stream_id;
    channel_ready = true;
    camera_peer_transport_ready(true);
    activity = esp_timer_get_time();
    send_text(peer_auth.begin(peer_offer, answer));
    return 0;
}
int on_data(esp_peer_data_frame_t *frame, void *) {
    if (!channel_ready || frame->type != ESP_PEER_DATA_CHANNEL_STRING || frame->size <= 0 || frame->size > 1024) return 0;
    // Parse exactly one JSON document, never assuming network data is terminated.
    std::string text(reinterpret_cast<char *>(frame->data), frame->size);
    const char *end = nullptr;
    cJSON *request = cJSON_ParseWithOpts(text.c_str(), &end, true);
    if (!request) return 0;
    if (!peer_auth.allowed()) {
        send_text(peer_auth.verify(request));
        cJSON_Delete(request); return 0;
    }
    activity = esp_timer_get_time();
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    // Keep the receive callback short. Sensor I/O, NVS writes, and SD access
    // run only after the peer loop returns and releases its transport locks.
    if (!(cJSON_IsString(op) && !strcmp(op->valuestring, "ping"))) {
        if (commands.size() < 16) commands.emplace_back(std::move(text));
        else {
            cJSON *reply = cJSON_CreateObject();
            cJSON_AddStringToObject(reply, "type", "error");
            cJSON_AddStringToObject(reply, "error", "Camera busy; please retry the setting");
            send_text(reply);
        }
    }
    cJSON_Delete(request);
    return 0;
}
void close_peer() {
    channel_ready = false;
    camera_peer_transport_ready(false);
    if (peer) { esp_peer_close(peer); peer = nullptr; }
    stop_transfer();
    answer.clear(); replies.clear(); commands.clear(); peer_offer.clear(); peer_auth.clear(); failed = false; remote_peer = false;
    pacer.reset(esp_timer_get_time());
}
esp_err_t respond(httpd_req_t *r, const char *status, const char *body) {
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, body);
}
bool same_origin(httpd_req_t *r) {
    if (!provisioning_is_ready()) return false;
    esp_netif_ip_info_t ip{};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK) return false;
    return camera_http::allow_request(r, ip, true);
}
esp_err_t page(httpd_req_t *r) {
    if (!same_origin(r)) return respond(r, "403 Forbidden", "{}");
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(r, "Content-Security-Policy", "default-src 'none'; script-src 'self' 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src blob:; frame-ancestors 'none'; base-uri 'none'");
    return httpd_resp_send(r, reinterpret_cast<const char *>(page_start), page_end - page_start - 1);
}
esp_err_t settings_script(httpd_req_t *r) {
    if (!same_origin(r)) return respond(r, "403 Forbidden", "{}");
    httpd_resp_set_type(r, "application/javascript");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, reinterpret_cast<const char *>(settings_start), settings_end - settings_start - 1);
}
esp_err_t motion_script(httpd_req_t *r) {
    if (!same_origin(r)) return respond(r, "403 Forbidden", "{}");
    httpd_resp_set_type(r, "application/javascript");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, reinterpret_cast<const char *>(motion_start_js), motion_end_js - motion_start_js - 1);
}
esp_err_t session(httpd_req_t *r) {
    if (!same_origin(r)) return respond(r, "403 Forbidden", "{\"error\":\"Use the local device page\"}");
    Lock guard;
    if (r->method == HTTP_DELETE) { close_peer(); return respond(r, "200 OK", "{}"); }
    if (r->method == HTTP_GET) {
        if (!peer) return respond(r, "404 Not Found", "{}");
        if (failed) return respond(r, "503 Service Unavailable", "{\"error\":\"Peer connection failed\"}");
        if (answer.empty()) return respond(r, "202 Accepted", "{}");
        httpd_resp_set_type(r, "application/sdp");
        httpd_resp_set_hdr(r, "Cache-Control", "no-store");
        return httpd_resp_send(r, answer.data(), answer.size());
    }
    if (peer) return respond(r, "409 Conflict", "{\"error\":\"One browser at a time; disconnect the other viewer, or wait up to 5 minutes if its connection was lost\"}");
    if (!RemoteAuth::configured()) return respond(r, "403 Forbidden", "{\"error\":\"Set a camera password over USB before viewing\"}");
    if (RemoteAuth::blocked()) return respond(r, "429 Too Many Requests", "{\"error\":\"Too many wrong passwords; wait 30 seconds\"}");
    if (!r->content_len || r->content_len > 16384) return respond(r, "400 Bad Request", "{}");
    std::string offer(r->content_len, '\0');
    size_t read = 0;
    while (read < offer.size()) {
        int n = httpd_req_recv(r, &offer[read], offer.size() - read);
        if (n <= 0) return respond(r, "408 Request Timeout", "{}");
        read += n;
    }
    if (offer.find('\0') != std::string::npos || offer.find("a=fingerprint:sha-256 ") == std::string::npos ||
        offer.find("UDP/DTLS/SCTP") == std::string::npos || offer.find("a=candidate:") == std::string::npos) {
        return respond(r, "400 Bad Request", "{\"error\":\"Expected complete DTLS data-channel SDP with ICE candidates\"}");
    }
    sockaddr_storage remote{};
    socklen_t remote_length = sizeof(remote);
    if (getpeername(httpd_req_to_sockfd(r), reinterpret_cast<sockaddr *>(&remote), &remote_length) != 0) return ESP_FAIL;
    in_addr address{};
    if (remote.ss_family == AF_INET) {
        address = reinterpret_cast<const sockaddr_in &>(remote).sin_addr;
    } else if (remote.ss_family == AF_INET6) {
        const auto &ipv6 = reinterpret_cast<const sockaddr_in6 &>(remote);
        constexpr uint8_t mapped_prefix[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(ipv6.sin6_addr.s6_addr, mapped_prefix, sizeof(mapped_prefix))) {
            return respond(r, "400 Bad Request", "{\"error\":\"Direct IPv4 LAN connection required\"}");
        }
        memcpy(&address, ipv6.sin6_addr.s6_addr + 12, sizeof(address));
    } else return respond(r, "400 Bad Request", "{}");
    char remote_ip[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &address, remote_ip, sizeof(remote_ip))) return ESP_FAIL;
    normalize_local_candidates(offer, remote_ip);
    peer_offer = offer;
    esp_peer_default_cfg_t extra = peer_defaults(4);
    camera_peer_transport_ready(false);
    esp_peer_cfg_t cfg{};
    cfg.role = ESP_PEER_ROLE_CONTROLLED;
    cfg.no_auto_reconnect = true;
    cfg.enable_data_channel = true;
    cfg.manual_ch_create = true;
    cfg.extra_cfg = &extra; cfg.extra_size = sizeof(extra);
    cfg.on_state = on_state; cfg.on_msg = on_message;
    cfg.on_channel_open = on_channel; cfg.on_data = on_data;
    int ret = esp_peer_open(&cfg, esp_peer_get_default_impl(), &peer);
    if (ret == 0) ret = esp_peer_new_connection(peer);
    if (ret == 0) {
        esp_peer_msg_t msg{};
        msg.type = ESP_PEER_MSG_TYPE_SDP;
        msg.data = reinterpret_cast<uint8_t *>(&offer[0]);
        msg.size = offer.size();
        ret = esp_peer_send_msg(peer, &msg);
    }
    if (ret != 0) { ESP_LOGE(TAG, "Peer creation/SDP failed: %d", ret); close_peer(); return respond(r, "503 Service Unavailable", "{\"error\":\"Peer creation failed\"}"); }
    activity = esp_timer_get_time();
    return respond(r, "202 Accepted", "{}");
}
void remote_events() {
    RemoteEvent event;
    while (remote_receive(event)) {
        if (event.type == 4 && event.data == "connect") {
            if (peer) { remote_send(4, "busy"); continue; }
            esp_peer_ice_server_cfg_t ice{}; remote_ice(ice);
            cJSON *ready = cJSON_CreateObject();
            cJSON_AddStringToObject(ready, "type", "ready");
            cJSON *servers = cJSON_AddArrayToObject(ready, "iceServers");
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "urls", ice.stun_url ? ice.stun_url : "");
            cJSON_AddItemToArray(servers, entry);
            char *payload = cJSON_PrintUnformatted(ready);
            if (payload) { remote_send(4, payload); cJSON_free(payload); }
            cJSON_Delete(ready); continue;
        }
        if (event.type == ESP_PEER_SIGNALING_MSG_TRANSPORT_LOST) {
            if (remote_peer && !peer_auth.allowed()) close_peer();
            continue;
        }
        if (event.type == ESP_PEER_SIGNALING_MSG_BYE) { if (remote_peer) close_peer(); continue; }
        if (event.type == 1 && !remote_peer) {
            if (peer) { remote_send(4, "busy"); continue; }
            if (!RemoteAuth::configured()) { remote_send(4, "password_not_configured"); continue; }
            if (RemoteAuth::blocked()) { remote_send(4, "authentication_rate_limited"); continue; }
            peer_offer = event.data;
            esp_peer_default_cfg_t extra = peer_defaults(8);
            camera_peer_transport_ready(false);
            esp_peer_ice_server_cfg_t ice{}; remote_ice(ice);
            esp_peer_cfg_t cfg{};
            cfg.server_lists = &ice; cfg.server_num = 1;
            cfg.role = ESP_PEER_ROLE_CONTROLLED; cfg.no_auto_reconnect = true;
            cfg.enable_data_channel = true; cfg.manual_ch_create = true;
            cfg.extra_cfg = &extra; cfg.extra_size = sizeof(extra);
            cfg.on_state = on_state; cfg.on_msg = on_message;
            cfg.on_channel_open = on_channel; cfg.on_data = on_data;
            remote_peer = true;
            int ret = esp_peer_open(&cfg, esp_peer_get_default_impl(), &peer);
            if (!ret) ret = esp_peer_new_connection(peer);
            if (!ret) {
                esp_peer_msg_t msg{}; msg.type = ESP_PEER_MSG_TYPE_SDP;
                msg.data = reinterpret_cast<uint8_t *>(event.data.data()); msg.size = event.data.size();
                ret = esp_peer_send_msg(peer, &msg);
            }
            if (ret) { ESP_LOGE(TAG, "Remote peer failed: %d", ret); close_peer(); }
            activity = esp_timer_get_time();
        } else if (remote_peer && peer && (event.type == 1 || event.type == 2)) {
            esp_peer_msg_t msg{};
            msg.type = event.type == 1 ? ESP_PEER_MSG_TYPE_SDP : ESP_PEER_MSG_TYPE_CANDIDATE;
            msg.data = reinterpret_cast<uint8_t *>(event.data.data()); msg.size = event.data.size();
            esp_peer_send_msg(peer, &msg);
        }
    }
}
void start_server() {
    // ESP-IDF 6.1's initializer omits use_secure_element, which is zero initialized.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
#pragma GCC diagnostic pop
    cfg.httpd.stack_size = 8192;  // Peak use is about 4.5 KB during TLS handshakes.
    cfg.httpd.max_uri_handlers = 10;
    // Browsers open speculative TLS sockets alongside the HTML, module and
    // session requests. Two slots evict the module's connection mid-load.
    cfg.httpd.max_open_sockets = 4;
    cfg.httpd.lru_purge_enable = true;
    cfg.httpd.recv_wait_timeout = 5;
    cfg.httpd.send_wait_timeout = 5;
    cfg.user_cb = [](esp_https_server_user_cb_arg_t *event) {
        if (event->user_cb_state != HTTPD_SSL_USER_CB_SESS_ERROR) return;
        wifi_ap_record_t ap{};
        esp_wifi_sta_get_ap_info(&ap);
        ESP_LOGW(TAG, "HTTPS handshake failed: internal heap %u, largest %u, RSSI %d",
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), int(ap.rssi));
    };
    cfg.servercert = reinterpret_cast<const uint8_t *>(tls_identity_cert());
    cfg.servercert_len = strlen(tls_identity_cert()) + 1;
    cfg.prvtkey_pem = reinterpret_cast<const uint8_t *>(tls_identity_key());
    cfg.prvtkey_len = strlen(tls_identity_key()) + 1;
    // HTTPS only: the viewer's password proof needs Web Crypto, which
    // browsers provide only in secure contexts.
    ESP_ERROR_CHECK(httpd_ssl_start(&server, &cfg));
    httpd_uri_t route{};
    route.uri = "/"; route.method = HTTP_GET; route.handler = page;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
    route.uri = "/camera-settings.js"; route.handler = settings_script;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
    route.uri = "/camera-motion.js"; route.handler = motion_script;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
    route.uri = "/api/session"; route.handler = session;
    for (auto method : {HTTP_GET, HTTP_POST, HTTP_DELETE}) {
        route.method = method; ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
    }
    esp_netif_ip_info_t ip{};
    ESP_ERROR_CHECK(esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip));
    ESP_LOGI(TAG, "Local camera ready: https://" IPSTR "/", IP2STR(&ip.ip));
}
void worker(common::Application &app) {
    bool camera_ready = false;
    camera_fb_t *fb = nullptr;
    size_t offset = 0;
    uint32_t sequence = 0;
    int64_t capture_stalled_since = 0;
    int64_t last_progress = 0, next_status = 0;
    bool sntp_started = false;
    uint32_t queue_full_seen = camera_transport_queue_full();
    int64_t next_pacing_report = 0;
    uint8_t packet[CHUNK + 16];
    int64_t next_app = 0;
    for (;;) {
        if (!provisioning_is_ready()) {
            { Lock guard; if (fb) { camera_frame_return(fb); fb = nullptr; } close_peer(); }
            if (server) { ESP_ERROR_CHECK(httpd_ssl_stop(server)); server = nullptr; }
            // Keep detecting and recording while Wi-Fi is down.
            const bool motion_enabled = camera_ready && motion_settings().enabled;
            if (camera_ready && camera_capture_idle(!camera_stream_images() && !motion_enabled) &&
                motion_enabled && motion_frame_due()) {
                if (camera_fb_t *frame = camera_frame_get()) { motion_feed(frame); camera_frame_return(frame); }
            }
            vTaskDelay(pdMS_TO_TICKS(motion_enabled ? 10 : 100));
            continue;
        }
        if (!camera_ready) {
            // Modem sleep delays packets until the AP's DTIM. Continuous video
            // must not fill the TX queue while the radio is sleeping.
            const esp_err_t power = esp_wifi_set_ps(WIFI_PS_NONE);
            if (power != ESP_OK) ESP_LOGW(TAG, "Cannot disable Wi-Fi modem sleep: %s", esp_err_to_name(power));
            // The sensor bus occasionally misreads right after power-up (an
            // unrecognised sensor ID, or a register write timing out). Retry
            // from a clean driver state before giving up and restarting.
            esp_err_t started = camera_start();
            for (int attempt = 2; started != ESP_OK && attempt <= 3; ++attempt) {
                ESP_LOGW(TAG, "Camera start failed (%s); retrying (%d of 3)", esp_err_to_name(started), attempt);
                esp_camera_deinit();
                vTaskDelay(pdMS_TO_TICKS(250));
                started = camera_start();
            }
            ESP_ERROR_CHECK(started); camera_ready = true;
        }
        if (!server) start_server();
        if (!sntp_started) {
            // Clip names use wall-clock time; viewers' clocks cover the gap until this syncs.
            esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_CAMERA_SNTP_SERVER);
            sntp.wait_for_sync = false;
            sntp_started = esp_netif_sntp_init(&sntp) == ESP_OK;
        }
        {
            Lock guard;
            remote_events();
            const int64_t now = esp_timer_get_time();
            static int64_t last_heap_report = 0;
            if (now - last_heap_report >= 60000000) {
                // Wi-Fi, DMA and TLS sessions compete for internal RAM.
                last_heap_report = now;
                ESP_LOGI(TAG, "Internal RAM: free %u, largest %u, lowest %u",
                         unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                         unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                         unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
            }
            if (peer && (failed || peer_auth.expired(now) || now - activity > PEER_IDLE_US)) {
                if (fb) { camera_frame_return(fb); fb = nullptr; }
                ESP_LOGW(TAG, "Closing camera peer: failed=%d idle_ms=%lld", failed, (now - activity) / 1000);
                close_peer();
            }
            if (peer) esp_peer_main_loop(peer);
            if (!commands.empty()) {
                if (fb) { camera_frame_return(fb); fb = nullptr; }
                cJSON *request = cJSON_Parse(commands.front().c_str());
                commands.pop_front();
                if (request) {
                    const cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
                    cJSON *reply = nullptr;
                    if (cJSON_IsString(op) && !strcmp(op->valuestring, "time")) set_clock(request);
                    else if (cJSON_IsString(op) && !strcmp(op->valuestring, "recording_download")) start_transfer(request);
                    else if (cJSON_IsString(op) && !strcmp(op->valuestring, "recording_cancel")) stop_transfer();
                    else if (cJSON_IsString(op) && !strcmp(op->valuestring, "sd_format")) {
                        stop_transfer();  // Formatting needs every file on the card closed.
                        send_text(motion_command(request));
                    }
                    else if ((reply = motion_command(request))) send_text(reply);
                    else send_text(camera_command(request));
                    cJSON_Delete(request);
                }
            }
            const bool viewer = channel_ready && peer_auth.allowed();
            if (viewer && now >= next_status) {
                const bool overlay = camera_stream_images() && motion_overlay_active();
                send_text(motion_status(overlay));
                next_status = now + (overlay ? 500000 : 1000000);
            }
            bool reply_waiting = false;
            while (channel_ready && !replies.empty()) {
                std::string &text = replies.front();
                esp_peer_data_frame_t reply{};
                reply.type = ESP_PEER_DATA_CHANNEL_STRING; reply.stream_id = stream_id;
                reply.data = reinterpret_cast<uint8_t *>(&text[0]); reply.size = text.size();
                int ret = esp_peer_send_data(peer, &reply);
                if (ret == 0) { replies.pop_front(); continue; }
                if (ret != ESP_PEER_ERR_WOULD_BLOCK && ret != ESP_PEER_ERR_NO_MEM) {
                    ESP_LOGW(TAG, "Control send failed: result=%d bytes=%u", ret, unsigned(text.size()));
                    failed = true;
                }
                break;
            }
            reply_waiting = !replies.empty();
            const bool downloading = viewer && transfer.fd >= 0;
            const bool images_enabled = camera_stream_images();
            const bool motion_enabled = motion_settings().enabled;
            if (fb && (!viewer || !images_enabled)) { camera_frame_return(fb); fb = nullptr; }
            const bool capture_ready = camera_capture_idle(!images_enabled && !motion_enabled);
            if (!images_enabled && !motion_enabled) capture_stalled_since = 0;
            const bool bulk_traffic = viewer && (images_enabled || downloading);
            // Backpressure slows bulk traffic: blocked control replies, a full
            // Wi-Fi TX queue, or internal RAM running low.
            const uint32_t queue_full = camera_transport_queue_full();
            const bool low_memory = bulk_traffic && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < PACING_LOW_MEMORY;
            if (bulk_traffic && (reply_waiting || queue_full != queue_full_seen || low_memory)) {
                slow_reply += reply_waiting; slow_queue += queue_full != queue_full_seen; slow_memory += low_memory;
                pacer.congested(now);
            }
            queue_full_seen = queue_full;
            // True when the Wi-Fi TX queue filled since the last check; slows the pacer.
            auto queue_filled = [&] {
                const uint32_t count = camera_transport_queue_full();
                if (count == queue_full_seen) return false;
                queue_full_seen = count; ++slow_queue;
                pacer.congested(esp_timer_get_time());
                return true;
            };
            if (bulk_traffic && now >= next_pacing_report) {
                next_pacing_report = now + 5000000;
                wifi_ap_record_t ap{};
                esp_wifi_sta_get_ap_info(&ap);
                ESP_LOGI(TAG, "Send pacing: %u KiB/s, %u slowdowns (send cache %u, TX queue %u, replies %u, memory %u), RSSI %d dBm",
                         unsigned(pacer.rate() / 1024), unsigned(pacer.cuts()), unsigned(slow_cache), unsigned(slow_queue),
                         unsigned(slow_reply), unsigned(slow_memory), int(ap.rssi));
            }
            // Wi-Fi needs contiguous internal RAM even though images live in PSRAM.
            // Stop admitting video and recording transfers under pressure; let
            // SCTP acknowledgements drain its cache while still servicing
            // settings and heartbeats.
            const bool video_memory = bulk_traffic && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 12288 &&
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 2048;
            if (bulk_traffic && !video_memory) {
                static int64_t last_memory_report = 0;
                if (now - last_memory_report > 10000000) {
                    last_memory_report = now;
                    ESP_LOGW(TAG, "Video paused for internal memory: free=%u largest=%u",
                        unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                        unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
                }
            }
            const bool streaming = viewer && images_enabled && capture_ready && !reply_waiting && !downloading && video_memory;
            if (downloading && !reply_waiting && video_memory && pacer.due(now)) {
                // Abandon a partly sent frame; the viewer discards incomplete frames.
                if (fb) { camera_frame_return(fb); fb = nullptr; }
                for (int chunks = 0; transfer.fd >= 0 && chunks < BULK_CHUNKS_PER_PASS && pacer.due(esp_timer_get_time()); ++chunks) {
                    const uint32_t size = std::min<uint32_t>(CHUNK, transfer.size - transfer.offset);
                    if (transfer.buffered != transfer.offset &&
                        (lseek(transfer.fd, transfer.offset, SEEK_SET) < 0 || read(transfer.fd, packet + 16, size) != ssize_t(size))) {
                        cJSON *error = cJSON_CreateObject();
                        cJSON_AddStringToObject(error, "type", "download");
                        cJSON_AddNumberToObject(error, "id", transfer.id);
                        cJSON_AddStringToObject(error, "error", "SD card read failed");
                        send_text(error); stop_transfer();
                        break;
                    }
                    transfer.buffered = transfer.offset;
                    // Four LE uint32: magic "FILE", transfer id, total bytes, byte offset.
                    const uint32_t header[] = {0x454c4946, transfer.id, transfer.size, transfer.offset};
                    memcpy(packet, header, sizeof(header));
                    esp_peer_data_frame_t chunk{};
                    chunk.type = ESP_PEER_DATA_CHANNEL_DATA; chunk.stream_id = stream_id;
                    chunk.data = packet; chunk.size = size + 16;
                    int ret = esp_peer_send_data(peer, &chunk);
                    if (ret != 0) {
                        if (ret == ESP_PEER_ERR_WOULD_BLOCK || ret == ESP_PEER_ERR_NO_MEM) { ++slow_cache; pacer.congested(esp_timer_get_time()); }
                        else failed = true;
                        break;
                    }
                    activity = esp_timer_get_time();
                    pacer.sent(size + 16, activity);
                    transfer.offset += size;
                    if (transfer.offset == transfer.size) stop_transfer();
                    // A full Wi-Fi TX queue ends the burst so the worker services
                    // acknowledgements and controls before sending more.
                    if (queue_filled()) break;
                }
            }
            // Motion detection and recording capture frames with or without a viewer.
            if (!fb && capture_ready && (streaming || (motion_enabled && motion_frame_due()))) {
                fb = camera_frame_get(streaming);
                if (streaming) { offset = 0; ++sequence; last_progress = esp_timer_get_time(); }
                if (fb) { capture_stalled_since = 0; motion_feed(fb); }
                else if (!capture_stalled_since) capture_stalled_since = esp_timer_get_time();
                else if (esp_timer_get_time() - capture_stalled_since >= 8000000) {
                    // The OV2640 capture pipeline can stall across control
                    // changes/reconnects. Recover with no held buffer and
                    // retain the connection and all current user settings.
                    ESP_LOGW(TAG, "Restarting stalled camera capture");
                    ESP_ERROR_CHECK(camera_recover());
                    capture_stalled_since = 0;
                    if (viewer) send_text(camera_settings());
                }
                if (fb && !streaming) { camera_frame_return(fb); fb = nullptr; }
            }
            if (streaming && pacer.due(now)) {
                // Pace bulk packets; continuous capture must not saturate Wi-Fi
                // and leave DTLS unable to send acknowledgements or controls.
                for (int chunks = 0; fb && chunks < BULK_CHUNKS_PER_PASS; ++chunks) {
                    size_t size = std::min(CHUNK, fb->len - offset);
                    // Four LE uint32: magic, sequence, total bytes, byte offset.
                    const uint32_t header[] = {0x4745504a, sequence, uint32_t(fb->len), uint32_t(offset)};
                    memcpy(packet, header, sizeof(header)); memcpy(packet + 16, fb->buf + offset, size);
                    esp_peer_data_frame_t frame{};
                    frame.type = ESP_PEER_DATA_CHANNEL_DATA; frame.stream_id = stream_id;
                    frame.data = packet; frame.size = size + 16;
                    int ret = esp_peer_send_data(peer, &frame);
                    if (ret == 0) {
                        offset += size; last_progress = esp_timer_get_time();
                        pacer.sent(size + 16, last_progress);
                    } else if (ret == ESP_PEER_ERR_WOULD_BLOCK || ret == ESP_PEER_ERR_NO_MEM) {
                        ++slow_cache; pacer.congested(esp_timer_get_time());
                    }
                    if (offset == fb->len || esp_timer_get_time() - last_progress > 2000000 || (ret != 0 && ret != ESP_PEER_ERR_WOULD_BLOCK && ret != ESP_PEER_ERR_NO_MEM)) {
                        camera_frame_return(fb); fb = nullptr;
                    }
                    // A full Wi-Fi TX queue ends the burst, as for downloads above.
                    if (ret != 0 || queue_filled() || !pacer.due(esp_timer_get_time())) break;
                }
            }
            if (!viewer && !motion_enabled) capture_stalled_since = 0;
        }
        if (esp_timer_get_time() >= next_app) { app.main(); next_app = esp_timer_get_time() + 20000; }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
}
extern "C" void app_main() {
    static common::Application app(common_board_config);
    // Wi-Fi enables the hardware entropy source before generating secrets.
    // Provisioning generates the shared identity before opening its server.
    ESP_ERROR_CHECK(app.setup());
    ESP_ERROR_CHECK(media_lib_add_default_adapter());
    remote_start();
    mutex = xSemaphoreCreateRecursiveMutex();
    ESP_ERROR_CHECK(mutex ? ESP_OK : ESP_ERR_NO_MEM);
    motion_start();
    // The peer worker runs in the main task, which also polls the setup
    // button: one stack instead of two leaves internal RAM for motion analysis.
    vTaskPrioritySet(nullptr, 5);
    worker(app);
}
