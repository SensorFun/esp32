#include "camera_signaling.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <atomic>
#include <cstring>
#include <string>

namespace {
constexpr size_t max_message = 32768;
struct Client {
    esp_peer_signaling_cfg_t cfg{};
    esp_websocket_client_handle_t ws = nullptr;
    SemaphoreHandle_t mutex = nullptr, finished = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<bool> stopping{false}, failed{false}, joined{false};
    std::string room, secret, stun, incoming;
    size_t expected = 0;
};
const char *string(cJSON *object, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}
void fail(Client *client) {
    if (!client->stopping && !client->failed.exchange(true)) {
        client->joined = false;
        if (client->cfg.on_close) client->cfg.on_close(client->cfg.ctx);
    }
}
bool send(Client *client, cJSON *body) {
    xSemaphoreTake(client->mutex, portMAX_DELAY);
    bool ok = false;
    if (!client->stopping && !client->failed) {
        char *payload = cJSON_PrintUnformatted(body);
        if (payload) {
            const size_t length = strlen(payload);
            ok = esp_websocket_client_send_text(client->ws, payload, length, pdMS_TO_TICKS(2000)) == static_cast<int>(length);
            cJSON_free(payload);
        }
    }
    xSemaphoreGive(client->mutex);
    cJSON_Delete(body);
    return ok;
}
void receive(Client *client, cJSON *root) {
    const char *type = string(root, "type");
    if (!strcmp(type, "error")) { fail(client); return; }
    if (!strcmp(type, "joined")) {
        if (client->joined.exchange(true)) return;
        esp_peer_signaling_ice_info_t ice{};
        ice.server_info.stun_url = const_cast<char *>(client->stun.c_str());
        ice.is_initiator = false;
        if (client->cfg.on_ice_info) client->cfg.on_ice_info(&ice, client->cfg.ctx);
        if (client->cfg.on_connected) client->cfg.on_connected(client->cfg.ctx);
        return;
    }
    if (!client->joined) return;
    esp_peer_signaling_msg_t incoming{};
    const char *text = nullptr;
    if (!strcmp(type, "offer")) { incoming.type = ESP_PEER_SIGNALING_MSG_SDP; text = string(root, "sdp"); }
    else if (!strcmp(type, "candidate")) { incoming.type = ESP_PEER_SIGNALING_MSG_CANDIDATE; text = string(root, "candidate"); }
    else if (!strcmp(type, "customized")) { incoming.type = ESP_PEER_SIGNALING_MSG_CUSTOMIZED; text = string(root, "data"); }
    else if (!strcmp(type, "bye")) { incoming.type = ESP_PEER_SIGNALING_MSG_BYE; text = ""; }
    else if (!strcmp(type, "signaling_lost")) { incoming.type = ESP_PEER_SIGNALING_MSG_TRANSPORT_LOST; text = ""; }
    if (text) {
        incoming.data = reinterpret_cast<uint8_t *>(const_cast<char *>(text)); incoming.size = strlen(text);
        if (client->cfg.on_msg) client->cfg.on_msg(&incoming, client->cfg.ctx);
    }
}
void websocket_event(void *context, esp_event_base_t, int32_t event, void *raw) {
    auto *client = static_cast<Client *>(context);
    if (client->stopping || client->failed) return;
    if (event == WEBSOCKET_EVENT_CONNECTED) {
        cJSON *join = cJSON_CreateObject();
        cJSON_AddStringToObject(join, "type", "join");
        cJSON_AddStringToObject(join, "room", client->room.c_str());
        cJSON_AddStringToObject(join, "secret", client->secret.c_str());
        cJSON_AddBoolToObject(join, "preserve_peer", true);
        if (!send(client, join)) fail(client);
    } else if (event == WEBSOCKET_EVENT_DISCONNECTED || event == WEBSOCKET_EVENT_ERROR) fail(client);
    else if (event == WEBSOCKET_EVENT_DATA) {
        auto *data = static_cast<esp_websocket_event_data_t *>(raw);
        if (data->op_code != 1 && data->op_code != 0) return;
        if (data->data_len < 0 || data->payload_len < 0 || data->payload_offset < 0 ||
            static_cast<size_t>(data->payload_len) > max_message) { fail(client); return; }
        if (data->payload_offset == 0 && data->op_code == 1) { client->incoming.clear(); client->expected = 0; }
        // esp_websocket_client chunks a frame by payload_offset. Continuation
        // frames may be delivered separately; concatenate until FIN is seen.
        if (static_cast<size_t>(data->payload_offset) != client->expected || client->incoming.size() + data->data_len > max_message) { fail(client); return; }
        client->incoming.append(data->data_ptr, data->data_len);
        client->expected += data->data_len;
        if (client->expected == static_cast<size_t>(data->payload_len)) {
            client->expected = 0;
            if (!data->fin) return;
            cJSON *root = cJSON_ParseWithLength(client->incoming.c_str(), client->incoming.size() + 1);
            if (!root) { fail(client); return; }
            receive(client, root); cJSON_Delete(root); client->incoming.clear();
        }
    }
}
void maintenance(void *context) {
    auto *client = static_cast<Client *>(context);
    unsigned seconds = 0;
    while (!client->stopping) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (client->stopping) break;
        ++seconds;
        if (!client->joined && seconds >= 20) fail(client);
    }
    xSemaphoreGive(client->finished);
    vTaskDelete(nullptr);
}
int start(esp_peer_signaling_cfg_t *cfg, esp_peer_signaling_handle_t *result) {
    if (!cfg || !result || !cfg->signal_url || strncmp(cfg->signal_url, "wss://", 6) != 0 || !cfg->extra_cfg || cfg->extra_size != sizeof(CameraSignalingConfig)) return ESP_PEER_ERR_INVALID_ARG;
    const auto *extra = static_cast<CameraSignalingConfig *>(cfg->extra_cfg);
    if (!extra->room || strlen(extra->room) != 32 || !extra->secret || !*extra->secret || !extra->stun || (strncmp(extra->stun, "stun:", 5) != 0 && strncmp(extra->stun, "stuns:", 6) != 0)) return ESP_PEER_ERR_INVALID_ARG;
    for (const char *p = extra->room; *p; ++p) if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F'))) return ESP_PEER_ERR_INVALID_ARG;
    auto *client = new Client;
    client->cfg = *cfg; client->room = extra->room; client->secret = extra->secret; client->stun = extra->stun;
    client->mutex = xSemaphoreCreateMutex(); client->finished = xSemaphoreCreateBinary();
    esp_websocket_client_config_t config{};
    config.uri = cfg->signal_url;
    config.crt_bundle_attach = esp_crt_bundle_attach; config.disable_auto_reconnect = true;
    config.buffer_size = 4096; config.task_stack = 8192; config.network_timeout_ms = 5000;
    if (client->mutex && client->finished) client->ws = esp_websocket_client_init(&config);
    if (!client->ws || esp_websocket_register_events(client->ws, WEBSOCKET_EVENT_ANY, websocket_event, client) != ESP_OK ||
        xTaskCreate(maintenance, "signal_alive", 4096, client, 3, &client->task) != pdPASS) {
        if (client->ws) esp_websocket_client_destroy(client->ws);
        if (client->mutex) vSemaphoreDelete(client->mutex);
        if (client->finished) vSemaphoreDelete(client->finished);
        delete client; return ESP_PEER_ERR_NO_MEM;
    }
    if (esp_websocket_client_start(client->ws) != ESP_OK) {
        client->stopping = true; xTaskNotifyGive(client->task); xSemaphoreTake(client->finished, portMAX_DELAY);
        esp_websocket_client_destroy(client->ws); vSemaphoreDelete(client->mutex); vSemaphoreDelete(client->finished); delete client; return ESP_PEER_ERR_INVALID_ARG;
    }
    *result = client; return 0;
}
int send_signal(esp_peer_signaling_handle_t handle, esp_peer_signaling_msg_t *signal) {
    auto *client = static_cast<Client *>(handle);
    if (!client || !signal || signal->size < 0 || signal->size > 16384 || (signal->size && !signal->data) || !client->joined) return ESP_PEER_ERR_INVALID_ARG;
    const std::string text(signal->data ? reinterpret_cast<const char *>(signal->data) : "", signal->size);
    cJSON *body = cJSON_CreateObject();
    if (signal->type == ESP_PEER_SIGNALING_MSG_SDP) { cJSON_AddStringToObject(body, "type", "answer"); cJSON_AddStringToObject(body, "sdp", text.c_str()); }
    else if (signal->type == ESP_PEER_SIGNALING_MSG_CANDIDATE) { cJSON_AddStringToObject(body, "type", "candidate"); cJSON_AddStringToObject(body, "candidate", text.c_str()); }
    else if (signal->type == ESP_PEER_SIGNALING_MSG_CUSTOMIZED) { cJSON_AddStringToObject(body, "type", "customized"); cJSON_AddStringToObject(body, "data", text.c_str()); }
    else if (signal->type == ESP_PEER_SIGNALING_MSG_BYE) cJSON_AddStringToObject(body, "type", "bye");
    else { cJSON_Delete(body); return ESP_PEER_ERR_INVALID_ARG; }
    return send(client, body) ? 0 : ESP_PEER_ERR_INVALID_ARG;
}
int stop(esp_peer_signaling_handle_t handle) {
    auto *client = static_cast<Client *>(handle);
    if (!client) return ESP_PEER_ERR_INVALID_ARG;
    // Closing the socket promptly tells the server to release this room
    // membership and notify the peer; no explicit app-level request needed.
    client->stopping = true;
    xTaskNotifyGive(client->task); xSemaphoreTake(client->finished, portMAX_DELAY);
    esp_websocket_client_stop(client->ws); esp_websocket_client_destroy(client->ws);
    vSemaphoreDelete(client->mutex); vSemaphoreDelete(client->finished); delete client;
    return 0;
}
const esp_peer_signaling_impl_t impl{start, send_signal, stop};
}
const esp_peer_signaling_impl_t *camera_signaling_impl() { return &impl; }
