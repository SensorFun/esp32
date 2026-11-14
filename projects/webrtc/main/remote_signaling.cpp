#include "remote_signaling.h"
#include "janus_signaling.h"
#include "esp_peer_signaling.h"
#include "camera_access.h"
#include "esp_log.h"
#include "provisioning.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <atomic>
#include <cstring>
#include "sdkconfig.h"

namespace {
QueueHandle_t incoming, outgoing;
std::atomic<bool> connected{false}, interrupted{false};
esp_peer_signaling_handle_t signaling;
char ice_url[256], ice_user[256], ice_password[256];
SemaphoreHandle_t ice_mutex;
void queue(int type, const char *data, size_t size) {
    if (size > 16384) return;
    auto *event = new RemoteEvent{type, std::string(data ? data : "", size)};
    if (xQueueSend(incoming, &event, 0) != pdTRUE) delete event;
}
int ice_info(esp_peer_signaling_ice_info_t *info, void *) {
    xSemaphoreTake(ice_mutex, portMAX_DELAY);
    snprintf(ice_url, sizeof(ice_url), "%s", info->server_info.stun_url ? info->server_info.stun_url : "");
    snprintf(ice_user, sizeof(ice_user), "%s", info->server_info.user ? info->server_info.user : "");
    snprintf(ice_password, sizeof(ice_password), "%s", info->server_info.psw ? info->server_info.psw : "");
    xSemaphoreGive(ice_mutex);
    return 0;
}
int on_connected(void *) { connected = true; ESP_LOGI("remote", "Private signaling connected"); return 0; }
int on_close(void *) { connected = false; interrupted = true; ESP_LOGW("remote", "Private signaling disconnected; rebuilding membership"); queue(3, "", 0); return 0; }
int on_message(esp_peer_signaling_msg_t *msg, void *) {
    ESP_LOGI("remote", "Received signal type %d (%d bytes)", int(msg->type), msg->size);
    queue(int(msg->type), reinterpret_cast<char *>(msg->data), msg->size);
    return 0;
}
void task(void *) {
    // Stable room identity uses the Wi-Fi station MAC, never the setup AP MAC.
    char camera[13]{};
    char room[33]{}, active_room[33]{};
    uint32_t active_revision = 0;
    ESP_ERROR_CHECK(camera_access::mac_room(camera, sizeof(camera)));
    ESP_LOGI("remote", "Remote camera ID: %s", camera);
    char url[257]{};
    for (;;) {
        if (!provisioning_is_ready()) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        const uint32_t revision = camera_access::generation();
        if (camera_access::signaling_room(room, sizeof(room)) != ESP_OK) {
            if (signaling) { connected = false; esp_peer_signaling_stop(signaling); signaling = nullptr; queue(3, "", 0); }
            vTaskDelay(pdMS_TO_TICKS(500)); continue;
        }
        if (signaling && (revision != active_revision || strcmp(room, active_room) != 0)) {
            connected = false; esp_peer_signaling_stop(signaling); signaling = nullptr; queue(3, "", 0);
            RemoteEvent *stale = nullptr;
            while (xQueueReceive(outgoing, &stale, 0) == pdTRUE) delete stale;
        }
        if (signaling && interrupted.exchange(false)) {
            connected = false; esp_peer_signaling_stop(signaling); signaling = nullptr;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (!signaling) {
            interrupted = false;
            snprintf(active_room, sizeof(active_room), "%s", room); active_revision = revision;
            if (provisioning_get_signaling_url(url, sizeof(url)) != ESP_OK) { ESP_LOGW("remote", "Invalid saved signaling server; open provisioning to select a server"); vTaskDelay(pdMS_TO_TICKS(10000)); continue; }
            esp_peer_signaling_cfg_t cfg{};
            cfg.signal_url = url; cfg.on_ice_info = ice_info;
            cfg.on_connected = on_connected; cfg.on_close = on_close; cfg.on_msg = on_message;
            JanusSignalingConfig janus{room, CONFIG_CAMERA_JANUS_API_SECRET, CONFIG_CAMERA_JANUS_STUN_URL};
            cfg.extra_cfg = &janus; cfg.extra_size = sizeof(janus);
            int ret = esp_peer_signaling_start(&cfg, janus_signaling_impl(), &signaling);
            if (ret) { ESP_LOGW("remote", "Signaling unavailable: %d; retry in 10 seconds", ret); vTaskDelay(pdMS_TO_TICKS(10000)); continue; }
        }
        RemoteEvent *event = nullptr;
        if (xQueueReceive(outgoing, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            esp_peer_signaling_msg_t msg{};
            msg.type = static_cast<esp_peer_signaling_msg_type_t>(event->type);
            msg.data = reinterpret_cast<uint8_t *>(&event->data[0]); msg.size = event->data.size();
            if (connected) {
                int ret = esp_peer_signaling_send_msg(signaling, &msg);
                ESP_LOGI("remote", "Sent signal type %d: %d", event->type, ret);
                if (ret) interrupted = true;
            }
            delete event;
        }
        if (!provisioning_is_ready()) { connected = false; esp_peer_signaling_stop(signaling); signaling = nullptr; }
    }
}
}
void remote_start() {
    incoming = xQueueCreate(12, sizeof(RemoteEvent *)); outgoing = xQueueCreate(8, sizeof(RemoteEvent *));
    ice_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(incoming && outgoing && ice_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    if (xTaskCreate(task, "remote_signal", 12288, nullptr, 4, nullptr) != pdPASS) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
}
bool remote_receive(RemoteEvent &event) {
    RemoteEvent *item = nullptr;
    if (xQueueReceive(incoming, &item, 0) != pdTRUE) return false;
    event = *item; delete item; return true;
}
void remote_send(int type, const char *data) {
    auto *event = new RemoteEvent{type, data ? data : ""};
    if (xQueueSend(outgoing, &event, 0) != pdTRUE) delete event;
}
bool remote_connected() { return connected; }
void remote_ice(esp_peer_ice_server_cfg_t &server) {
    // esp_peer_open copies the strings synchronously; keep refresh synchronized.
    static std::string url, user, password;
    xSemaphoreTake(ice_mutex, portMAX_DELAY);
    url = ice_url; user = ice_user; password = ice_password;
    xSemaphoreGive(ice_mutex);
    server.stun_url = url.data();
    server.user = user.empty() ? nullptr : user.data();
    server.psw = password.empty() ? nullptr : password.data();
}
