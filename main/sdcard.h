#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mount point used for VFS
#define SDCARD_MOUNT_POINT "/sdcard"

// SPI host and pins for the SD card module
// Adjust these GPIOs to match your wiring.
// They must be output-capable pins on your ESP32-S3 board.
#define SDCARD_SPI_HOST SPI2_HOST
#define SDCARD_PIN_MOSI GPIO_NUM_11
#define SDCARD_PIN_MISO GPIO_NUM_13
#define SDCARD_PIN_SCK GPIO_NUM_12
#define SDCARD_PIN_CS GPIO_NUM_10

/**
 * @brief Initialize SPI bus, attach SD card, and mount FAT filesystem.
 *
 * This will mount the card at SDCARD_MOUNT_POINT ("/sdcard").
 * Safe to call multiple times; subsequent calls are no-ops if already mounted.
 */
esp_err_t sdcard_init(void);

/**
 * @brief Unmount filesystem and release SPI bus.
 */
void sdcard_deinit(void);

/**
 * @brief Get pointer to the underlying sdmmc_card_t structure.
 *
 * @return sdmmc_card_t* or NULL if card is not mounted.
 */
sdmmc_card_t *sdcard_get_card(void);

/**
 * @brief Check if SD card is currently mounted.
 */
bool sdcard_is_mounted(void);

#ifdef __cplusplus
}
#endif
