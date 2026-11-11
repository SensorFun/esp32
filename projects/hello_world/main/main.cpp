#include "esp_log.h"
#include "provisioning.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void) {
    const provisioning_config_t setup = {
        .reset_gpio = WIFI_SETUP_BUTTON_GPIO_NUM,
        .reset_hold_ms = 5000,
        .serial_reset = true,
    };
    ESP_ERROR_CHECK(provisioning_start(&setup));
    while (true) {
        ESP_LOGI("hello_world", "Hello from %s", camera_board_name);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
