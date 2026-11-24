#include "common/application.h"
#include "diagnostics.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "provisioning.h"
#include "local_http.h"
#include "tls_identity.h"
#include "camera_controls.h"
#include "motion.h"
#include "motion_capture_profile.h"
#include "jpeg_dimensions.h"
#include "recorder.h"
#include "remote_signaling.h"
#include "esp_peer_signaling.h"
#include "remote_auth.h"
#include "peer_receive.h"
#include "peer_send.h"
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
#include "esp_event.h"
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
// Data-channel message payload for video and downloads. Each message costs one
// pass through esp_peer's SCTP and DTLS send path, so larger messages spend less
// time per byte there; SCTP still splits each into MTU-sized datagrams.
constexpr size_t CHUNK = 2048;
// A viewer pings every 5 seconds. Browsers throttle timers in background tabs
// and on sleeping phones, so the camera keeps the session through up to five
// minutes without any message and only then disconnects it. Recording
// transfers count as activity too. Closed tabs disconnect at once.
constexpr int64_t PEER_IDLE_US = 5 * 60 * 1000000LL;
// Bulk traffic (video, recording downloads) is not rate limited. Each worker
// pass sends up to 24 KiB, and a burst ends early when the SCTP send cache or
// the Wi-Fi TX queue is full, so the loop can service acknowledgements and
// control replies before sending more.
constexpr int BULK_CHUNKS_PER_PASS = std::max<int>(1, 24576 / CHUNK);
// Backpressure seen on bulk sends, for the periodic transport log: a full SCTP
// send cache, a full Wi-Fi TX queue, and passes where a control reply waited.
uint32_t full_cache, full_queue, reply_blocked;
// Where the worker's time goes while streaming, reset with each transport log:
// video frames and bytes sent, and microseconds spent in the peer loop
// (receive and acknowledgements), waiting for camera frames, and sending chunks.
struct StreamTiming {
    uint32_t frames = 0, passes = 0;
    uint64_t bytes = 0;
    int64_t loop_us = 0, capture_us = 0, send_us = 0, since = 0;
} timing;
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
int64_t activity = 0, next_logs = 0;
extern const unsigned char page_start[] asm("_binary_viewer_html_start");
extern const unsigned char page_end[] asm("_binary_viewer_html_end");
extern const unsigned char settings_start[] asm("_binary_settings_js_start");
extern const unsigned char settings_end[] asm("_binary_settings_js_end");
extern const unsigned char motion_start_js[] asm("_binary_motion_js_start");
extern const unsigned char motion_end_js[] asm("_binary_motion_js_end");
extern const unsigned char logs_start[] asm("_binary_logs_js_start");
extern const unsigned char logs_end[] asm("_binary_logs_js_end");
// One recording download at a time; its chunks replace video frames until done.
// `buffered` is the offset of the chunk already read into the packet buffer,
// kept for a retry after backpressure instead of reading the card again.
struct Transfer {
    int fd = -1;
    uint32_t id = 0, size = 0, offset = 0, buffered = UINT32_MAX;
    bool active() const { return fd >= 0; }
} transfer;
uint32_t transfer_ids = 0;
bool playback_active = false, media_paused = false;
// Board restarts for a camera that would not recover. Kept in RTC memory
// across software resets so a sensor that never returns cannot cause an
// endless reboot loop; cleared by power-on and by the first good frame.
RTC_NOINIT_ATTR uint32_t camera_restart_magic, camera_restarts;
constexpr uint32_t kCameraRestartMagic = 0x43414d52;
constexpr uint32_t kMaxCameraRestarts = 2;
uint32_t &camera_restart_count() {
    if (camera_restart_magic != kCameraRestartMagic) { camera_restart_magic = kCameraRestartMagic; camera_restarts = 0; }
    return camera_restarts;
}
void pause_media(bool paused) {
    if (paused == media_paused) return;
    motion_pause(paused);
    recorder_pause(paused);
    media_paused = paused;
    diagnostic_event("info", paused ? "Capture, analysis and recording paused for download/playback" : "Download/playback ended; capture, analysis and recording resumed");
}

