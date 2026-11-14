#include "camera_access.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <vector>
#include "psa/crypto.h"
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <string>

namespace camera_access {
namespace { std::atomic<uint32_t> revision{0}; std::mutex record_mutex; }
void wipe(void *data, size_t size) { volatile uint8_t *p = static_cast<volatile uint8_t *>(data); while (size--) *p++ = 0; }
bool equal(const uint8_t *a, const uint8_t *b, size_t size) { unsigned diff = 0; for (size_t i = 0; i < size; ++i) diff |= a[i] ^ b[i]; return diff == 0; }
bool valid_password(const char *password) { if (!password) return false; size_t n = strnlen(password, 97); return n >= 12 && n <= 96; }
uint32_t generation() { return revision; }
bool hash(const void *input, size_t length, uint8_t output[32]) {
    size_t written;
    return psa_hash_compute(PSA_ALG_SHA_256, static_cast<const uint8_t *>(input), length, output, 32, &written) == PSA_SUCCESS && written == 32;
}
static bool import_key(const uint8_t *key, size_t size, mbedtls_svc_key_id_t &id) {
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);
    psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_bits(&attrs, size * 8);
    psa_status_t ret = psa_import_key(&attrs, key, size, &id);
    psa_reset_key_attributes(&attrs);
    return ret == PSA_SUCCESS;
}
static bool compute(mbedtls_svc_key_id_t key, const void *input, size_t length, uint8_t output[32]) {
    size_t written;
    return psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), static_cast<const uint8_t *>(input), length, output, 32, &written) == PSA_SUCCESS && written == 32;
}
bool mac(const uint8_t *key, size_t size, const void *input, size_t length, uint8_t output[32]) {
    mbedtls_svc_key_id_t id = 0;
    if (!import_key(key, size, id)) return false;
    bool ok = compute(id, input, length, output);
    psa_destroy_key(id);
    return ok;
}
// PBKDF2-HMAC-SHA256, one 32-byte block. Yield regularly so password setup
// cannot starve the watchdog/idle tasks on either board.
static bool derive(const char *password, const uint8_t *salt, size_t salt_size, uint8_t output[32]) {
    mbedtls_svc_key_id_t id = 0;
    if (!import_key(reinterpret_cast<const uint8_t *>(password), strlen(password), id)) return false;
    std::vector<uint8_t> initial(salt_size + 4, 0);
    uint8_t u[32]{}, next[32]{};
    memcpy(initial.data(), salt, salt_size); initial.back() = 1;
    bool ok = compute(id, initial.data(), initial.size(), u);
    memcpy(output, u, 32);
    for (uint32_t i = 1; ok && i < ITERATIONS; ++i) {
        ok = compute(id, u, sizeof(u), next);
        for (int j = 0; j < 32; ++j) output[j] ^= next[j];
        memcpy(u, next, 32);
        if ((i & 511) == 0) vTaskDelay(1);
    }
    psa_destroy_key(id); wipe(initial.data(), initial.size()); wipe(u, sizeof(u)); wipe(next, sizeof(next));
    return ok;
}
esp_err_t set_password(const char *password) {
    if (!valid_password(password)) return ESP_ERR_INVALID_ARG;
    Record record;
    uint8_t root[32]{}, salt_hash[32]{}, salted[32]{}, client_key[32]{}, room[32]{};
    char camera[13], salt_hex[33];
    if (mac_room(camera, sizeof(camera)) != ESP_OK) return ESP_FAIL;
    const std::string salt_input = std::string("camera-kdf-v2\n") + camera;
    const std::string room_input = std::string("camera-room-v1\n") + camera;
    esp_fill_random(record.salt, sizeof(record.salt));
    for (size_t i = 0; i < sizeof(record.salt); ++i) snprintf(salt_hex + i * 2, 3, "%02x", record.salt[i]);
    const std::string auth_input = std::string("camera-auth-v2\n") + salt_hex;
    bool ok = hash(salt_input.data(), salt_input.size(), salt_hash) &&
        derive(password, salt_hash, sizeof(salt_hash), root) &&
        mac(root, 32, room_input.data(), room_input.size(), room) &&
        mac(root, 32, auth_input.data(), auth_input.size(), salted) &&
        mac(salted, 32, "Client Key", 10, client_key) && hash(client_key, 32, record.stored_key) &&
        mac(salted, 32, "Server Key", 10, record.server_key);
    memcpy(record.room_digest, room, sizeof(record.room_digest));
    wipe(root, sizeof(root)); wipe(salt_hash, sizeof(salt_hash)); wipe(room, sizeof(room));
    wipe(salted, sizeof(salted)); wipe(client_key, sizeof(client_key));
    if (!ok) { wipe(&record, sizeof(record)); return ESP_FAIL; }
    std::lock_guard<std::mutex> guard(record_mutex);
    nvs_handle_t handle;
    esp_err_t ret = nvs_open("camera_access", NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        ret = nvs_set_blob(handle, "verifier", &record, sizeof(record));
        if (ret == ESP_OK) ret = nvs_commit(handle);
        nvs_close(handle);
    }
    wipe(&record, sizeof(record));
    if (ret == ESP_OK) ++revision;
    return ret;
}
esp_err_t reset_settings() {
    // Called before application tasks start. Forget Wi-Fi/passwords while
    // retaining only the ID needed to remove this board's stale public slot.
    esp_err_t ret = nvs_flash_init();
    if (ret != ESP_OK) return ret;
    std::vector<uint8_t> resume;
    nvs_handle_t handle;
    ret = nvs_open("apprtc_resume", NVS_READONLY, &handle);
    if (ret == ESP_OK) {
        size_t size = 0;
        ret = nvs_get_blob(handle, "own_membership", nullptr, &size);
        if (ret == ESP_OK && size > 0 && size <= 256) {
            resume.resize(size);
            ret = nvs_get_blob(handle, "own_membership", resume.data(), &size);
        } else if (ret == ESP_OK) ret = ESP_ERR_INVALID_SIZE;
        nvs_close(handle);
    }
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) return ret;
    ret = nvs_flash_deinit();
    if (ret == ESP_OK) ret = nvs_flash_erase();
    if (ret == ESP_OK && !resume.empty()) {
        ret = nvs_flash_init();
        if (ret == ESP_OK) {
            ret = nvs_open("apprtc_resume", NVS_READWRITE, &handle);
            if (ret == ESP_OK) {
                ret = nvs_set_blob(handle, "own_membership", resume.data(), resume.size());
                if (ret == ESP_OK) ret = nvs_commit(handle);
                nvs_close(handle);
            }
            esp_err_t closed = nvs_flash_deinit();
            if (ret == ESP_OK) ret = closed;
        }
    }
    return ret;
}
esp_err_t clear_password() {
    std::lock_guard<std::mutex> guard(record_mutex);
    nvs_handle_t handle;
    esp_err_t ret = nvs_open("camera_access", NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        ret = nvs_erase_key(handle, "verifier");
        if (ret == ESP_ERR_NVS_NOT_FOUND) ret = ESP_OK;
        if (ret == ESP_OK) ret = nvs_commit(handle);
        nvs_close(handle);
    }
    if (ret == ESP_OK) ++revision;
    return ret;
}
esp_err_t load(Record &record, uint32_t *loaded_generation) {
    std::lock_guard<std::mutex> guard(record_mutex);
    nvs_handle_t handle;
    esp_err_t ret = nvs_open("camera_access", NVS_READONLY, &handle);
    if (ret != ESP_OK) return ret;
    size_t size = sizeof(record);
    ret = nvs_get_blob(handle, "verifier", &record, &size); nvs_close(handle);
    if (ret == ESP_OK && (size != sizeof(record) || record.version != 2 || record.iterations != ITERATIONS)) ret = ESP_ERR_INVALID_STATE;
    if (ret != ESP_OK) wipe(&record, sizeof(record));
    else if (loaded_generation) *loaded_generation = revision;
    return ret;
}
esp_err_t mac_room(char *out, size_t size) {
    if (size < 13) return ESP_ERR_INVALID_SIZE;
    uint8_t address[6];
    esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, address);
    if (ret != ESP_OK) return ret;
    snprintf(out, size, "%02X%02X%02X%02X%02X%02X", address[0], address[1], address[2], address[3], address[4], address[5]);
    return ESP_OK;
}
esp_err_t signaling_room(char *out, size_t size) {
    if (size < 33) return ESP_ERR_INVALID_SIZE;
    Record record;
    esp_err_t ret = load(record);
    if (ret == ESP_OK) {
        for (size_t i = 0; i < sizeof(record.room_digest); ++i) snprintf(out + i * 2, 3, "%02X", record.room_digest[i]);
    }
    wipe(&record, sizeof(record));
    return ret;
}

}
