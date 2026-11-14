#include "common/application.h"
#include "provisioning.h"
#include "tls_identity.h"
#include "camera_controls.h"
#include "remote_signaling.h"
#include "remote_auth.h"
#include "local_candidates.h"
#include "lwip/sockets.h"
#include "esp_camera.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "media_lib_adapter.h"
#include "esp_https_server.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <string>

namespace {
constexpr char TAG[] = "local_webrtc";
constexpr size_t CHUNK = 4096;
SemaphoreHandle_t mutex;
esp_peer_handle_t peer;
httpd_handle_t server;
std::string answer, pending_reply, remote_offer;
RemoteAuth remote_auth;
bool channel_ready = false, failed = false, remote_peer = false;
uint16_t stream_id;
int64_t activity = 0;
extern const unsigned char page_start[] asm("_binary_viewer_html_start");
extern const unsigned char page_end[] asm("_binary_viewer_html_end");

struct Lock {
    Lock() { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(mutex); }
};
int send_text(cJSON *json) {
    if (!json) return -1;
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return -1;
    pending_reply.assign(text);
    cJSON_free(text);
    return 0;
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
    stream_id = info->stream_id;
    channel_ready = true;
    activity = esp_timer_get_time();
    if (remote_peer) send_text(remote_auth.begin(remote_offer, answer));
    return 0;
}
int on_data(esp_peer_data_frame_t *frame, void *) {
    if (!channel_ready || frame->type != ESP_PEER_DATA_CHANNEL_STRING || frame->size <= 0 || frame->size > 1024) return 0;
    // Parse exactly one JSON document, never assuming network data is terminated.
    std::string text(reinterpret_cast<char *>(frame->data), frame->size);
    const char *end = nullptr;
    cJSON *request = cJSON_ParseWithOpts(text.c_str(), &end, true);
    if (!request) return 0;
    if (remote_peer && !remote_auth.allowed()) {
        send_text(remote_auth.verify(request));
        cJSON_Delete(request); return 0;
    }
    activity = esp_timer_get_time();
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    if (!(cJSON_IsString(op) && !strcmp(op->valuestring, "ping"))) send_text(camera_command(request));
    cJSON_Delete(request);
    return 0;
}
void close_peer() {
    channel_ready = false;
    if (peer) { esp_peer_close(peer); peer = nullptr; }
    answer.clear(); pending_reply.clear(); remote_offer.clear(); remote_auth.clear(); failed = false; remote_peer = false;
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
    char address[20], host[48], origin[64];
    snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.ip));
    if (httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) != ESP_OK) return false;
    if (host != std::string(address) && host != std::string(address) + ":443") return false;
    if (httpd_req_get_hdr_value_len(r, "Origin")) {
        if (httpd_req_get_hdr_value_str(r, "Origin", origin, sizeof(origin)) != ESP_OK || origin != std::string("https://") + host) return false;
    }
    return true;
}
esp_err_t page(httpd_req_t *r) {
    if (!same_origin(r)) return respond(r, "403 Forbidden", "{}");
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(r, "Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src blob:; frame-ancestors 'none'; base-uri 'none'");
    return httpd_resp_send(r, reinterpret_cast<const char *>(page_start), page_end - page_start - 1);
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
    if (peer) return respond(r, "409 Conflict", "{\"error\":\"One browser at a time; disconnect or wait 30 seconds\"}");
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
    esp_peer_default_cfg_t extra{};
    extra.agent_recv_timeout = 10;
    extra.data_ch_cfg.send_cache_size = 32768;
    extra.data_ch_cfg.recv_cache_size = 8192;
    extra.max_candidates = 4;
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
        if (event.type == 3) { if (remote_peer) close_peer(); continue; }
        if (event.type == 1 && !remote_peer) {
            if (peer) { remote_send(4, "busy"); continue; }
            if (!RemoteAuth::configured()) { remote_send(4, "password_not_configured"); continue; }
            if (RemoteAuth::blocked()) { remote_send(4, "authentication_rate_limited"); continue; }
            remote_offer = event.data;
            esp_peer_default_cfg_t extra{};
            extra.agent_recv_timeout = 500; extra.data_ch_cfg.send_cache_size = 32768;
            extra.data_ch_cfg.recv_cache_size = 8192; extra.max_candidates = 8;
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
    cfg.httpd.stack_size = 12288;
    cfg.httpd.max_open_sockets = 2;
    cfg.httpd.lru_purge_enable = true;
    cfg.httpd.recv_wait_timeout = 5;
    cfg.httpd.send_wait_timeout = 5;
    cfg.servercert = reinterpret_cast<const uint8_t *>(tls_identity_cert());
    cfg.servercert_len = strlen(tls_identity_cert()) + 1;
    cfg.prvtkey_pem = reinterpret_cast<const uint8_t *>(tls_identity_key());
    cfg.prvtkey_len = strlen(tls_identity_key()) + 1;
    ESP_ERROR_CHECK(httpd_ssl_start(&server, &cfg));
    httpd_uri_t route{};
    route.uri = "/"; route.method = HTTP_GET; route.handler = page;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
    route.uri = "/api/session"; route.handler = session;
    for (auto method : {HTTP_GET, HTTP_POST, HTTP_DELETE}) {
        route.method = method; ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
    }
    esp_netif_ip_info_t ip{};
    ESP_ERROR_CHECK(esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip));
    ESP_LOGI(TAG, "Local camera ready: https://" IPSTR "/", IP2STR(&ip.ip));
}
void worker(void *) {
    bool camera_ready = false;
    camera_fb_t *fb = nullptr;
    size_t offset = 0;
    uint32_t sequence = 0;
    unsigned capture_timeouts = 0;
    int64_t frame_start = 0, next_frame = 0;
    uint8_t packet[CHUNK + 16];
    for (;;) {
        if (!provisioning_is_ready()) {
            { Lock guard; if (fb) { esp_camera_fb_return(fb); fb = nullptr; } close_peer(); }
            if (server) { ESP_ERROR_CHECK(httpd_ssl_stop(server)); server = nullptr; }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!camera_ready) { ESP_ERROR_CHECK(camera_start()); camera_ready = true; }
        if (!server) start_server();
        {
            Lock guard;
            remote_events();
            const int64_t now = esp_timer_get_time();
            if (peer && (failed || (remote_peer && remote_auth.expired(now)) || now - activity > 30000000)) {
                if (fb) { esp_camera_fb_return(fb); fb = nullptr; }
                close_peer();
            }
            if (peer) esp_peer_main_loop(peer);
            bool reply_waiting = false;
            if (channel_ready && !pending_reply.empty()) {
                esp_peer_data_frame_t reply{};
                reply.type = ESP_PEER_DATA_CHANNEL_STRING; reply.stream_id = stream_id;
                reply.data = reinterpret_cast<uint8_t *>(&pending_reply[0]); reply.size = pending_reply.size();
                int ret = esp_peer_send_data(peer, &reply);
                if (ret == 0) pending_reply.clear();
                else if (ret != ESP_PEER_ERR_WOULD_BLOCK) failed = true;
                reply_waiting = !pending_reply.empty();
            }
            if (channel_ready && !reply_waiting && (!remote_peer || remote_auth.allowed())) {
                if (!fb && now >= next_frame) {
                    fb = esp_camera_fb_get(); offset = 0; ++sequence; frame_start = now;
                    next_frame = now + 200000;
                    if (fb) capture_timeouts = 0;
                    else if (++capture_timeouts >= 2) {
                        // The OV2640 capture pipeline can stall across control
                        // changes/reconnects. Recover with no held buffer and
                        // retain the connection and all current user settings.
                        ESP_LOGW(TAG, "Restarting stalled camera capture");
                        ESP_ERROR_CHECK(camera_recover());
                        capture_timeouts = 0;
                        send_text(camera_settings());
                    }
                }
                if (fb) {
                    size_t size = std::min(CHUNK, fb->len - offset);
                    // Four LE uint32: magic, sequence, total bytes, byte offset.
                    const uint32_t header[] = {0x4745504a, sequence, uint32_t(fb->len), uint32_t(offset)};
                    memcpy(packet, header, sizeof(header)); memcpy(packet + 16, fb->buf + offset, size);
                    esp_peer_data_frame_t frame{};
                    frame.type = ESP_PEER_DATA_CHANNEL_DATA; frame.stream_id = stream_id;
                    frame.data = packet; frame.size = size + 16;
                    int ret = esp_peer_send_data(peer, &frame);
                    if (ret == 0) offset += size;
                    if (offset == fb->len || now - frame_start > 2000000 || (ret != 0 && ret != ESP_PEER_ERR_WOULD_BLOCK)) {
                        esp_camera_fb_return(fb); fb = nullptr;
                    }
                }
            } else if (!channel_ready && fb) { esp_camera_fb_return(fb); fb = nullptr; }
        }
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
    if (xTaskCreate(worker, "local_peer", 24576, nullptr, 5, nullptr) != pdPASS) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    for (;;) { app.main(); vTaskDelay(pdMS_TO_TICKS(20)); }
}
