#include "provisioning.h"
#include "esp_app_desc.h"
#if CONFIG_PROVISIONING_SECURE
#include "esp_https_server.h"
#include "tls_identity.h"
#endif
#include "credentials.h"
#if CONFIG_CAMERA_REMOTE_ACCESS
#include "camera_access.h"
#endif
#include "local_http.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
using provisioning::Credentials;
struct SetupRequest { Credentials wifi; char camera_password[97]{}; bool usb = false; };
constexpr char TAG[] = "provisioning";
constexpr char NVS_NAMESPACE[] = "wifi_setup";
constexpr char NVS_KEY[] = "credentials";
constexpr EventBits_t CONNECTED = BIT0, DISCONNECTED = BIT1;
constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;
enum class State { starting, connecting, provisioning, connected, storage_error };
std::atomic<State> state{State::starting};
std::atomic<bool> submitting{false};
std::atomic<const char *> usb_result{"idle"};
std::atomic<TickType_t> handoff_at{0};
EventGroupHandle_t events = nullptr;
QueueHandle_t requests = nullptr;
esp_netif_t *station_netif = nullptr, *ap_netif = nullptr;
httpd_handle_t server = nullptr, local_http = nullptr;
char ap_address[16]{};
std::atomic<bool> ap_active{false};

extern const unsigned char html_start[] asm("_binary_index_html_start");
extern const unsigned char html_end[] asm("_binary_index_html_end");

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

void response_headers(httpd_req_t *request) {
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
}

esp_err_t json_response(httpd_req_t *request, const char *status, const char *body) {
    response_headers(request);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, body);
}

bool secure_request(httpd_req_t *request) {
#if CONFIG_PROVISIONING_SECURE
    return request->handle != local_http;
#else
    (void)request;
    return false;
#endif
}

bool setup_request(httpd_req_t *request) {
    esp_netif_ip_info_t ip{};
    if (esp_netif_get_ip_info(ap_netif, &ip) != ESP_OK) return false;
    return camera_http::allow_request(request, ip, secure_request(request));
}

esp_err_t page(httpd_req_t *request) {
    if (!setup_request(request)) return json_response(request, "403 Forbidden", "{\"error\":\"Use the setup Wi-Fi address\"}");
    response_headers(request);
    httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; form-action 'self'; frame-ancestors 'none'");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, reinterpret_cast<const char *>(html_start), html_end - html_start - 1);
}

#if CONFIG_CAMERA_REMOTE_ACCESS
// The remote viewer address for this camera, e.g. <viewer>/A1B2C3D4E5F6.
std::string remote_url() {
    char camera[13]{};
    if (camera_access::mac_room(camera, sizeof(camera)) != ESP_OK) return "";
    return std::string(CONFIG_CAMERA_REMOTE_VIEWER_URL) + camera;
}
#endif

// Lists nearby networks as a JSON array. The setup page and USB setup share it.
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

esp_err_t scan(httpd_req_t *request) {
    if (!setup_request(request)) return json_response(request, "403 Forbidden", "{\"error\":\"Use setup Wi-Fi\"}");
    if (state.load() != State::provisioning || submitting.load()) {
        return json_response(request, "409 Conflict", "{\"error\":\"Wait for the connection attempt to finish\"}");
    }
    std::string networks;
    const esp_err_t err = scan_networks(networks);
    if (err == ESP_ERR_WIFI_STATE || err == ESP_ERR_WIFI_TIMEOUT)
        return json_response(request, "503 Service Unavailable", "{\"error\":\"Scan unavailable; try again\"}");
    if (err != ESP_OK) return json_response(request, "503 Service Unavailable", "{\"error\":\"Scan failed\"}");
    const std::string body = "{\"ssids\":" + networks + camera_password_fields() + "}";
    return json_response(request, "200 OK", body.c_str());
}

esp_err_t status(httpd_req_t *request) {
    if (!setup_request(request)) return json_response(request, "403 Forbidden", "{\"error\":\"Use setup Wi-Fi\"}");
    const State current = state.load();
    if (current == State::storage_error) return json_response(request, "500 Internal Server Error", "{\"error\":\"Could not save Wi-Fi settings; try again\"}");
    if (current == State::connected) {
        esp_netif_ip_info_t ip{};
        if (esp_netif_get_ip_info(station_netif, &ip) != ESP_OK || !ip.ip.addr)
            return json_response(request, "200 OK", "{\"state\":\"connecting\"}");
        char address[16]{};
        std::snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.ip));
        std::string body = "{\"state\":\"connected\",\"ip\":" + provisioning::json_quote(address);
#if CONFIG_CAMERA_REMOTE_ACCESS
        body += ",\"local_url\":" + provisioning::json_quote(std::string(secure_request(request) ? "https://" : "http://") + address + "/");
        if (const std::string remote = remote_url(); !remote.empty()) body += ",\"remote_url\":" + provisioning::json_quote(remote);
