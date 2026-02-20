#include <string.h>
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display.h"

static const char *TAG = "display";

// Defaults for ESP32-S3 DevKit I2C pins. Adjust as needed.
#ifndef DISPLAY_SDA_GPIO
#define DISPLAY_SDA_GPIO 8
#endif
#ifndef DISPLAY_SCL_GPIO
#define DISPLAY_SCL_GPIO 9
#endif
#ifndef DISPLAY_I2C_PORT
#define DISPLAY_I2C_PORT I2C_NUM_0
#endif
#ifndef DISPLAY_I2C_ADDR
#define DISPLAY_I2C_ADDR 0x3C
#endif

#define I2C_FREQ_HZ 400000
#define SSD1306_WIDTH 128
#define SSD1306_PAGES 8
#define DISPLAY_MAX_LINES 8
#define DISPLAY_CHARS_PER_LINE 21

static bool s_display_ready = false;

static char s_status_lines[DISPLAY_MAX_LINES][DISPLAY_CHARS_PER_LINE + 1];
static int s_status_count = 0;

static void push_status_line(const char *text)
{
    if (!text) {
        return;
    }
    size_t len = strlen(text);
    if (len == 0) {
        return;
    }
    if (len > DISPLAY_CHARS_PER_LINE) {
        len = DISPLAY_CHARS_PER_LINE;
    }
    if (s_status_count < DISPLAY_MAX_LINES) {
        memcpy(s_status_lines[s_status_count], text, len);
        s_status_lines[s_status_count][len] = '\0';
        s_status_count++;
    } else {
        memmove(s_status_lines[0], s_status_lines[1],
                (DISPLAY_MAX_LINES - 1) * (DISPLAY_CHARS_PER_LINE + 1));
        memcpy(s_status_lines[DISPLAY_MAX_LINES - 1], text, len);
        s_status_lines[DISPLAY_MAX_LINES - 1][len] = '\0';
    }
}

static const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // 32
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // 33 !
    {0x00, 0x07, 0x00, 0x07, 0x00}, // 34 "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // 35 #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // 36 $
    {0x23, 0x13, 0x08, 0x64, 0x62}, // 37 %
    {0x36, 0x49, 0x55, 0x22, 0x50}, // 38 &
    {0x00, 0x05, 0x03, 0x00, 0x00}, // 39 '
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // 40 (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // 41 )
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // 42 *
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // 43 +
    {0x00, 0x50, 0x30, 0x00, 0x00}, // 44 ,
    {0x08, 0x08, 0x08, 0x08, 0x08}, // 45 -
    {0x00, 0x60, 0x60, 0x00, 0x00}, // 46 .
    {0x20, 0x10, 0x08, 0x04, 0x02}, // 47 /
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 48 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 49 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 50 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 51 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 52 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 53 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 54 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 55 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 56 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 57 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, // 58 :
    {0x00, 0x56, 0x36, 0x00, 0x00}, // 59 ;
    {0x08, 0x14, 0x22, 0x41, 0x00}, // 60 <
    {0x14, 0x14, 0x14, 0x14, 0x14}, // 61 =
    {0x00, 0x41, 0x22, 0x14, 0x08}, // 62 >
    {0x02, 0x01, 0x51, 0x09, 0x06}, // 63 ?
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // 64 @
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // 65 A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 66 B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 67 C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // 68 D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 69 E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // 70 F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // 71 G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 72 H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 73 I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 74 J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 75 K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 76 L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // 77 M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 78 N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 79 O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 80 P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 81 Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 82 R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 83 S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // 84 T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 85 U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 86 V
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // 87 W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 88 X
    {0x07, 0x08, 0x70, 0x08, 0x07}, // 89 Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // 90 Z
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // 91 [
    {0x02, 0x04, 0x08, 0x10, 0x20}, // 92 /
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // 93 ]
    {0x04, 0x02, 0x01, 0x02, 0x04}, // 94 ^
    {0x40, 0x40, 0x40, 0x40, 0x40}, // 95 _
    {0x00, 0x01, 0x02, 0x04, 0x00}, // 96 `
    {0x20, 0x54, 0x54, 0x54, 0x78}, // 97 a
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // 98 b
    {0x38, 0x44, 0x44, 0x44, 0x20}, // 99 c
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // 100 d
    {0x38, 0x54, 0x54, 0x54, 0x18}, // 101 e
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // 102 f
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, // 103 g
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // 104 h
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // 105 i
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // 106 j
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // 107 k
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // 108 l
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // 109 m
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // 110 n
    {0x38, 0x44, 0x44, 0x44, 0x38}, // 111 o
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // 112 p
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // 113 q
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // 114 r
    {0x48, 0x54, 0x54, 0x54, 0x20}, // 115 s
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // 116 t
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // 117 u
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // 118 v
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // 119 w
    {0x44, 0x28, 0x10, 0x28, 0x44}, // 120 x
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // 121 y
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // 122 z
    {0x00, 0x08, 0x36, 0x41, 0x00}, // 123 {
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // 124 |
    {0x00, 0x41, 0x36, 0x08, 0x00}, // 125 }
    {0x10, 0x08, 0x08, 0x10, 0x08}, // 126 ~
    {0x00, 0x06, 0x09, 0x09, 0x06}, // 127
};

