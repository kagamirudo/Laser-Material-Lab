#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mount point used for VFS
#define SDCARD_MOUNT_POINT "/sdcard"
#define MAX_CHAR_SIZE 1024

// SPI host and pins for the SD card module
// Adjust these GPIOs to match your wiring.
// They must be output-capable pins on your ESP32-S3 board.
//
// NEW WIRING (ESP32-S3 to SD Card Module):
// =======================================
// ESP32-S3 Pin    ->  SD Card Module Pin
// --------------------------------------------
// GPIO 11 (MOSI)  ->  DI  (Data In / MOSI)
// GPIO 13 (MISO)  ->  DO  (Data Out / MISO)
// GPIO 12 (CLK)   ->  CLK (Clock / SCK)
// GPIO 14 (CS)    ->  CS  (Chip Select)
// 3.3V            ->  VCC
// GND             ->  GND
//
// NOTES:
// - Matches the pin assignment you requested:
//     MOSI: GPIO 11
//     MISO: GPIO 13
//     CLK : GPIO 12
//     CS  : GPIO 14
// - Keep SPI wires short and ensure proper pull‑ups on SD lines.
//
#define SDCARD_SPI_HOST SPI2_HOST
#define SDCARD_PIN_MOSI GPIO_NUM_11
#define SDCARD_PIN_MISO GPIO_NUM_13
#define SDCARD_PIN_SCK GPIO_NUM_12
#define SDCARD_PIN_CS GPIO_NUM_14

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
 * @brief Write data to a file on the SD card.
 */
static esp_err_t sdcard_write_file(const char *filename, const char *data);

/**
 * @brief Read data from a file on the SD card.
 */
static esp_err_t sdcard_read_file(const char *filename);

/**
 * @brief Test SD card by reading capacity and performing read/write test
 *
 * Verifies the card can be detected, shows capacity (should show ~32GB),
 * and performs a simple write/read/delete test to confirm functionality.
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t sdcard_test_read(void);

/**
 * @brief List files and directories in the SD card mount point.
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t sdcard_list_files(void);

#ifdef __cplusplus
}
#endif
