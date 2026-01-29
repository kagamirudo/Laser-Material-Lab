#include "sdcard.h"

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "SDCARD";

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

esp_err_t sdcard_init(void) {
    if (s_mounted) {
        return ESP_OK;
    }

    esp_err_t ret;

    // --- Configure SPI bus (match Espressif SDSPI example behaviour) ---
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SDCARD_PIN_MOSI,
        .miso_io_num = SDCARD_PIN_MISO,
        .sclk_io_num = SDCARD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ESP_LOGI(TAG, "Initializing SPI bus for SD card...");
    ESP_LOGI(TAG, "SPI pins: MOSI=GPIO%d, MISO=GPIO%d, SCK=GPIO%d, CS=GPIO%d",
             SDCARD_PIN_MOSI, SDCARD_PIN_MISO, SDCARD_PIN_SCK, SDCARD_PIN_CS);

    // Use SDSPI default host config (same as Espressif example)
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    // Explicitly request 20 MHz SD clock (max for SDSPI in many examples)
    host.max_freq_khz = 20000;  // 20.00 MHz

    // Initialize SPI bus on the host slot with default DMA settings
    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // Device (slot) configuration
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SDCARD_PIN_CS;
    slot_config.host_id = host.slot;

    ESP_LOGI(TAG, "Attempting to mount SD card at %s (SDSPI host slot=%d, max_freq=%d kHz)...",
             SDCARD_MOUNT_POINT, host.slot, host.max_freq_khz);

    // Mount config: mirror Espressif example semantics
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,   // set true if you want auto-format on first failure
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &slot_config,
                                  &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s (0x%x)", esp_err_to_name(ret), ret);
        ESP_LOGE(TAG, "Troubleshooting tips:");
        ESP_LOGE(TAG, "  1. Check wiring: MOSI->GPIO%d, MISO->GPIO%d, SCK->GPIO%d, CS->GPIO%d",
                 SDCARD_PIN_MOSI, SDCARD_PIN_MISO, SDCARD_PIN_SCK, SDCARD_PIN_CS);
        ESP_LOGE(TAG, "  2. Verify power: Ensure SD module has stable 3.3V or 5V power");
        ESP_LOGE(TAG, "  3. Check ground: Ensure common ground between ESP32 and SD module");
        ESP_LOGE(TAG, "  4. Verify card: Ensure SD card is inserted and formatted as FAT32");
        ESP_LOGE(TAG, "  5. Try slower: If issue persists, reduce max_freq_khz in code");
        // Clean up bus on failure
        spi_bus_free(SDCARD_SPI_HOST);
        s_card = NULL;
        s_mounted = false;
        return ret;
    }

    s_mounted = true;

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", SDCARD_MOUNT_POINT);

    return ESP_OK;
}

void sdcard_deinit(void) {
    if (!s_mounted) {
        return;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to unmount SD card: %s", esp_err_to_name(ret));
    }

    spi_bus_free(SDCARD_SPI_HOST);
    s_card = NULL;
    s_mounted = false;

    ESP_LOGI(TAG, "SD card unmounted");
}

sdmmc_card_t *sdcard_get_card(void) { return s_mounted ? s_card : NULL; }

bool sdcard_is_mounted(void) { return s_mounted; }

static esp_err_t sdcard_write_file(const char *filename, const char *data) {
    FILE *f = fopen(filename, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filename);
        return ESP_FAIL;
    }
    fprintf(f, "%s", data);
    fclose(f);
    return ESP_OK;
}

static esp_err_t sdcard_read_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", filename);
        return ESP_FAIL;
    }
    char line[MAX_CHAR_SIZE];
    fgets(line, MAX_CHAR_SIZE, f);
    fclose(f);
    ESP_LOGI(TAG, "Read data: %s", line);
    return ESP_OK;
}

