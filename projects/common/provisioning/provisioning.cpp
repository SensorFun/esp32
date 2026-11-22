#include "provisioning.h"
#include "esp_app_desc.h"
#if CONFIG_PROVISIONING_SECURE
#include "tls_identity.h"
#endif
#include "credentials.h"
#if CONFIG_CAMERA_REMOTE_ACCESS
#include "camera_access.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

// Wi-Fi is provisioned only over the physical USB console. The board never
// opens a setup access point: anyone in radio range could otherwise move it
// to a network of their choosing.
namespace {
using provisioning::Credentials;
struct SetupRequest { Credentials wifi; char camera_password[97]{}; bool usb = false; };
constexpr char TAG[] = "provisioning";
constexpr char NVS_NAMESPACE[] = "wifi_setup";
constexpr char NVS_KEY[] = "credentials";
constexpr EventBits_t CONNECTED = BIT0, DISCONNECTED = BIT1;
constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;
// Pause between attempts to rejoin an unavailable saved network. Scans need
// the station idle, so USB setup can list networks during the pause.
constexpr uint32_t RETRY_PAUSE_MS = 15000;
enum class State { starting, connecting, provisioning, connected, storage_error };
std::atomic<State> state{State::starting};
std::atomic<bool> submitting{false};
std::atomic<const char *> usb_result{"idle"};
EventGroupHandle_t events = nullptr;
QueueHandle_t requests = nullptr;
esp_netif_t *station_netif = nullptr;

template<typename T> void wipe(T &credentials) {
    volatile unsigned char *bytes = reinterpret_cast<volatile unsigned char *>(&credentials);
    for (size_t i = 0; i < sizeof(credentials); ++i) bytes[i] = 0;
}

#if CONFIG_CAMERA_REMOTE_ACCESS
bool camera_password_saved() {
    camera_access::Record record;
    const bool saved = camera_access::load(record) == ESP_OK;
    wipe(record);
    return saved;
}
#endif

const char *camera_password_fields() {
#if CONFIG_CAMERA_REMOTE_ACCESS
    if (camera_password_saved()) return ",\"camera_password_required\":false,\"camera_password_saved\":true";
    return ",\"camera_password_required\":true,\"camera_password_saved\":false";
#else
    return ",\"camera_password_required\":false,\"camera_password_saved\":false";
#endif
}

esp_err_t load_credentials(Credentials &credentials) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t size = sizeof(credentials);
    err = nvs_get_blob(handle, NVS_KEY, &credentials, &size);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    if (size != sizeof(credentials) || credentials.ssid[32] != '\0' ||
        credentials.password[64] != '\0' ||
        !provisioning::valid_credentials(credentials.ssid, credentials.password)) {
        wipe(credentials);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t save_credentials(const Credentials &credentials) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, NVS_KEY, &credentials, sizeof(credentials));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

void wifi_event(void *, esp_event_base_t base, int32_t id, void *) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(events, CONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(events, CONNECTED);
        xEventGroupSetBits(events, DISCONNECTED);
    }
}

#if CONFIG_CAMERA_REMOTE_ACCESS
// The remote viewer address for this camera, e.g. <viewer>/A1B2C3D4E5F6.
std::string remote_url() {
    char camera[13]{};
    if (camera_access::mac_room(camera, sizeof(camera)) != ESP_OK) return "";
    return std::string(CONFIG_CAMERA_REMOTE_VIEWER_URL) + camera;
}
#endif

// Lists nearby networks as a JSON array for USB setup.
esp_err_t scan_networks(std::string &json) {
    wifi_scan_config_t config{};
    config.show_hidden = true;
    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK) return err;
    uint16_t count = 0;
    err = esp_wifi_scan_get_ap_num(&count);
    std::vector<std::string> names;
    for (uint16_t i = 0; err == ESP_OK && i < count; ++i) {
        wifi_ap_record_t record{};
        err = esp_wifi_scan_get_ap_record(&record);
        if (err != ESP_OK) break;
        const char *ssid = reinterpret_cast<const char *>(record.ssid);
        std::string name(ssid, strnlen(ssid, 32));
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    esp_wifi_clear_ap_list();
    if (err != ESP_OK) return err;
    json = "[";
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) json += ',';
        json += provisioning::json_quote(names[i]);
    }
    json += ']';
    return ESP_OK;
}

void apply_credentials(const Credentials &credentials) {
    esp_wifi_scan_stop();
    const bool connected = xEventGroupGetBits(events) & CONNECTED;
    state = State::connecting;
    xEventGroupClearBits(events, CONNECTED | DISCONNECTED);
    if (esp_wifi_disconnect() == ESP_OK && connected) {
        xEventGroupWaitBits(events, DISCONNECTED, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));
    }
    wifi_config_t config{};
    std::memcpy(config.sta.ssid, credentials.ssid, std::strlen(credentials.ssid));
    std::memcpy(config.sta.password, credentials.password, std::strlen(credentials.password));
    config.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    std::memset(config.sta.password, 0, sizeof(config.sta.password));
    xEventGroupClearBits(events, CONNECTED | DISCONNECTED);
    esp_wifi_connect();
}

