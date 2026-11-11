#include "common/application.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void) {
    static common::Application common(common_board_config);
    ESP_ERROR_CHECK(common.setup());
    ESP_LOGI("hello_world", "Camera profile: %s (capture not initialized)", FIRMWARE_CAMERA_SENSOR);
    TickType_t last_message = xTaskGetTickCount();
    while (true) {
        common.main();
        const TickType_t now = xTaskGetTickCount();
        if (now - last_message >= pdMS_TO_TICKS(1000)) {
            ESP_LOGI("hello_world", "Hello from %s", common.board().name);
            last_message = now;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
