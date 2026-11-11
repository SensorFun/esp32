#pragma once
#include "common/board.h"

inline constexpr char camera_board_name[] = "ESP32-S3-WROOM N16R8 CAM";

#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5
#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM    8
#define Y3_GPIO_NUM    9
#define Y2_GPIO_NUM    11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

// On-board microSD slot, SDMMC one-bit mode.
#define SD_MMC_CMD_GPIO_NUM 38
#define SD_MMC_CLK_GPIO_NUM 39
#define SD_MMC_D0_GPIO_NUM  40
// The S3 has a single SDMMC slot pair; slot 1 is the one wired here.
#define SD_MMC_SLOT 1

// BOOT is checked while running; do not hold strapping pins at power-on.
inline constexpr common::BoardConfig common_board_config = {
    .name = camera_board_name,
};