bool due(TickType_t now, TickType_t at) { return static_cast<int32_t>(now - at) >= 0; }

void supervisor(void *) {
    Credentials pending{};
    char pending_camera_password[97]{};
    bool save_pending = false;
    bool usb_pending = false, restoring = false, retry_pending = false;
    TickType_t started = xTaskGetTickCount(), last_attempt = started, retry_at = 0;
    if (load_credentials(pending) == ESP_OK) {
        apply_credentials(pending);
        wipe(pending);
    } else {
        state = State::provisioning;
        ESP_LOGI(TAG, "No saved Wi-Fi; waiting for USB provisioning");
    }
    for (;;) {
        SetupRequest next{};
        if (xQueueReceive(requests, &next, pdMS_TO_TICKS(100)) == pdTRUE) {
            pending = next.wifi;
            memcpy(pending_camera_password, next.camera_password, sizeof(pending_camera_password));
            usb_pending = next.usb;
            restoring = retry_pending = false;
            wipe(next);
            save_pending = true;
            apply_credentials(pending);
            started = last_attempt = xTaskGetTickCount();
            if (!usb_pending) submitting = false;
        }
        const TickType_t now = xTaskGetTickCount();
        const State current = state.load();
        const EventBits_t bits = xEventGroupGetBits(events);
        if (current == State::connecting) {
            if (bits & CONNECTED) {
                esp_err_t saved = ESP_OK;
#if CONFIG_CAMERA_REMOTE_ACCESS
                if (save_pending && pending_camera_password[0]) saved = camera_access::set_password(pending_camera_password);
#endif
                if (saved == ESP_OK && save_pending) saved = save_credentials(pending);
                if (saved != ESP_OK) {
                    ESP_LOGE(TAG, "Could not persist Wi-Fi settings");
                    state = State::storage_error;
                } else {
                    state = State::connected;
                    ESP_LOGI(TAG, "Wi-Fi connected; settings ready");
                }
                if (usb_pending) {
                    usb_result = saved != ESP_OK ? "storage_error" : restoring ? "restored" : "saved";
                    restoring = usb_pending = false;
                    submitting = false;
                }
                save_pending = false;
                wipe(pending);
                wipe(pending_camera_password);
            } else if (now - started >= pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) {
                esp_wifi_disconnect();
                save_pending = false;
                wipe(pending);
                wipe(pending_camera_password);
                // The saved network is untouched until a new network gets an IP.
                if (usb_pending && !restoring && load_credentials(pending) == ESP_OK) {
                    restoring = true;
                    apply_credentials(pending);
                    wipe(pending);
                    started = last_attempt = now;
                    continue;
                }
                if (usb_pending) {
                    usb_result = "failed";
                    restoring = usb_pending = false;
                    submitting = false;
                }
                state = State::provisioning;
                // A saved network may only be down for now (router restart,
                // out of range). Keep rejoining it rather than opening setup.
                retry_pending = load_credentials(pending) == ESP_OK;
                wipe(pending);
                if (retry_pending) {
                    retry_at = now + pdMS_TO_TICKS(RETRY_PAUSE_MS);
                    ESP_LOGW(TAG, "Saved Wi-Fi unavailable; retrying in %u s", unsigned(RETRY_PAUSE_MS / 1000));
                } else {
                    ESP_LOGW(TAG, "Wi-Fi connection unavailable; waiting for USB provisioning");
                }
            } else if ((bits & DISCONNECTED) && now - last_attempt >= pdMS_TO_TICKS(1000)) {
                xEventGroupClearBits(events, DISCONNECTED);
                esp_wifi_connect();
                last_attempt = now;
            }
        } else if (current == State::provisioning) {
            // A USB scan in progress needs the station idle; retry after it.
            if (retry_pending && due(now, retry_at) && !submitting.load()) {
                retry_pending = false;
                if (load_credentials(pending) == ESP_OK) {
                    apply_credentials(pending);
                    started = last_attempt = now;
                }
                wipe(pending);
            }
        } else if (current == State::connected && !(bits & CONNECTED)) {
            state = State::connecting;
            started = last_attempt = now;
            esp_wifi_connect();
        }
    }
}

} // namespace

extern "C" esp_err_t provisioning_start(void) {
    if (events) return ESP_ERR_INVALID_STATE;
    // Do not erase unrelated application data if NVS initialization fails.
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err;
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Network initialization failed");
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    station_netif = esp_netif_create_default_wifi_sta();
    if (!station_netif) return ESP_ERR_NO_MEM;
    events = xEventGroupCreate();
    requests = xQueueCreate(1, sizeof(SetupRequest));
    if (!events || !requests) return ESP_ERR_NO_MEM;
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi), TAG, "Wi-Fi initialization failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, nullptr), TAG, "Event registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr), TAG, "Event registration failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
#if CONFIG_PROVISIONING_SECURE
    ESP_RETURN_ON_ERROR(tls_identity_init(), TAG, "TLS identity failed");
