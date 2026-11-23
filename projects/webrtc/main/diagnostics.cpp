#include "diagnostics.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <cstdio>
#include <cstring>

namespace {
struct Entry { uint32_t id; int64_t ms; char level[8]; char message[128]; uint32_t repeats; };
Entry entries[48]{};
uint32_t sequence = 0;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
}
void diagnostic_event(const char *level, const char *message) {
    if (!message || !*message) return;
    const int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&lock);
    Entry &last = entries[sequence % 48];
    if (sequence && !strcmp(last.message, message) && !strcmp(last.level, level) && now - last.ms < 10000) {
        // Give the replacement a fresh sequence so connected viewers see its count.
        Entry copy = last;
        copy.id = ++sequence; copy.ms = now; ++copy.repeats;
        entries[sequence % 48] = copy;
    } else {
        Entry &entry = entries[++sequence % 48];
        entry.id = sequence; entry.ms = now; entry.repeats = 1;
        snprintf(entry.level, sizeof(entry.level), "%s", level);
        snprintf(entry.message, sizeof(entry.message), "%s", message);
    }
    portEXIT_CRITICAL(&lock);
}
cJSON *diagnostic_logs(uint32_t after) {
    Entry batch[4]{};
    unsigned count = 0;
    uint32_t oldest, newest;
    portENTER_CRITICAL(&lock);
    newest = sequence;
    oldest = newest > 48 ? newest - 47 : 1;
    uint32_t start = after >= newest ? newest + 1 : (after + 1 < oldest ? oldest : after + 1);
    for (uint32_t id = start; id <= newest && count < 4; ++id) batch[count++] = entries[id % 48];
    portEXIT_CRITICAL(&lock);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "logs");
    cJSON_AddBoolToObject(reply, "gap", after && after + 1 < oldest);
    cJSON *list = cJSON_AddArrayToObject(reply, "entries");
    for (unsigned i = 0; i < count; ++i) {
        const Entry &entry = batch[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", entry.id);
        cJSON_AddNumberToObject(item, "ms", entry.ms);
        cJSON_AddStringToObject(item, "level", entry.level);
        cJSON_AddStringToObject(item, "message", entry.message);
        cJSON_AddNumberToObject(item, "repeats", entry.repeats);
        cJSON_AddItemToArray(list, item);
    }
    return reply;
}
