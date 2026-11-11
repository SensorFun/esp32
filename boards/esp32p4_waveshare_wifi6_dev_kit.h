#pragma once

// Waveshare ESP32-P4-WIFI6-DEV-KIT (not ESP32-P4-WIFI6).
// Schematic: https://files.waveshare.com/wiki/ESP32-P4-WIFI6-DEV-KIT/ESP32-P4-WIFI6-DEV-KIT.pdf
// Camera uses dedicated two-lane MIPI-CSI signals, not a DVP GPIO bus.
// The sensor is a separately attached module; do not assume an OV2640.
inline constexpr char camera_board_name[] = "Waveshare ESP32-P4 MIPI-CSI";

// On-board TF slot. GPIO45 controls its active-low power switch.
#define SD_MMC_CMD_GPIO_NUM 44
#define SD_MMC_CLK_GPIO_NUM 43
#define SD_MMC_D0_GPIO_NUM  39
#define SD_MMC_D1_GPIO_NUM  40
#define SD_MMC_D2_GPIO_NUM  41
#define SD_MMC_D3_GPIO_NUM  42
#define SD_MMC_POWER_GPIO_NUM 45
#define SD_MMC_POWER_ON_LEVEL 0
// These are exactly the P4's slot-0 IOMUX pads (SDMMC_SLOT0_IOMUX_PIN_NUM_*).
// Slot 1 has no IOMUX pins on this chip, so leaving the driver on its default
// slot routes slot-1 signals through the GPIO matrix onto slot-0's dedicated
// pads and the host times out before it ever addresses the card.
#define SD_MMC_SLOT 0
// The P4 declares SOC_SDMMC_IO_POWER_EXTERNAL: the SD I/O rail comes from an
// on-chip LDO that must be brought up before the bus will clock at all.
#define SD_MMC_LDO_CHANNEL 4

// Shared camera SCCB bus; MIPI data lanes are dedicated peripheral pins.
#define CAMERA_SCL_GPIO_NUM 8
#define CAMERA_SDA_GPIO_NUM 7
#define CAMERA_RESET_GPIO_NUM -1
#define CAMERA_PWDN_GPIO_NUM -1

// BOOT button, sampled only after normal application startup.
#define WIFI_SETUP_BUTTON_GPIO_NUM 35