#endif
    if (xTaskCreate(supervisor, "wifi_setup", 6144, nullptr, 4, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

extern "C" bool provisioning_is_ready(void) {
    return events && state.load() == State::connected && (xEventGroupGetBits(events) & CONNECTED);
}

extern "C" esp_err_t provisioning_submit_credentials_with_camera(const char *ssid, const char *password, const char *camera_password) {
    if (!requests || state.load() != State::provisioning) return ESP_ERR_INVALID_STATE;
    if (!ssid || !password || !provisioning::valid_credentials(ssid, password)) return ESP_ERR_INVALID_ARG;
#if CONFIG_CAMERA_REMOTE_ACCESS
    if (!camera_access::valid_password(camera_password)) return ESP_ERR_INVALID_ARG;
#endif
    SetupRequest setup{};
    memcpy(setup.wifi.ssid, ssid, strlen(ssid));
    memcpy(setup.wifi.password, password, strlen(password));
    if (camera_password) {
        if (strlen(camera_password) > 96) return ESP_ERR_INVALID_ARG;
        memcpy(setup.camera_password, camera_password, strlen(camera_password));
    }
    const bool acquired = !submitting.exchange(true);
    const bool queued = acquired && xQueueSend(requests, &setup, 0) == pdTRUE;
    if (acquired && !queued) submitting = false;
    wipe(setup);
    return queued ? ESP_OK : ESP_ERR_INVALID_STATE;
}
extern "C" esp_err_t provisioning_submit_credentials(const char *ssid, const char *password) {
    return provisioning_submit_credentials_with_camera(ssid, password, nullptr);
}

extern "C" void provisioning_usb_command(const char *command) {
    const std::string_view input(command);
    auto reply = [](const char *result) {
        std::printf("SENSORFUN_USB {\"result\":\"%s\"}\n", result);
    };
    if (input == "status") {
        // `project` is the firmware's CMake project name, so the flasher can
        // check the board runs the project it is provisioning for.
        std::string body = "SENSORFUN_USB {\"status\":true,\"project\":" +
            provisioning::json_quote(esp_app_get_description()->project_name) +
            ",\"result\":\"" + usb_result.load() + "\"";
        esp_netif_ip_info_t ip{};
        if (station_netif && provisioning_is_ready() &&
            esp_netif_get_ip_info(station_netif, &ip) == ESP_OK && ip.ip.addr) {
            char address[16]{};
            std::snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.ip));
            body += ",\"ip\":\"" + std::string(address) + "\"";
#if CONFIG_CAMERA_REMOTE_ACCESS
            if (const std::string remote = remote_url(); !remote.empty()) body += ",\"remote_url\":" + provisioning::json_quote(remote);
#endif
        }
        body += "}\n";
        std::fputs(body.c_str(), stdout);
        return;
    }
    if (!requests || state.load() == State::starting) { reply("busy"); return; }
    if (submitting.exchange(true)) { reply("busy"); return; }
    if (input == "scan") {
        // A camera password is only required while none is saved, so
        // re-running setup keeps the current one.
        std::string networks;
        const esp_err_t err = state.load() == State::connecting ? ESP_ERR_INVALID_STATE : scan_networks(networks);
        submitting = false;
        if (err != ESP_OK) { reply(err == ESP_ERR_INVALID_STATE ? "busy" : "scan_failed"); return; }
        std::printf("SENSORFUN_USB {\"result\":\"scan\",\"ssids\":%s%s}\n",
                    networks.c_str(), camera_password_fields());
        return;
    }
    const char *result = "invalid";
    // Accepted while the board is rejoining its saved network, too: the new
    // network replaces that attempt, and the saved one is restored on failure.
    if (input.starts_with("wifi ")) {
        SetupRequest setup{};
        std::string camera_password;
#if CONFIG_CAMERA_REMOTE_ACCESS
        const bool valid = provisioning::parse_credentials(input.substr(5), setup.wifi, &camera_password, !camera_password_saved());
#else
        const bool valid = provisioning::parse_credentials(input.substr(5), setup.wifi);
#endif
        if (valid) {
            memcpy(setup.camera_password, camera_password.data(), camera_password.size());
            setup.usb = true;
            usb_result = "pending";
            if (xQueueSend(requests, &setup, 0) == pdTRUE) result = "pending";
            else { usb_result = "idle"; result = "busy"; }
        }
        wipe(setup);
        if (!camera_password.empty()) {
            volatile char *secret = camera_password.data();
            for (size_t i = 0; i < camera_password.size(); ++i) secret[i] = 0;
        }
    }
#if CONFIG_CAMERA_REMOTE_ACCESS
    else if (input.starts_with("password ")) {
        std::string password;
        if (provisioning::form_decode(input.substr(9), password) && camera_access::valid_password(password.c_str())) {
            result = camera_access::set_password(password.c_str()) == ESP_OK ? "saved" : "storage_error";
        }
        camera_access::wipe(password.data(), password.size());
    }
#endif
    if (std::strcmp(result, "pending") != 0) submitting = false;
    reply(result);
}
