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
//
// WIRING DIAGRAM (ESP32-S3 to SD Card Module):
// ============================================
// ESP32-S3 Pin    ->  SD Card Module Pin
// --------------------------------------------
// GPIO 41 (MOSI)  ->  DI (Data In / MOSI)
// GPIO 42 (MISO)  ->  DO (Data Out / MISO)
// GPIO 40 (SCK)   ->  CLK (Clock / SCK)
// GPIO 39 (CS)    ->  CS (Chip Select)
// 3.3V            ->  VCC (Power - check if module needs 5V or 3.3V)
// GND             ->  GND (Ground)
//
// NOTES:
// - Using HSPI pins (GPIO 40-42, 39) as per Arduino example
// - Most SD card modules have onboard 3.3V regulator (accept 5V input)
// - If module has 5V pin, you can use 5V; if only 3.3V, use 3.3V
// - Ensure common ground connection
// - Keep SPI wires short (< 10cm recommended)
// - Module should have pull-up resistors (most do)
// - Card must be formatted as FAT32 (32GB cards are FAT32 by default)
//
#define SDCARD_SPI_HOST SPI2_HOST
#define SDCARD_PIN_MOSI GPIO_NUM_41
#define SDCARD_PIN_MISO GPIO_NUM_42
#define SDCARD_PIN_SCK GPIO_NUM_40
#define SDCARD_PIN_CS GPIO_NUM_39

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

/**
 * @brief Test SD card by reading capacity and performing read/write test
 * 
 * Verifies the card can be detected, shows capacity (should show ~32GB),
 * and performs a simple write/read/delete test to confirm functionality.
 * 
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t sdcard_test_read(void);

#ifdef __cplusplus
}
#endif