#endif
        body += "}";
        return json_response(request, "200 OK", body.c_str());
    }
    const char *body =
        (current == State::connecting || submitting.load()) ? "{\"state\":\"connecting\"}" : "{\"state\":\"provisioning\"}";
    return json_response(request, "200 OK", body);
}

esp_err_t finish_setup(httpd_req_t *request) {
    if (!setup_request(request)) return json_response(request, "403 Forbidden", "{\"error\":\"Use setup Wi-Fi\"}");
    if (state.load() != State::connected || !(xEventGroupGetBits(events) & CONNECTED)) {
        return json_response(request, "409 Conflict", "{\"error\":\"Wait for Wi-Fi setup to finish\"}");
    }
    const auto result = json_response(request, "200 OK", "{}");
    if (result == ESP_OK) handoff_at = xTaskGetTickCount();
    return result;
}

esp_err_t connect_request(httpd_req_t *request) {
    if (!setup_request(request)) return json_response(request, "403 Forbidden", "{\"error\":\"Use setup Wi-Fi\"}");
    if (state.load() == State::connecting || submitting.exchange(true)) {
        return json_response(request, "409 Conflict", "{\"error\":\"Connection already in progress\"}");
    }
    char type[64]{};
    if (httpd_req_get_hdr_value_str(request, "Content-Type", type, sizeof(type)) != ESP_OK ||
        std::strcmp(type, "application/x-www-form-urlencoded") != 0 ||
        request->content_len == 0 || request->content_len > 1024) {
        submitting = false;
        return json_response(request, "400 Bad Request", "{\"error\":\"Invalid setup request\"}");
    }
    char body[1025]{};
    size_t received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count <= 0) {
            submitting = false;
            return json_response(request, "408 Request Timeout", "{\"error\":\"Setup request timed out\"}");
        }
        received += count;
    }
    SetupRequest setup{};
    std::string camera_password;
    const bool valid = provisioning::parse_credentials(std::string_view(body, received), setup.wifi,
#if CONFIG_CAMERA_REMOTE_ACCESS
        &camera_password, !camera_password_saved()
#else
        nullptr
#endif
    );
    wipe(body);
    if (!valid) {
        if (!camera_password.empty()) { volatile char *p = camera_password.data(); for (size_t i = 0; i < camera_password.size(); ++i) p[i] = 0; }
        submitting = false;
        return json_response(request, "400 Bad Request", "{\"error\":\"Check Wi-Fi credentials, camera password (12–96 bytes)\"}");
    }
    memcpy(setup.camera_password, camera_password.data(), camera_password.size());
    if (!camera_password.empty()) { volatile char *p = camera_password.data(); for (size_t i = 0; i < camera_password.size(); ++i) p[i] = 0; }
    const bool queued = xQueueSend(requests, &setup, 0) == pdTRUE;
    wipe(setup);
    if (!queued) {
        submitting = false;
        return json_response(request, "409 Conflict", "{\"error\":\"Setup is busy; try again\"}");
    }
    return json_response(request, "202 Accepted", "{\"state\":\"connecting\"}");
}