/**
 * @brief Test SD card by reading capacity and performing a simple read/write test
 * 
 * This function:
 * - Checks if card is mounted
 * - Reads and displays card capacity (should show ~32GB for your card)
 * - Creates a test file, writes to it, reads it back, then deletes it
 * - Verifies the card can be read and written to
 * 
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t sdcard_test_read(void) {
    if (!s_mounted || s_card == NULL) {
        ESP_LOGE(TAG, "SD card not mounted. Call sdcard_init() first.");
        return ESP_ERR_INVALID_STATE;
    }

    // Get card capacity
    uint64_t card_size_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    uint64_t card_size_mb = card_size_bytes / (1024 * 1024);
    uint64_t card_size_gb = card_size_bytes / (1024 * 1024 * 1024);
    
    // Determine card type based on capacity (more reliable than OCR flags)
    const char *card_type;
    if (card_size_gb <= 2) {
        card_type = "SDSC (Standard Capacity)";
    } else if (card_size_gb <= 32) {
        card_type = "SDHC (High Capacity)";
    } else {
        card_type = "SDXC (Extended Capacity)";
    }
    
    ESP_LOGI(TAG, "=== SD Card Test ===");
    ESP_LOGI(TAG, "Card detected: %s", s_card->cid.name);
    ESP_LOGI(TAG, "Card type: %s", card_type);
    ESP_LOGI(TAG, "Card capacity: %llu bytes (%.2f MB / %.2f GB)", 
             card_size_bytes, (double)card_size_mb, (double)card_size_gb);
    ESP_LOGI(TAG, "Sector size: %d bytes", s_card->csd.sector_size);
    
    // Verify 32GB card (should be around 32GB, allow some tolerance)
    if (card_size_gb < 28 || card_size_gb > 35) {
        ESP_LOGW(TAG, "WARNING: Card size (%.2f GB) doesn't match expected 32GB", (double)card_size_gb);
    } else {
        ESP_LOGI(TAG, "✓ Card size matches expected 32GB range");
    }

    sdcard_list_files();
    // Test file operations: write, read, delete
    const char *test_file = SDCARD_MOUNT_POINT "/foo.txt";

    // Create a file - buffer
    char test_data[MAX_CHAR_SIZE];
    snprintf(test_data, MAX_CHAR_SIZE, "Hello123 %s", s_card->cid.name);
    
    // Write test
    esp_err_t ret = sdcard_write_file(test_file, test_data);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "✓ Test file written successfully (%d bytes)", strlen(test_data));
    
    // Read test
    char read_buffer[128] = {0};
    FILE *f = fopen(test_file, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "✗ Failed to open test file for reading");
        remove(test_file);
        return ESP_FAIL;
    }
    size_t read = fread(read_buffer, 1, sizeof(read_buffer) - 1, f);
    fclose(f);
    
    if (read != strlen(test_data) || strcmp(read_buffer, test_data) != 0) {
        ESP_LOGE(TAG, "✗ Test data mismatch (read %d bytes)", read);
        // remove(test_file);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "✓ Test file read successfully: \"%s\"", read_buffer);
    
    // Cleanup
    // if (remove(test_file) != 0) {
    //     ESP_LOGW(TAG, "Warning: Failed to delete test file");
    // } else {
    //     ESP_LOGI(TAG, "✓ Test file deleted");
    // }
    
    ESP_LOGI(TAG, "=== SD Card Test PASSED ===");
    ESP_LOGI(TAG, "Card is ready for use. Mount point: %s", SDCARD_MOUNT_POINT);
    
    return ESP_OK;
}

esp_err_t sdcard_list_files(void) {
    if (!s_mounted || s_card == NULL) {
        ESP_LOGE(TAG, "SD card not mounted. Call sdcard_init() first.");
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(SDCARD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open SD card directory: %s (errno=%d: %s)",
                 SDCARD_MOUNT_POINT, errno, strerror(errno));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Listing files in %s:", SDCARD_MOUNT_POINT);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        ESP_LOGI(TAG, " - %s", entry->d_name);
    }

    closedir(dir);
    return ESP_OK;
}
