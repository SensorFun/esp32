#pragma once
#include "common/board.h"

#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM    5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22
#define LED_GPIO_NUM   4

// On-board microSD slot, SDMMC one-bit mode. The original ESP32 routes slot 1
// through fixed IO_MUX pins, so these document the wiring rather than select it.
#define SD_MMC_CMD_GPIO_NUM 15
#define SD_MMC_CLK_GPIO_NUM 14
#define SD_MMC_D0_GPIO_NUM  2
#define SD_MMC_SLOT 1

// GPIO12-to-GND setup jumper; reserve this pin instead of using SD D2.
// GPIO12 is a voltage strapping pin: restore its low pull before restart.
inline constexpr common::BoardConfig common_board_config = {
    .name = board_display_name,
    .setup_reset = {.sense_gpio = 12, .check_at_startup = true, .restart_pull = common::RestartPull::down},
};