struct Lock {
    Lock() { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(mutex); }
};
int send_text(cJSON *json) {
    if (!json) return -1;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
    if (cJSON_IsString(error)) diagnostic_event("error", error->valuestring);
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
    if (transfer.active()) {
        diagnostic_eventf("info", "Download %s: %u/%u bytes", transfer.offset == transfer.size ? "completed" : "stopped", unsigned(transfer.offset), unsigned(transfer.size));
        if (transfer.fd >= 0) close(transfer.fd);
    }
    transfer.fd = -1;
    transfer.buffered = UINT32_MAX;
    pause_media(playback_active);
}
void start_transfer(const cJSON *request) {
    stop_transfer();
    pause_media(true);
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(request, "name");
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "download");
    int fd = -1;
    uint32_t size = 0;
    const char *error = cJSON_IsString(name) ? recorder_open(name->valuestring, fd, size) : "Missing recording name";
    if (cJSON_IsString(name)) cJSON_AddStringToObject(reply, "name", name->valuestring);
    if (error) { cJSON_AddStringToObject(reply, "error", error); pause_media(playback_active); }
    else {
        transfer = {fd, ++transfer_ids, size, 0};
        diagnostic_eventf("info", "Download started: %u bytes", unsigned(size));
        cJSON_AddNumberToObject(reply, "id", transfer.id);
        cJSON_AddNumberToObject(reply, "size", size);
    }
    send_text(reply);
}
#if CONFIG_IDF_TARGET_ARCH_XTENSA
const char *exception_name(uint32_t cause) {
    switch (cause) {
    case 0: return "IllegalInstruction";
    case 1: return "Syscall";
    case 2: return "InstructionFetchError";
    case 3: return "LoadStoreError";
    case 4: return "Level1Interrupt";
    case 5: return "Alloca";
    case 6: return "IntegerDivideByZero";
    case 8: return "Privileged";
    case 9: return "LoadStoreAlignment";
    case 12: return "InstrPIFDataError";
    case 13: return "LoadStorePIFDataError";
    case 14: return "InstrPIFAddrError";
    case 15: return "LoadStorePIFAddrError";
    case 16: return "InstTLBMiss";
    case 17: return "InstTLBMultiHit";
    case 18: return "InstFetchPrivilege";
    case 20: return "InstFetchProhibited";
    case 24: return "LoadStoreTLBMiss";
    case 25: return "LoadStoreTLBMultiHit";
    case 26: return "LoadStorePrivilege";
    case 28: return "LoadProhibited";
    case 29: return "StoreProhibited";
    default: return "unknown";
    }
}
#endif

// The saved core dump holds raw task stacks, which can contain keys,
// passwords, signaling URLs and IP addresses, so it never leaves the board.
// Viewers get this text summary instead: the crashed task's name, exception
// cause, faulting address and code addresses only. CPU registers are left out
// because they can hold data values.
void send_crash_summary(const cJSON *request) {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "crash_summary");
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(request, "request_id");
    const double value = cJSON_IsNumber(token) ? token->valuedouble : 0;
    cJSON_AddNumberToObject(reply, "request_id", value >= 1 && value <= UINT32_MAX ? uint32_t(value) : 0);
    auto *summary = static_cast<esp_core_dump_summary_t *>(calloc(1, sizeof(esp_core_dump_summary_t)));
    const esp_err_t result = summary ? esp_core_dump_get_summary(summary) : ESP_ERR_NO_MEM;
    if (result != ESP_OK) {
        cJSON_AddStringToObject(reply, "error", result == ESP_ERR_NO_MEM ? "Not enough memory for the crash summary"
                                                                          : "No saved crash on this camera");
        free(summary);
        send_text(reply);
        return;
    }
    char running[65] = {};
    esp_app_get_elf_sha256(running, sizeof(running));
    char task[sizeof(summary->exc_task) + 1] = {}, elf[sizeof(summary->app_elf_sha256) + 1] = {};
    memcpy(task, summary->exc_task, sizeof(summary->exc_task));
    memcpy(elf, summary->app_elf_sha256, sizeof(summary->app_elf_sha256));
    std::string text = "Camera crash summary\n";
    char line[160];
    const auto add = [&](const char *format, auto... values) { snprintf(line, sizeof(line), format, values...); text += line; };
    add("Crashed firmware ELF SHA-256 (prefix): %s\n", elf);
    add("Running firmware: %s, ELF SHA-256 %s (%s)\n", esp_app_get_description()->version, running,
        elf[0] && !strncmp(running, elf, strlen(elf)) ? "same build" : "different build");
    add("Core dump format: 0x%08x\n", unsigned(summary->core_dump_version));
    add("Task: %s\n", task[0] ? task : "(unknown)");
    add("PC: 0x%08x\n", unsigned(summary->exc_pc));
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    add("Exception cause: %u (%s)\n", unsigned(summary->ex_info.exc_cause), exception_name(summary->ex_info.exc_cause));
    add("Faulting address: 0x%08x\n", unsigned(summary->ex_info.exc_vaddr));
    for (int level = 0; level < EPCx_REGISTER_COUNT; ++level) {
        if (summary->ex_info.epcx_reg_bits & (1 << level)) add("EPC%d: 0x%08x\n", level + 1, unsigned(summary->ex_info.epcx[level]));
    }