static esp_err_t i2c_write(uint8_t control, const uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DISPLAY_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, control, true);
    if (len > 0) {
        i2c_master_write(cmd, (uint8_t *)data, len, true);
    }
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(DISPLAY_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t ssd1306_cmd(uint8_t cmd);

static esp_err_t ssd1306_cmd_with_retry(uint8_t cmd, int max_retries)
{
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < max_retries; i++) {
        ret = ssd1306_cmd(cmd);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        if (i < max_retries - 1) {
            vTaskDelay(pdMS_TO_TICKS(10)); // Small delay before retry
        }
    }
    return ret;
}

static esp_err_t ssd1306_cmd(uint8_t cmd)
{
    return i2c_write(0x00, &cmd, 1);
}

static esp_err_t ssd1306_data(const uint8_t *data, size_t len)
{
    return i2c_write(0x40, data, len);
}

static void ssd1306_set_cursor(uint8_t col, uint8_t page)
{
    ssd1306_cmd(0xB0 | (page & 0x07));
    ssd1306_cmd(0x00 | (col & 0x0F));
    ssd1306_cmd(0x10 | ((col >> 4) & 0x0F));
}

static void ssd1306_clear(void)
{
    uint8_t zeros[SSD1306_WIDTH];
    memset(zeros, 0x00, sizeof(zeros));
    for (uint8_t page = 0; page < SSD1306_PAGES; page++) {
        ssd1306_set_cursor(0, page);
        ssd1306_data(zeros, sizeof(zeros));
    }
}

/* Clear only a range of pages (e.g. 2 pages per line) to avoid full-screen blink */
static void ssd1306_clear_pages(uint8_t start_page, uint8_t end_page)
{
    if (start_page > end_page || end_page >= SSD1306_PAGES) {
        return;
    }
    uint8_t zeros[SSD1306_WIDTH];
    memset(zeros, 0x00, sizeof(zeros));
    for (uint8_t page = start_page; page <= end_page; page++) {
        ssd1306_set_cursor(0, page);
        ssd1306_data(zeros, sizeof(zeros));
    }
}

static void ssd1306_draw_text(uint8_t col, uint8_t page, const char *text)
{
    if (!text) {
        return;
    }
    ssd1306_set_cursor(col, page);
    while (*text && col < SSD1306_WIDTH) {
        char c = *text++;
        if (c < 32 || c > 127) {
            c = '?';
        }
        const uint8_t *glyph = font5x7[c - 32];
        ssd1306_data(glyph, 5);
        uint8_t spacer = 0x00;
        ssd1306_data(&spacer, 1);
        col += 6;
    }
}

