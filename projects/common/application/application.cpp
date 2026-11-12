#include "common/application.h"

#include <cstdio>
#include <cstring>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "soc/soc_caps.h"
#if SOC_WIFI_SUPPORTED
#include "provisioning.h"
#endif

namespace {
constexpr char TAG[] = "common";
constexpr uint32_t SETUP_REQUESTED = 0x53545550;
// Retain only an explicit setup request across software restart. Other reset
// reasons ignore and clear the marker; ordinary reboots never erase NVS.
RTC_NOINIT_ATTR uint32_t setup_request;
uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
}

namespace common {
esp_err_t Application::configure_reset_pins() {
    const auto &reset = board_.setup_reset;
    if (reset.hold_ms < 1000 || reset.bridge_gpio < -1 || reset.sense_gpio < -1) return ESP_ERR_INVALID_ARG;
    if (reset.sense_gpio == -1) return reset.bridge_gpio == -1 ? ESP_OK : ESP_ERR_INVALID_ARG;
    if (!GPIO_IS_VALID_GPIO(reset.sense_gpio)) return ESP_ERR_INVALID_ARG;
    if (reset.bridge_gpio != -1) {
        if (reset.bridge_gpio == reset.sense_gpio || !GPIO_IS_VALID_OUTPUT_GPIO(reset.bridge_gpio)) return ESP_ERR_INVALID_ARG;
        // Never drive a bridge pin high: released is high-impedance.
        ESP_RETURN_ON_ERROR(gpio_set_level(static_cast<gpio_num_t>(reset.bridge_gpio), 1), TAG, "Bridge level failed");
        gpio_config_t output{};
        output.pin_bit_mask = 1ULL << reset.bridge_gpio;
        output.mode = GPIO_MODE_OUTPUT_OD;
        ESP_RETURN_ON_ERROR(gpio_config(&output), TAG, "Bridge configuration failed");
    }
    gpio_config_t input{};
    input.pin_bit_mask = 1ULL << reset.sense_gpio;
    input.mode = GPIO_MODE_INPUT;
    input.pull_up_en = GPIO_PULLUP_ENABLE;
    return gpio_config(&input);
}

bool Application::reset_active() const {
    const auto &reset = board_.setup_reset;
    if (reset.sense_gpio == -1) return false;
    const auto sense = static_cast<gpio_num_t>(reset.sense_gpio);
    if (reset.bridge_gpio == -1) return gpio_get_level(sense) == 0;
    const auto bridge = static_cast<gpio_num_t>(reset.bridge_gpio);
    gpio_set_level(bridge, 1);
    esp_rom_delay_us(50);
    const bool released_high = gpio_get_level(sense) != 0;
    gpio_set_level(bridge, 0);
    esp_rom_delay_us(50);
    const bool driven_low = gpio_get_level(sense) == 0;
    gpio_set_level(bridge, 1);
    return released_high && driven_low;
}

esp_err_t Application::setup() {
    if (started_ || !board_.name) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(configure_reset_pins(), TAG, "Invalid board setup/reset configuration");
    if (esp_reset_reason() == ESP_RST_SW && setup_request == SETUP_REQUESTED) {
        // Erase before Wi-Fi or project tasks can hold NVS handles. Do not erase
        // the partition underneath running applications or on ordinary resets.
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Could not clear default NVS");
        setup_request = 0;
        ESP_LOGI(TAG, "Default NVS cleared; starting fresh setup");
    } else setup_request = 0;
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "NVS initialization failed");
    started_ = true;
    if (board_.setup_reset.check_at_startup && reset_active()) {
        // A jumper present at startup must be held and then removed before
        // peripherals claim these pins or networking starts.
        while (reset_active() || reset_.tracking()) {
            main();
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
#if SOC_WIFI_SUPPORTED
    if (board_.has_wifi) ESP_RETURN_ON_ERROR(provisioning_start(), TAG, "Provisioning initialization failed");
#else
    if (board_.has_wifi) return ESP_ERR_NOT_SUPPORTED;
#endif
    if (serial_reset_ && xTaskCreate(serial_worker, "setup_console", 3072, this, 3, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Common setup ready");
    return ESP_OK;
}

void Application::main() {
    if (!started_) return;
    if (reset_.update(reset_active(), now_ms())) enter_setup();
    if (reset_.armed() && !announced_) {
        announced_ = true;
        ESP_LOGI(TAG, "Release setup button or remove jumper to clear NVS and restart");
    } else if (!reset_.armed()) announced_ = false;
}

void Application::enter_setup() {
    if (!started_) return;
    const auto &reset = board_.setup_reset;
    if (reset.sense_gpio >= 0 && reset.restart_pull != RestartPull::unchanged) {
        const auto pull = reset.restart_pull == RestartPull::down ? GPIO_PULLDOWN_ONLY : GPIO_PULLUP_ONLY;
        ESP_ERROR_CHECK(gpio_set_pull_mode(static_cast<gpio_num_t>(reset.sense_gpio), pull));
    }
    setup_request = SETUP_REQUESTED;
    ESP_LOGI(TAG, "Restarting into setup; default NVS will be cleared");
    esp_restart();
}

void Application::serial_worker(void *context) {
    auto &app = *static_cast<Application *>(context);
    char line[97]{};
#if CONFIG_PROVISIONING_SECURE
    char ssid[33]{};
    int provisioning_input = 0;
#endif
    size_t length = 0;
    bool overflow = false, last_was_cr = false;
    ESP_LOGI(TAG, "Type wifi-reset then Enter to clear NVS and restart setup");
    for (;;) {
        const int ch = std::getchar();
        if (ch == EOF) {
            std::clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (ch == '\n' && last_was_cr) { last_was_cr = false; continue; }
        last_was_cr = ch == '\r';
        if (ch == '\n' || ch == '\r') {
#if CONFIG_PROVISIONING_SECURE
            if (provisioning_input == 1) {
                if (!overflow && length > 0 && length <= 32) {
                    memcpy(ssid, line, length); provisioning_input = 2;
                    ESP_LOGI(TAG, "USB provisioning: password (input is not echoed)");
                } else { provisioning_input = 0; ESP_LOGE(TAG, "Invalid SSID length"); }
            } else if (provisioning_input == 2) {
                const esp_err_t err = overflow ? ESP_ERR_INVALID_ARG : provisioning_submit_credentials(ssid, line);
                ESP_LOGI(TAG, "USB provisioning submitted: %s", esp_err_to_name(err));
                memset(ssid, 0, sizeof(ssid)); provisioning_input = 0;
            } else if (!overflow && strcmp(line, "wifi-provision") == 0) {
                provisioning_input = 1;
                ESP_LOGI(TAG, "USB provisioning: SSID");
            } else
#endif
            if (!overflow && std::strcmp(line, "wifi-reset") == 0) app.enter_setup();
            std::memset(line, 0, sizeof(line));
            length = 0;
            overflow = false;
        } else if (length < sizeof(line) - 1) line[length++] = static_cast<char>(ch);
        else overflow = true;
    }
}
}