#endif
    const uint32_t depth = std::min<uint32_t>(summary->exc_bt_info.depth, std::size(summary->exc_bt_info.bt));
    add("Backtrace (%u entries%s):", unsigned(depth), summary->exc_bt_info.corrupted ? ", corrupted" : "");
    std::string addresses;
    for (uint32_t i = 0; i < depth; ++i) { add(" 0x%08x", unsigned(summary->exc_bt_info.bt[i])); addresses += line; }
    text += "\n\nDecode with the matching build's webrtc.elf:\n";
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    text += "xtensa-" CONFIG_IDF_TARGET "-elf-addr2line -pfiaC -e webrtc.elf";
#else
    text += "riscv32-esp-elf-addr2line -pfiaC -e webrtc.elf";
#endif
    // The backtrace normally starts at the PC; list it separately only if not.
    if (!depth || summary->exc_bt_info.bt[0] != summary->exc_pc) {
        snprintf(line, sizeof(line), " 0x%08x", unsigned(summary->exc_pc));
        text += line;
    }
    text += addresses + "\n";
    free(summary);
    cJSON_AddStringToObject(reply, "text", text.c_str());
    diagnostic_event("info", "Crash summary sent to viewer");
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
    diagnostic_event("info", ("WebRTC peer state " + std::to_string(int(state))).c_str());
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
        diagnostic_event(peer_auth.allowed() ? "info" : "warning", peer_auth.allowed() ? "Viewer authenticated" : "Viewer authentication rejected");
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
    if (peer) diagnostic_event("info", "Viewer disconnected");
    next_logs = 0;
    channel_ready = false;
    camera_peer_transport_ready(false);
    if (peer) { esp_peer_close(peer); peer = nullptr; }
    playback_active = false;
    stop_transfer();
    answer.clear(); replies.clear(); commands.clear(); peer_offer.clear(); peer_auth.clear(); failed = false; remote_peer = false;
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
esp_err_t logs_script(httpd_req_t *r) {
    if (!same_origin(r)) return respond(r, "403 Forbidden", "{}");
    httpd_resp_set_type(r, "application/javascript");
    return httpd_resp_send(r, reinterpret_cast<const char *>(logs_start), logs_end - logs_start - 1);
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
    if (ret != 0) { diagnostic_event("error", ("Peer creation failed: " + std::to_string(ret)).c_str()); ESP_LOGE(TAG, "Peer creation/SDP failed: %d", ret); close_peer(); return respond(r, "503 Service Unavailable", "{\"error\":\"Peer creation failed\"}"); }
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
        if (event.type == ESP_PEER_SIGNALING_MSG_BYE) {
            // Signaling is shared by whoever joins the room next. Once the
            // viewer has proven the password, only its own peer connection
            // (channel close, failure or heartbeat timeout) ends the session,
            // so a refused second viewer's bye cannot disconnect it.
            if (remote_peer && !peer_auth.allowed()) close_peer();
            continue;
        }
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
            if (ret) { diagnostic_event("error", ("Remote peer creation failed: " + std::to_string(ret)).c_str()); ESP_LOGE(TAG, "Remote peer failed: %d", ret); close_peer(); }
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
        diagnostic_event("warning", "Local HTTPS handshake failed");
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
    route.uri = "/camera-logs.js"; route.handler = logs_script;
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
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
        [](void *, esp_event_base_t, int32_t, void *data) {
            const auto *event = static_cast<wifi_event_sta_disconnected_t *>(data);
            diagnostic_eventf("warning", "Wi-Fi disconnected: reason=%u RSSI=%d", unsigned(event->reason), int(event->rssi));
        }, nullptr));
    char boot[80];
    snprintf(boot, sizeof(boot), "Application started; reset reason %d", int(esp_reset_reason()));
    diagnostic_event("info", boot);
    diagnostic_eventf("info", "Board %s; firmware %s; IDF %s", app.board().name, esp_app_get_description()->version, esp_app_get_description()->idf_ver);
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC: diagnostic_event("error", "Previous boot ended in a panic"); break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: diagnostic_event("error", "Previous boot ended in a watchdog reset"); break;
    case ESP_RST_BROWNOUT: diagnostic_event("error", "Brownout reset: supply voltage dropped"); break;
    case ESP_RST_POWERON: diagnostic_event("info", "Power-on reset"); break;
    default: break;
    }
    diagnostic_boot();
    bool network_ready = false;
    bool camera_ready = false, capture_fault = false;
    if (esp_reset_reason() == ESP_RST_POWERON || esp_reset_reason() == ESP_RST_BROWNOUT) camera_restart_count() = 0;
    // Recovery after a failed start or a stalled capture: retry after 2 s and
    // 10 s, cutting sensor power each time; then restart the board (limited by
    // kMaxCameraRestarts); then keep retrying once a minute.
    int recovery_attempts = 0;
    int64_t next_recovery = 0;
    bool camera_proven = false, fault_reported = false, settings_pending = false;
    const auto camera_failed = [&](esp_err_t err, const char *what) {
        capture_fault = true;
        ++recovery_attempts;
        diagnostic_eventf("error", "%s failed: %s (0x%x)", what, esp_err_to_name(err), unsigned(err));
        ESP_LOGE(TAG, "%s failed: %s (attempt %d)", what, esp_err_to_name(err), recovery_attempts);
        const int64_t now = esp_timer_get_time();
        if (recovery_attempts < 3) { next_recovery = now + (recovery_attempts == 1 ? 2000000 : 10000000); return; }
        uint32_t &restarts = camera_restart_count();
        if (restarts < kMaxCameraRestarts) {
            ++restarts;
            diagnostic_event("error", "Camera did not recover; restarting the board");
            ESP_LOGE(TAG, "Camera did not recover; restarting (%u of %u)", unsigned(restarts), unsigned(kMaxCameraRestarts));
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        if (recovery_attempts == 3) diagnostic_event("error", "Camera restart limit reached; retrying once a minute");
        next_recovery = now + 60000000;
    };
    const auto frame_ok = [&] {
        if (camera_proven) return;
        camera_proven = true;
        camera_restart_count() = 0;
    };
    camera_fb_t *fb = nullptr;
    size_t offset = 0;
    uint32_t sequence = 0;
    int64_t capture_stalled_since = 0;
    int64_t last_progress = 0, next_status = 0;
    bool sntp_started = false;
    uint32_t queue_full_seen = camera_transport_queue_full();
    int64_t next_transport_report = 0;
    // Header plus one chunk; too large for the worker's stack, so it lives in PSRAM.
    auto *packet = static_cast<uint8_t *>(heap_caps_malloc(CHUNK + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    configASSERT(packet);
    motion::CaptureProfile motion_capture;
    const auto select_capture = [&](bool live, bool paused, ManualRecording manual) {
        const motion::Activity event = motion_activity();
        const MotionSettings motion = motion_settings();
        const bool triggered = motion_capture.update(esp_timer_get_time(), motion, event,
                                                       !paused && !capture_fault && recorder_available() &&
                                                       manual == ManualRecording::idle);
        if (!camera_ready || paused || capture_fault) {
            recorder_set_motion_capture(false);
            return false;
        }
        // The camera captures 160x120 by default, whether or not detection is
        // on (its checkbox only runs the algorithm). Only live video and SD
        // recording use the selected resolution.
        const bool idle = !live && !triggered && manual == ManualRecording::idle;
        if (camera_capture_change_needed(idle) && fb) {
            camera_frame_return(fb);
            fb = nullptr;
        }
        const esp_err_t switched = camera_select_motion_idle(idle);
        recorder_set_motion_capture(switched == ESP_OK && triggered);
        if (switched != ESP_OK) {
            camera_failed(switched, "Camera capture mode change");
            return false;
        }
        return true;
    };
    // Frames captured before a size change are dropped. A size whose JPEG
    // never matches the driver's table would drop every frame and end in
    // stall recovery, so each mismatched size is reported once.
    uint32_t reported_mismatch = 0;
    // Frames grabbed from the camera per second for any use: live video,
    // motion analysis or recording. Viewers show it as the camera's FPS.
    uint32_t grabbed = 0;
    int64_t grab_window = esp_timer_get_time();
    float grab_fps = 0;
    const auto get_frame = [&](bool prefetch = false) {
        auto *frame = camera_frame_get(prefetch);
        if (frame && frame->format == PIXFORMAT_JPEG &&
            !jpeg_matches_capture_size(frame->buf, frame->len, frame->width, frame->height)) {
            const uint32_t size = uint32_t(frame->width) << 16 | frame->height;
            if (size != reported_mismatch) {
                reported_mismatch = size;
                diagnostic_eventf("warning", "Dropped a JPEG whose size is not %ux%u", unsigned(frame->width), unsigned(frame->height));
            }
            camera_frame_return(frame);
            return static_cast<camera_fb_t *>(nullptr);
        }
        if (frame) ++grabbed;
        return frame;
    };
    int64_t next_app = 0;
    for (;;) {
        if (camera_ready && capture_fault && esp_timer_get_time() >= next_recovery) {
            // A faulted camera holds no framebuffer: the stall that caused it
            // had none, and capture stays off while faulted.
            diagnostic_eventf("warning", "Camera recovery attempt %d with sensor power cycle", recovery_attempts + 1);
            const esp_err_t recovered = camera_recover(true);
            if (recovered == ESP_OK) {
                capture_fault = false;
                recovery_attempts = 0;
                settings_pending = true;
                diagnostic_event("info", "Camera recovered");
            } else camera_failed(recovered, "Camera recovery");
        }
        if (!provisioning_is_ready()) {
            if (network_ready) diagnostic_event("warning", "Wi-Fi unavailable");
            network_ready = false;
            { Lock guard; if (fb) { camera_frame_return(fb); fb = nullptr; } close_peer(); }
            if (server) { ESP_ERROR_CHECK(httpd_ssl_stop(server)); server = nullptr; }
            // Keep detecting and recording while Wi-Fi is down.
            const auto manual = recorder_manual_state();
            const bool recording = manual == ManualRecording::active;
            const bool motion_enabled = camera_ready && manual == ManualRecording::idle && motion_settings().enabled;
            if (select_capture(false, false, manual) && camera_capture_idle(!motion_enabled && !recording) &&
                (recorder_manual_frame_due() || (motion_enabled && motion_frame_due()))) {
                if (camera_fb_t *frame = get_frame()) {
                    frame_ok();
                    if (recording) recorder_manual_feed(frame); else motion_feed(frame);
                    camera_frame_return(frame);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(recording || motion_enabled ? 10 : 100));
            continue;
        }
        if (!network_ready) diagnostic_event("info", "Wi-Fi ready");
        network_ready = true;
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
                diagnostic_event("error", "Camera initialization failed; retrying");
                ESP_LOGW(TAG, "Camera start failed (%s); retrying (%d of 3)", esp_err_to_name(started), attempt);
                esp_camera_deinit();
                vTaskDelay(pdMS_TO_TICKS(250));
                started = camera_start();
            }
            // Never abort: keep Wi-Fi, viewers, logs and core dump download
            // reachable, and recover the camera on the schedule above.
            camera_ready = true;
            if (started == ESP_OK) diagnostic_event("info", "Camera initialized");
            else camera_failed(started, "Camera initialization");
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
                wifi_ap_record_t ap{};
                esp_wifi_sta_get_ap_info(&ap);
                diagnostic_eventf("info", "Health RAM free=%u largest=%u minimum=%u; Wi-Fi RSSI=%d dBm",
                    unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                    unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                    unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), int(ap.rssi));
                ESP_LOGI(TAG, "Internal RAM: free %u, largest %u, lowest %u",
                         unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                         unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                         unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
            }
            if (peer && (failed || peer_auth.expired(now) || now - activity > PEER_IDLE_US)) {
                if (fb) { camera_frame_return(fb); fb = nullptr; }
                diagnostic_event("warning", failed ? "Peer transport failed" : peer_auth.expired(now) ? "Viewer authentication timed out" : "Viewer heartbeat timed out");
                ESP_LOGW(TAG, "Closing camera peer: failed=%d idle_ms=%lld", failed, (now - activity) / 1000);
                close_peer();
            }
            if (peer) {
                const int64_t started = esp_timer_get_time();
                esp_peer_main_loop(peer);
                timing.loop_us += esp_timer_get_time() - started;
                ++timing.passes;
            }
            if (!commands.empty()) {
                if (fb) { camera_frame_return(fb); fb = nullptr; }
                cJSON *request = cJSON_Parse(commands.front().c_str());
                commands.pop_front();
                if (request) {
                    const cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
                    cJSON *reply = nullptr;
                    if (cJSON_IsString(op) && !strcmp(op->valuestring, "logs_get")) {
                        if (now >= next_logs && replies.size() < 4) {
                            const cJSON *after = cJSON_GetObjectItemCaseSensitive(request, "after");
                            const double cursor = cJSON_IsNumber(after) ? after->valuedouble : 0;
                            cJSON *logs = diagnostic_logs(cursor >= 0 && cursor <= UINT32_MAX ? uint32_t(cursor) : 0);
                            if (cursor == 0) {
                                const auto *description = esp_app_get_description();
                                cJSON_AddStringToObject(logs, "board", app.board().name);
                                cJSON_AddStringToObject(logs, "firmware", description->version);
                                cJSON_AddStringToObject(logs, "idf", description->idf_ver);
                                char hash[65];
                                for (unsigned i = 0; i < 32; ++i) snprintf(hash + 2 * i, 3, "%02x", description->app_elf_sha256[i]);
                                cJSON_AddStringToObject(logs, "elf_sha256", hash);
                                cJSON_AddNumberToObject(logs, "reset_reason", int(esp_reset_reason()));
                            }
                            send_text(logs);
                            next_logs = now + 2000000;
                        }
                    }
                    else if (cJSON_IsString(op) && !strcmp(op->valuestring, "logs_clear")) {
                        const uint32_t cleared = diagnostic_clear();
                        diagnostic_event("info", "Diagnostic history cleared by viewer");
                        // Replies stay in order: the viewer drops log replies
                        // until this acknowledgement, which starts after the clear.
                        cJSON *logs = diagnostic_logs(cleared);
                        cJSON_AddBoolToObject(logs, "cleared", true);
                        send_text(logs);
                    }
                    else if (cJSON_IsString(op) && !strcmp(op->valuestring, "time")) set_clock(request);
                    else if (cJSON_IsString(op) && !strcmp(op->valuestring, "recording_download")) start_transfer(request);
                    else if (cJSON_IsString(op) && !strcmp(op->valuestring, "recording_cancel")) stop_transfer();
                    else if (cJSON_IsString(op) && !strcmp(op->valuestring, "crash_summary")) send_crash_summary(request);
                    else if (cJSON_IsString(op) && !strcmp(op->valuestring, "recording_playback")) {
                        playback_active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "active"));
                        pause_media(playback_active || transfer.active());
                    }
                    else if (cJSON_IsString(op) && !strcmp(op->valuestring, "sd_format")) {
                        stop_transfer();  // Formatting needs every file on the card closed.
                        send_text(motion_command(request));
                    }
                    else if ((reply = motion_command(request))) send_text(reply);
                    else if (!capture_fault && (!media_paused || (cJSON_IsString(op) && !strcmp(op->valuestring, "get")))) send_text(camera_command(request));
                    else {
                        cJSON *error = cJSON_CreateObject();
                        cJSON_AddStringToObject(error, "type", "error");
                        cJSON_AddStringToObject(error, "error", capture_fault
                            ? "The camera sensor is not responding; recovery is in progress."
                            : "Camera settings are paused during downloads and recorded video playback.");
                        send_text(error);
                    }
                    cJSON_Delete(request);
                }
            }
            const bool viewer = channel_ready && peer_auth.allowed();
            if (!capture_fault) fault_reported = false;
            else if (viewer && !fault_reported) {
                cJSON *error = cJSON_CreateObject();
                cJSON_AddStringToObject(error, "type", "error");
                cJSON_AddStringToObject(error, "error", "The camera sensor is not responding; recovery is in progress.");
                send_text(error);
                fault_reported = true;
            }
            if (viewer && settings_pending) { send_text(camera_settings()); settings_pending = false; }
            const auto manual = recorder_manual_state();
            if (viewer && now >= next_status) {
                // Viewers draw the moving cells over the picture, or over a blank
                // canvas while image streaming is off.
                const bool overlay = manual == ManualRecording::idle && motion_overlay_active();
                cJSON *status = motion_status(overlay);
                cJSON_AddNumberToObject(status, "capture_fps", std::round(double(grab_fps) * 10) / 10);
                send_text(status);
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
                    diagnostic_event("error", ("Control send failed: " + std::to_string(ret)).c_str());
                    ESP_LOGW(TAG, "Control send failed: result=%d bytes=%u", ret, unsigned(text.size()));
                    failed = true;
                }
                break;
            }
            reply_waiting = !replies.empty();
            const bool downloading = viewer && transfer.active();
            const bool images_enabled = camera_stream_images();
            const bool motion_enabled = manual == ManualRecording::idle && motion_settings().enabled;
            const bool recording = manual == ManualRecording::active;
            if (fb && (!viewer || !images_enabled)) { camera_frame_return(fb); fb = nullptr; }
            const bool live = viewer && images_enabled;
            const bool capture_ready = select_capture(live, media_paused, manual) &&
                                       camera_capture_idle(!live && !motion_enabled && !recording);
            if (!images_enabled && !motion_enabled && !recording) capture_stalled_since = 0;
            const bool bulk_traffic = viewer && (images_enabled || downloading);
            if (bulk_traffic && reply_waiting) ++reply_blocked;
            if (!bulk_traffic) timing = StreamTiming{};  // Each stream's first window starts when it does.
            // True when the Wi-Fi TX queue filled since the last check; ends the burst.
            auto queue_filled = [&] {
                const uint32_t count = camera_transport_queue_full();
                if (count == queue_full_seen) return false;
                full_queue += count - queue_full_seen;
                queue_full_seen = count;
                return true;
            };
            queue_filled();
            if (bulk_traffic && now >= next_transport_report) {
                next_transport_report = now + 5000000;
                wifi_ap_record_t ap{};
                esp_wifi_sta_get_ap_info(&ap);
                const int64_t window = std::max<int64_t>(1, now - timing.since);
                const auto share = [&](int64_t us) { return unsigned(us * 100 / window); };
                ESP_LOGI(TAG, "Transport: send cache full %u, TX queue full %u, reply waits %u, RSSI %d dBm",
                         unsigned(full_cache), unsigned(full_queue), unsigned(reply_blocked), int(ap.rssi));
                if (timing.since) ESP_LOGI(TAG, "Stream timing: %u.%u fps, %u KiB/s, avg frame %u B, %u passes; time in peer loop %u%%, camera wait %u%%, chunk send %u%% (%u us/KiB, %u B chunks)",
                         unsigned(timing.frames * 10000000LL / window / 10), unsigned(timing.frames * 10000000LL / window % 10), unsigned(timing.bytes * 1000000 / window / 1024),
                         unsigned(timing.frames ? timing.bytes / timing.frames : 0), unsigned(timing.passes),
                         share(timing.loop_us), share(timing.capture_us), share(timing.send_us),
                         unsigned(timing.bytes ? timing.send_us * 1024 / int64_t(timing.bytes) : 0), unsigned(CHUNK));
                camera_udp_stats_t udp{};
                camera_transport_udp_stats(&udp);
                if (timing.since) ESP_LOGI(TAG, "UDP send: %u datagrams (%u B avg), in sendto %u%% (%u us avg, max %u us), %u calls >= 15 ms, shim retries %u%%",
                         unsigned(udp.calls), unsigned(udp.calls ? udp.bytes / udp.calls : 0), share(udp.us),
                         unsigned(udp.calls ? udp.us / udp.calls : 0), unsigned(udp.max_us), unsigned(udp.slow), share(udp.retry_us));
                timing = StreamTiming{};
                timing.since = now;
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
                    diagnostic_event("warning", "Video paused: insufficient internal RAM");
                    ESP_LOGW(TAG, "Video paused for internal memory: free=%u largest=%u",
                        unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                        unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
                }
            }
            const bool streaming = viewer && images_enabled && capture_ready && !reply_waiting && !downloading && video_memory;
            if (downloading && !reply_waiting && video_memory) {
                // Abandon a partly sent frame; the viewer discards incomplete frames.
                if (fb) { camera_frame_return(fb); fb = nullptr; }
                for (int chunks = 0; transfer.active() && chunks < BULK_CHUNKS_PER_PASS; ++chunks) {
                    const uint32_t size = std::min<uint32_t>(CHUNK, transfer.size - transfer.offset);
                    bool read_failed = false;
                    if (transfer.buffered != transfer.offset) {
                        read_failed = lseek(transfer.fd, transfer.offset, SEEK_SET) < 0 || read(transfer.fd, packet + 16, size) != ssize_t(size);
                    }
                    if (read_failed) {
                        cJSON *error = cJSON_CreateObject();
                        cJSON_AddStringToObject(error, "type", "download");
                        cJSON_AddNumberToObject(error, "id", transfer.id);
                        cJSON_AddStringToObject(error, "error", "SD card read failed");
                        send_text(error); stop_transfer();
                        break;
                    }
                    transfer.buffered = transfer.offset;
                    // Four LE uint32: magic "FILE", transfer id, total bytes, byte offset.
                    const uint32_t header[] = {0x454c4946u, transfer.id, transfer.size, transfer.offset};
                    memcpy(packet, header, sizeof(header));
                    esp_peer_data_frame_t chunk{};
                    chunk.type = ESP_PEER_DATA_CHANNEL_DATA; chunk.stream_id = stream_id;
                    chunk.data = packet; chunk.size = size + 16;
                    int ret = esp_peer_send_data(peer, &chunk);
                    if (ret != 0) {
                        if (ret == ESP_PEER_ERR_WOULD_BLOCK || ret == ESP_PEER_ERR_NO_MEM) ++full_cache;
                        else failed = true;
                        break;
                    }
                    activity = esp_timer_get_time();
                    transfer.offset += size;
                    if (transfer.offset == transfer.size) stop_transfer();
                    // A full Wi-Fi TX queue ends the burst so the worker services
                    // acknowledgements and controls before sending more.
                    if (queue_filled()) break;
                }
            }
            const bool manual_due = capture_ready && recording && recorder_manual_frame_due();
            // A slow viewer may still hold its frame. Manual recording can
            // use the other capture buffer without waiting for that send.
            // Only a frame that is already waiting: recording has no rate cap,
            // and waiting here for each new frame would stall the stream.
            if (fb && manual_due && camera_frame_ready()) {
                if (camera_fb_t *record_frame = get_frame(streaming)) {
                    recorder_manual_feed(record_frame);
                    camera_frame_return(record_frame);
                }
            }
            // Likewise for motion analysis and triggered recording: a large
            // frame can take a slow link several hundred ms to send, which would
            // otherwise cap both at the stream's frame rate.
            if (fb && capture_ready && motion_enabled && motion_frame_due() && camera_frame_ready()) {
                if (camera_fb_t *motion_frame = get_frame(streaming)) {
                    motion_feed(motion_frame);
                    camera_frame_return(motion_frame);
                }
            }
            const bool capture_requested = capture_ready && (streaming || manual_due || (motion_enabled && motion_frame_due()));
            // With no stream, detection or recording, the driver still captures
            // and overwrites its buffers. Take and return each ready frame,
            // never waiting, so the viewer's FPS still shows the sensor's rate.
            // Raw sensors are skipped: their frames belong to the JPEG encoder.
            if (viewer && capture_ready && !fb && !images_enabled && !motion_enabled && !recording &&
                esp_camera_available_frames()) {
                if (const sensor_t *sensor = esp_camera_sensor_get(); sensor && sensor->pixformat == PIXFORMAT_JPEG) {
                    if (camera_fb_t *frame = esp_camera_fb_get()) {
                        ++grabbed;
                        esp_camera_fb_return(frame);
                    }
                }
            }
            // Intentional pauses and held frames are not evidence of a stalled
            // sensor. In particular, file transfers temporarily suspend video.
            if (!capture_requested || fb) capture_stalled_since = 0;
            if (!fb && capture_requested) {
                const int64_t capture_started = esp_timer_get_time();
                fb = get_frame(streaming);
                if (streaming) timing.capture_us += esp_timer_get_time() - capture_started;
                if (streaming) { offset = 0; ++sequence; last_progress = esp_timer_get_time(); }
                if (fb) {
                    capture_stalled_since = 0;
                    frame_ok();
                    if (recording) recorder_manual_feed(fb);
                    else if (manual == ManualRecording::idle) motion_feed(fb);
                }
                else if (!capture_stalled_since) capture_stalled_since = esp_timer_get_time();
                else if (esp_timer_get_time() - capture_stalled_since >= 8000000) {
                    // The OV2640 capture pipeline can stall across control
                    // changes/reconnects. Recover with no held buffer and
                    // retain the connection and all current user settings.
                    diagnostic_event("warning", "Restarting stalled camera capture");
                    ESP_LOGW(TAG, "Restarting stalled camera capture");
                    // Cut sensor power on the first attempt too: a stalled
                    // OV2640 usually has a hung SCCB bus, and without power
                    // the driver probes it for ~30 s, blocking the worker.
                    const esp_err_t recovered = camera_recover(true);
                    capture_stalled_since = 0;
                    if (recovered == ESP_OK) {
                        diagnostic_event("info", "Camera capture recovery succeeded");
                        if (viewer) send_text(camera_settings());
                    } else {
                        // A failed probe may free the driver and invalidate the
                        // sensor pointer; capture and sensor commands stay off
                        // until a scheduled recovery succeeds.
                        camera_failed(recovered, "Camera capture recovery");
                    }
                }
                if (fb && !streaming) { camera_frame_return(fb); fb = nullptr; }
            }
            if (streaming) {
                // Bounded bursts: continuous capture must not saturate Wi-Fi
                // and leave DTLS unable to send acknowledgements or controls.
                for (int chunks = 0; fb && chunks < BULK_CHUNKS_PER_PASS; ++chunks) {
                    size_t size = std::min(CHUNK, fb->len - offset);
                    // Four LE uint32: magic, sequence, total bytes, byte offset.
                    const uint32_t header[] = {0x4745504a, sequence, uint32_t(fb->len), uint32_t(offset)};
                    memcpy(packet, header, sizeof(header)); memcpy(packet + 16, fb->buf + offset, size);
                    esp_peer_data_frame_t frame{};
                    frame.type = ESP_PEER_DATA_CHANNEL_DATA; frame.stream_id = stream_id;
                    frame.data = packet; frame.size = size + 16;
                    const int64_t send_started = esp_timer_get_time();
                    int ret = esp_peer_send_data(peer, &frame);
                    timing.send_us += esp_timer_get_time() - send_started;
                    if (ret == 0) timing.bytes += size;
                    if (ret == 0 && offset + size == fb->len) ++timing.frames;
                    if (ret == 0) {
                        offset += size; last_progress = esp_timer_get_time();
                    } else if (ret == ESP_PEER_ERR_WOULD_BLOCK || ret == ESP_PEER_ERR_NO_MEM) {
                        ++full_cache;
                    }
                    if (offset == fb->len || esp_timer_get_time() - last_progress > 2000000 || (ret != 0 && ret != ESP_PEER_ERR_WOULD_BLOCK && ret != ESP_PEER_ERR_NO_MEM)) {
                        camera_frame_return(fb); fb = nullptr;
                    }
                    // A full Wi-Fi TX queue ends the burst, as for downloads above.
                    if (ret != 0 || queue_filled()) break;
                }
            }
            if (!viewer && !motion_enabled && !recording) capture_stalled_since = 0;
        }
        if (const int64_t now = esp_timer_get_time(); now - grab_window >= 1000000) {
            grab_fps = float(grabbed) * 1e6f / float(now - grab_window);
            grabbed = 0;
            grab_window = now;
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