void start_setup() {
    if (ap_active) { state = State::provisioning; return; }
    uint8_t mac[6]{};
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    char name[32];
    std::snprintf(name, sizeof(name), "CAM_%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    wifi_config_t config{};
    std::memcpy(config.ap.ssid, name, std::strlen(name));
    config.ap.ssid_len = std::strlen(name);
    // Setup is an open access point. HTTPS encrypts credentials; local HTTP
    // is also available when explicitly chosen by the user.
    // The remote camera password is independent of joining this setup network.
    config.ap.authmode = WIFI_AUTH_OPEN;
    config.ap.max_connection = 2;
    config.ap.channel = 1;
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    esp_netif_ip_info_t ip{};
    ESP_ERROR_CHECK(esp_netif_get_ip_info(ap_netif, &ip));
    std::snprintf(ap_address, sizeof(ap_address), IPSTR, IP2STR(&ip.ip));
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 6144;
    http.recv_wait_timeout = 5;
    http.send_wait_timeout = 5;
    http.lru_purge_enable = true;
#if CONFIG_PROVISIONING_SECURE
    // ESP-IDF 6.1's initializer omits use_secure_element, which is zero initialized.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    httpd_ssl_config_t secure = HTTPD_SSL_CONFIG_DEFAULT();
#pragma GCC diagnostic pop
    secure.httpd = http;
    secure.httpd.server_port = 443;
    secure.httpd.max_open_sockets = 2;
    secure.servercert = (const uint8_t *)tls_identity_cert();
    secure.servercert_len = strlen(tls_identity_cert()) + 1;
    secure.prvtkey_pem = (const uint8_t *)tls_identity_key();
    secure.prvtkey_len = strlen(tls_identity_key()) + 1;
    ESP_ERROR_CHECK(httpd_ssl_start(&server, &secure));
    http.server_port = 80;
    http.ctrl_port = secure.httpd.ctrl_port + 1;
    http.max_open_sockets = 2;
    ESP_ERROR_CHECK(httpd_start(&local_http, &http));
#else
    ESP_ERROR_CHECK(httpd_start(&server, &http));
#endif
    const httpd_uri_t routes[] = {
        {.uri="/", .method=HTTP_GET, .handler=page, .user_ctx=nullptr},
        {.uri="/api/scan", .method=HTTP_GET, .handler=scan, .user_ctx=nullptr},
        {.uri="/api/status", .method=HTTP_GET, .handler=status, .user_ctx=nullptr},
        {.uri="/api/connect", .method=HTTP_POST, .handler=connect_request, .user_ctx=nullptr},
        {.uri="/api/finish", .method=HTTP_POST, .handler=finish_setup, .user_ctx=nullptr},
    };
    for (const auto &route : routes) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
        if (local_http) ESP_ERROR_CHECK(httpd_register_uri_handler(local_http, &route));
    }
    ap_active = true;
    state = State::provisioning;
#if CONFIG_PROVISIONING_SECURE
    ESP_LOGI(TAG, "Setup Wi-Fi: %s (no password) | https://%s | http://%s", name, ap_address, ap_address);
#else
    ESP_LOGI(TAG, "Setup Wi-Fi: %s (no password) | http://%s", name, ap_address);
#endif
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

void supervisor(void *) {
    Credentials pending{};
    char pending_camera_password[97]{};
    bool save_pending = false;
    bool usb_pending = false, restoring = false;
    TickType_t started = xTaskGetTickCount(), last_attempt = started, close_ap = 0;
    if (load_credentials(pending) == ESP_OK) {
        apply_credentials(pending);
        wipe(pending);
    } else start_setup();
    for (;;) {
        SetupRequest next{};
        if (xQueueReceive(requests, &next, pdMS_TO_TICKS(100)) == pdTRUE) {
            pending = next.wifi;
            memcpy(pending_camera_password, next.camera_password, sizeof(pending_camera_password));
            usb_pending = next.usb;
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
                    handoff_at = 0;
                    state = State::connected;
                    ESP_LOGI(TAG, "Wi-Fi connected; settings ready");
                    close_ap = xTaskGetTickCount();
                }
                if (usb_pending) {
                    if (saved == ESP_OK) handoff_at = xTaskGetTickCount();
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
                ESP_LOGW(TAG, "Wi-Fi connection unavailable; opening setup");
                start_setup();
                if (usb_pending) {
                    usb_result = "failed";
                    restoring = usb_pending = false;
                    submitting = false;
                }
            } else if ((bits & DISCONNECTED) && now - last_attempt >= pdMS_TO_TICKS(1000)) {
                xEventGroupClearBits(events, DISCONNECTED);
                esp_wifi_connect();
                last_attempt = now;
            }
        } else if (current == State::connected) {
            const auto handoff = handoff_at.load();
            if (!(bits & CONNECTED)) {
                state = State::connecting;
                started = last_attempt = now;
                esp_wifi_connect();
            } else if (ap_active && (now - close_ap >= pdMS_TO_TICKS(30000) ||
                       (handoff && xTaskGetTickCount() - handoff >= pdMS_TO_TICKS(750)))) {
#if CONFIG_PROVISIONING_SECURE
                ESP_ERROR_CHECK(httpd_ssl_stop(server));
#else
                ESP_ERROR_CHECK(httpd_stop(server));
#endif
                server = nullptr;
                if (local_http) { ESP_ERROR_CHECK(httpd_stop(local_http)); local_http = nullptr; }
                ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
                ap_active = false;
                ESP_LOGI(TAG, "Setup Wi-Fi closed");
            }
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
    ap_netif = esp_netif_create_default_wifi_ap();
    if (!station_netif || !ap_netif) return ESP_ERR_NO_MEM;
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
    return events && state.load() == State::connected && !ap_active &&
        (xEventGroupGetBits(events) & CONNECTED);
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
        // Same list as the setup page. A camera password is only required
        // while none is saved, so re-running setup keeps the current one.
        std::string networks;
        const esp_err_t err = state.load() == State::connecting ? ESP_ERR_INVALID_STATE : scan_networks(networks);
        submitting = false;
        if (err != ESP_OK) { reply(err == ESP_ERR_INVALID_STATE ? "busy" : "scan_failed"); return; }
        std::printf("SENSORFUN_USB {\"result\":\"scan\",\"ssids\":%s%s}\n",
                    networks.c_str(), camera_password_fields());
        return;
    }
    const char *result = "invalid";
    if (input.starts_with("wifi ") && state.load() != State::connecting) {
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
    } else if (input.starts_with("wifi ")) {
        result = "busy";
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
