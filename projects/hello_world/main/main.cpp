#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void) {
    while (true) {
        ESP_LOGI("hello_world", "Hello from %s", camera_board_name);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
