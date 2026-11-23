#include "sd_card.h"
#include "diagnostics.h"
#include "esp_log.h"
#include <atomic>

#if defined(SD_MMC_D0_GPIO_NUM)
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#endif

namespace sd_card {
namespace {
constexpr char TAG[] = "sd_card";
#if defined(SD_MMC_D0_GPIO_NUM)
std::atomic<State> current{State::absent};
sdmmc_card_t *card = nullptr;
bool quiet = false;
#else
std::atomic<State> current{State::unsupported};
#endif
}  // namespace

#if defined(SD_MMC_D0_GPIO_NUM)
namespace {
esp_err_t mount_card(bool format_if_needed) {
    if (card) return ESP_OK;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SD_MMC_SLOT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    // One data line leaves D1-D3 free; on the AI Thinker board D2 is the setup
    // jumper (GPIO12) and D1 drives the flash LED.
    slot.width = 1;
    slot.clk = static_cast<gpio_num_t>(SD_MMC_CLK_GPIO_NUM);
    slot.cmd = static_cast<gpio_num_t>(SD_MMC_CMD_GPIO_NUM);
    slot.d0 = static_cast<gpio_num_t>(SD_MMC_D0_GPIO_NUM);
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_mount_config_t config = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    // Never format on our own: the card may hold the user's other files. Only
    // an explicit format request formats a card that has no FAT volume.
    config.format_if_mount_failed = format_if_needed;
    config.max_files = 4;
    config.allocation_unit_size = 16 * 1024;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(kRoot, &host, &slot, &config, &card);
    if (err != ESP_OK) {
        card = nullptr;
        // Retries run every few seconds with no card inserted; report the first
        // failure only, and keep the driver quiet afterwards.
        if (!quiet) {
            diagnostic_eventf("warning", "SD unavailable: %s", esp_err_to_name(err));
            ESP_LOGW(TAG, "No usable SD card (%s); motion is still detected but not recorded", esp_err_to_name(err));
            esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
            esp_log_level_set("sdmmc_init", ESP_LOG_NONE);
            esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
            quiet = true;
        }
        // ESP_FAIL means a card answered but holds no FAT volume.
        current = err == ESP_FAIL ? State::error : State::absent;
        return err;
    }
    ESP_LOGI(TAG, "Mounted %s card, %llu MB", card->cid.name,
             (unsigned long long)(uint64_t(card->csd.capacity) * card->csd.sector_size / (1024 * 1024)));
    diagnostic_eventf("info", "SD mounted: %llu MB", (unsigned long long)(uint64_t(card->csd.capacity) * card->csd.sector_size / (1024 * 1024)));
    current = State::mounted;
    quiet = false;
    return ESP_OK;
}
}  // namespace
#endif

esp_err_t mount() {
#if defined(SD_MMC_D0_GPIO_NUM)
    return mount_card(false);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t format() {
#if defined(SD_MMC_D0_GPIO_NUM)
    diagnostic_event("warning", "SD formatting started");
    current = State::formatting;
    esp_err_t err = card ? ESP_OK : mount_card(true);
    if (err == ESP_OK) err = esp_vfs_fat_sdcard_format(kRoot, card);
    diagnostic_eventf(err == ESP_OK ? "info" : "error", "SD format result: %s", esp_err_to_name(err));
    if (err == ESP_OK) ESP_LOGI(TAG, "SD card formatted");
    else ESP_LOGE(TAG, "SD card format failed: %s", esp_err_to_name(err));
    current = card ? State::mounted : err == ESP_FAIL ? State::error : State::absent;
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void unmount(State reason) {
#if defined(SD_MMC_D0_GPIO_NUM)
    if (card) {
        esp_vfs_fat_sdcard_unmount(kRoot, card);
        card = nullptr;
    }
    diagnostic_event("warning", "SD unmounted");
    current = reason;
#else
    (void)reason;
#endif
}

State state() { return current; }

const char *state_name() {
    switch (current.load()) {
    case State::unsupported: return "unsupported";
    case State::absent: return "absent";
    case State::mounted: return "mounted";
    case State::formatting: return "formatting";
    default: return "error";
    }
}

bool space(uint64_t &total, uint64_t &free) {
#if defined(SD_MMC_D0_GPIO_NUM)
    return current == State::mounted && esp_vfs_fat_info(kRoot, &total, &free) == ESP_OK;
#else
    (void)total; (void)free;
    return false;
#endif
}

}  // namespace sd_card