esp_err_t display_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = DISPLAY_SDA_GPIO,
        .scl_io_num = DISPLAY_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
        .clk_flags = 0,
    };
    esp_err_t ret = i2c_param_config(DISPLAY_I2C_PORT, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2C parameters: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = i2c_driver_install(DISPLAY_I2C_PORT, conf.mode, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install I2C driver: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Give I2C bus time to stabilize after driver install
    vTaskDelay(pdMS_TO_TICKS(100));

    // Basic SSD1306 init sequence with retry logic
    ret = ssd1306_cmd_with_retry(0xAE, 3); // Display off
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to communicate with SSD1306 at address 0x%02X (SDA=%d SCL=%d): %s", 
                 DISPLAY_I2C_ADDR, DISPLAY_SDA_GPIO, DISPLAY_SCL_GPIO, esp_err_to_name(ret));
        ESP_LOGW(TAG, "Display may not be connected. Check wiring and power.");
        i2c_driver_delete(DISPLAY_I2C_PORT);
        return ret;
    }
    // Continue with initialization sequence (use retry for critical commands)
    if ((ret = ssd1306_cmd_with_retry(0x20, 3)) != ESP_OK) goto fail; // Set memory addressing mode
    if ((ret = ssd1306_cmd(0x00)) != ESP_OK) goto fail; // Horizontal addressing
    if ((ret = ssd1306_cmd(0xB0)) != ESP_OK) goto fail; // Set page start
    if ((ret = ssd1306_cmd(0xC8)) != ESP_OK) goto fail; // COM scan direction
    if ((ret = ssd1306_cmd(0x00)) != ESP_OK) goto fail; // Low column
    if ((ret = ssd1306_cmd(0x10)) != ESP_OK) goto fail; // High column
    if ((ret = ssd1306_cmd(0x40)) != ESP_OK) goto fail; // Start line
    if ((ret = ssd1306_cmd(0x81)) != ESP_OK) goto fail; // Contrast
    if ((ret = ssd1306_cmd(0x7F)) != ESP_OK) goto fail;
    if ((ret = ssd1306_cmd(0xA1)) != ESP_OK) goto fail; // Segment remap
    if ((ret = ssd1306_cmd(0xA6)) != ESP_OK) goto fail; // Normal display
    if ((ret = ssd1306_cmd(0xA8)) != ESP_OK) goto fail; // Multiplex
    if ((ret = ssd1306_cmd(0x3F)) != ESP_OK) goto fail;
    if ((ret = ssd1306_cmd(0xA4)) != ESP_OK) goto fail; // Display follows RAM
    if ((ret = ssd1306_cmd(0xD3)) != ESP_OK) goto fail; // Display offset
    if ((ret = ssd1306_cmd(0x00)) != ESP_OK) goto fail;
    if ((ret = ssd1306_cmd(0xD5)) != ESP_OK) goto fail; // Display clock
    if ((ret = ssd1306_cmd(0x80)) != ESP_OK) goto fail;
    if ((ret = ssd1306_cmd(0xD9)) != ESP_OK) goto fail; // Pre-charge
    if ((ret = ssd1306_cmd(0xF1)) != ESP_OK) goto fail;
    if ((ret = ssd1306_cmd(0xDA)) != ESP_OK) goto fail; // COM pins
    if ((ret = ssd1306_cmd(0x12)) != ESP_OK) goto fail;
    if ((ret = ssd1306_cmd(0xDB)) != ESP_OK) goto fail; // VCOM detect
    if ((ret = ssd1306_cmd(0x40)) != ESP_OK) goto fail;
    if ((ret = ssd1306_cmd(0x8D)) != ESP_OK) goto fail; // Charge pump
    if ((ret = ssd1306_cmd(0x14)) != ESP_OK) goto fail;
    if ((ret = ssd1306_cmd(0xAF)) != ESP_OK) goto fail; // Display on

    ssd1306_clear();
    s_display_ready = true;
    ESP_LOGI(TAG, "SSD1306 init ok (SDA=%d SCL=%d addr=0x%02X)",
             DISPLAY_SDA_GPIO, DISPLAY_SCL_GPIO, DISPLAY_I2C_ADDR);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "SSD1306 initialization failed: %s", esp_err_to_name(ret));
    i2c_driver_delete(DISPLAY_I2C_PORT);
    return ret;
}

void display_clear(void)
{
    if (!s_display_ready) {
        return;
    }
    s_status_count = 0;
    ssd1306_clear();
}

void display_show_status(const char *line1, const char *line2)
{
    if (!s_display_ready) {
        return;
    }
    push_status_line(line1);
    push_status_line(line2);
    ssd1306_clear();
    for (int i = 0; i < s_status_count; i++) {
        ssd1306_draw_text(0, (uint8_t)i * 2, s_status_lines[i]);
    }
}

void display_show_3lines(const char *line1, const char *line2, const char *line3)
{
    if (!s_display_ready) {
        return;
    }
    /* Clear and draw each line in turn to avoid full-screen blink */
    ssd1306_clear_pages(0, 1);
    if (line1) {
        ssd1306_draw_text(0, 0, line1);
    }
    ssd1306_clear_pages(2, 3);
    if (line2) {
        ssd1306_draw_text(0, 2, line2);
    }
    ssd1306_clear_pages(4, 5);
    if (line3) {
        ssd1306_draw_text(0, 4, line3);
    }
}

void display_update_3rd_line(const char *line3)
{
    if (!s_display_ready) {
        return;
    }
    ssd1306_clear_pages(4, 5);
    if (line3) {
        ssd1306_draw_text(0, 4, line3);
    }
}
