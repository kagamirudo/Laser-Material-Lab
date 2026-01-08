#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"

#include "wifi.h"
#include "server.h"

static const char *TAG = "LASER_ADC";

// Pin mapping (ESP32-S3)
#define LASER_GPIO   5    // PWM output to laser
#define ADC_GPIO     4    // ADC input from photodiode/sensor (ADC1_CH3 on S3)

// LEDC config
#define LASER_LEDC_MODE   LEDC_LOW_SPEED_MODE
#define LASER_LEDC_TIMER  LEDC_TIMER_0
#define LASER_LEDC_CH     LEDC_CHANNEL_0
#define LASER_LEDC_FREQ   5000       // 5 kHz
#define LASER_LEDC_RES    LEDC_TIMER_8_BIT
#define LASER_DUTY_FULL   ((1 << LASER_LEDC_RES) - 1)  // 255

// CSV logging config
#define MAX_SAMPLES       10000
#define SAMPLES_TO_AVG    16
#define CSV_FILENAME_MAX  64

static adc_oneshot_unit_handle_t adc_handle;
static adc_channel_t adc_chan;
static adc_unit_t adc_unit;

// Shared state for web API
static int s_current_adc_value = 0;
static FILE *s_csv_file = NULL;
static int s_sample_count = 0;
static char s_csv_filename[CSV_FILENAME_MAX] = {0};
static bool s_csv_logging_active = false;

static void laser_init_full_on(void) {
    ledc_timer_config_t tcfg = {
        .speed_mode      = LASER_LEDC_MODE,
        .duty_resolution = LASER_LEDC_RES,
        .timer_num       = LASER_LEDC_TIMER,
        .freq_hz         = LASER_LEDC_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ccfg = {
        .speed_mode = LASER_LEDC_MODE,
        .channel    = LASER_LEDC_CH,
        .timer_sel  = LASER_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = LASER_GPIO,
        .duty       = LASER_DUTY_FULL,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
    ESP_ERROR_CHECK(ledc_update_duty(LASER_LEDC_MODE, LASER_LEDC_CH));
    ESP_LOGI(TAG, "Laser PWM set full on @ GPIO%d", LASER_GPIO);
}

static void adc_init_gpio4(void) {
    // Map GPIO to ADC unit/channel dynamically
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(ADC_GPIO, &adc_unit, &adc_chan));

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = adc_unit,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten    = ADC_ATTEN_DB_12,  // up to ~3.3V
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, adc_chan, &chan_cfg));
    ESP_LOGI(TAG, "ADC initialized on GPIO%d (unit %d, channel %d)",
             ADC_GPIO, adc_unit, adc_chan);
}

static void create_csv_file(void) {
    // Generate filename with timestamp
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    snprintf(s_csv_filename, sizeof(s_csv_filename),
             "/spiffs/adc_log_%04d%02d%02d_%02d%02d%02d.csv",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    s_csv_file = fopen(s_csv_filename, "w");
    if (s_csv_file == NULL) {
        ESP_LOGE(TAG, "Failed to create CSV file: %s", s_csv_filename);
        s_csv_logging_active = false;
        return;
    }

    // Write CSV header
    fprintf(s_csv_file, "timestamp_ms,adc_raw\n");
    fflush(s_csv_file);
    
    s_sample_count = 0;
    s_csv_logging_active = true;
    ESP_LOGI(TAG, "CSV logging started: %s", s_csv_filename);
}

static void adc_task(void *pvParameters) {
    TickType_t start_time = xTaskGetTickCount();
    
    while (1) {
        // Take multiple samples and average
        int32_t sum = 0;
        int valid_samples = 0;
        for (int i = 0; i < SAMPLES_TO_AVG; i++) {
            int raw_single = 0;
            esp_err_t err = adc_oneshot_read(adc_handle, adc_chan, &raw_single);
            if (err == ESP_OK) {
                sum += raw_single;
                valid_samples++;
            }
        }

        if (valid_samples > 0) {
            int avg_raw = (int)(sum / valid_samples);
            s_current_adc_value = avg_raw;

            // Log to CSV if active and not at max samples
            if (s_csv_logging_active && s_sample_count < MAX_SAMPLES && s_csv_file != NULL) {
                TickType_t elapsed = xTaskGetTickCount() - start_time;
                uint32_t timestamp_ms = (uint32_t)(elapsed * portTICK_PERIOD_MS);
                
                fprintf(s_csv_file, "%lu,%d\n", (unsigned long)timestamp_ms, avg_raw);
                fflush(s_csv_file);
                
                s_sample_count++;
                
                if (s_sample_count >= MAX_SAMPLES) {
                    ESP_LOGI(TAG, "Reached max samples (%d), stopping CSV logging", MAX_SAMPLES);
                    fclose(s_csv_file);
                    s_csv_file = NULL;
                    s_csv_logging_active = false;
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Stub implementations for old LED effect handlers (not used in laser/ADC mode)
void stop_effect(void) {
    // No-op for laser/ADC mode
}

void set_rgb_color(uint32_t red, uint32_t green, uint32_t blue) {
    // No-op for laser/ADC mode
}

void rainbow_effect_task(void *pvParameters) {
    vTaskDelete(NULL);
}

void pulse_effect_task(void *pvParameters) {
    vTaskDelete(NULL);
}

void fast_cycle_effect_task(void *pvParameters) {
    vTaskDelete(NULL);
}

void gradient_effect_task(void *pvParameters) {
    vTaskDelete(NULL);
}

TaskHandle_t effect_task_handle = NULL;
bool effect_running = false;

// API functions for server.c
int get_current_adc_value(void) {
    return s_current_adc_value;
}

const char *get_csv_filename(void) {
    return s_csv_filename;
}

FILE *get_csv_file(void) {
    return s_csv_file;
}

bool is_csv_logging_active(void) {
    return s_csv_logging_active;
}

int get_sample_count(void) {
    return s_sample_count;
}

void app_main(void) {
    ESP_LOGI(TAG, "Laser ADC with Web Interface");

    // Initialize NVS (required for SPIFFS)
    ESP_ERROR_CHECK(nvs_flash_init());

    // Initialize SPIFFS for CSV storage (optional - continue without it if not configured)
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS not available (%s) - CSV logging disabled", esp_err_to_name(ret));
        ESP_LOGW(TAG, "To enable CSV logging, add a SPIFFS partition in partition table");
        // Continue without CSV logging - WiFi and ADC will still work
    } else {
        ESP_LOGI(TAG, "SPIFFS initialized");
    }

    // Initialize hardware
    laser_init_full_on();
    adc_init_gpio4();

    // Start ADC reading task
    xTaskCreate(adc_task, "adc_task", 4096, NULL, 5, NULL);

    // Start CSV logging (only if SPIFFS is available)
    if (ret == ESP_OK) {
        create_csv_file();
    }

    // Initialize WiFi AP (self-host)
    wifi_init_softap();
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Start web server
    httpd_handle_t server = start_webserver_http();
    if (server == NULL) {
        ESP_LOGE(TAG, "Failed to start web server");
        return;
    }

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);
    ESP_LOGI(TAG, "Web server started. Connect to WiFi: %s (password: %s)",
             WIFI_AP_SSID, WIFI_AP_PASSWORD);
    ESP_LOGI(TAG, "IP address: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "Open browser to: http://" IPSTR, IP2STR(&ip_info.ip));

    // Keep running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
