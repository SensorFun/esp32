#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace camera_access {
constexpr uint32_t ITERATIONS = 210000;
struct Record {
    uint32_t version = 2, iterations = ITERATIONS;
    uint8_t salt[16]{}, stored_key[32]{}, server_key[32]{}, room_digest[16]{};
};
void wipe(void *data, size_t size);
bool valid_password(const char *password);
esp_err_t reset_settings(); // Boot-time only: retain own signaling cleanup metadata.
esp_err_t clear_password();
esp_err_t set_password(const char *password);
esp_err_t load(Record &record, uint32_t *loaded_generation = nullptr);
uint32_t generation();
esp_err_t mac_room(char *out, size_t size); // Public camera identifier only.
esp_err_t signaling_room(char *out, size_t size);
bool mac(const uint8_t *key, size_t size, const void *input, size_t length, uint8_t output[32]);
bool hash(const void *input, size_t length, uint8_t output[32]);
bool equal(const uint8_t *a, const uint8_t *b, size_t size);
}
