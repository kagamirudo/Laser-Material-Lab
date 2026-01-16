#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/ledc.h"
#include "driver/gptimer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

#include "wifi.h"
#include "server.h"

static const char *TAG = "LASER_ADC";

// 4000 samples per second -> 4 per ms
// 20 million samples per run
// test: the rate

// Global SPIFFS config so we can re-register after formatting
static esp_vfs_spiffs_conf_t s_spiffs_conf = {
    .base_path = "/spiffs",
    .partition_label = NULL,
    .max_files = 5,
    .format_if_mount_failed = true,
};

static bool s_spiffs_mounted = false;

#define WIFI 1
#define HOST 2
// #define MODE WIFI
#define MODE HOST

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
#define MAX_SAMPLES       100000
#define CSV_FILENAME_MAX  64
#define SAMPLE_RATE_HZ    4000           // Target: 4000 samples per second
#define SAMPLE_INTERVAL_US (1000000 / SAMPLE_RATE_HZ)  // 250 microseconds per sample
#define BUFFER_SIZE       2000           // Buffer ~500ms of data (2000 samples @ 4000Hz)
#define FLUSH_INTERVAL    1000           // Flush buffer every 1000 samples
#define UI_UPDATE_INTERVAL 100           // Update sample count for UI every 100 samples
#define SAMPLE_QUEUE_SIZE 2000           // Queue size: buffer ~500ms of samples

static adc_oneshot_unit_handle_t adc_handle;
static adc_channel_t adc_chan;
static adc_unit_t adc_unit;

// RTOS primitives
static gptimer_handle_t s_timer_handle = NULL;
static QueueHandle_t s_sample_queue = NULL;
static SemaphoreHandle_t s_adc_value_mutex = NULL;
static SemaphoreHandle_t s_file_ops_mutex = NULL;  // Protects SPIFFS file operations

// Sample structure for queue
typedef struct {
    uint64_t timestamp_us;  // Timestamp in microseconds (absolute time)
    int adc_value;          // Raw ADC reading
} adc_sample_t;

// Sample buffer structure for CSV logging
typedef struct {
    uint32_t timestamp_us;  // Timestamp in microseconds from start
    int adc_value;          // Raw ADC reading
} sample_t;

// Shared state for web API
static int s_current_adc_value = 0;
static FILE *s_csv_file = NULL;
static int s_sample_count = 0;
static char s_csv_filename[CSV_FILENAME_MAX] = {0};
static bool s_csv_logging_active = false;

// Sample buffer for high-speed logging
static sample_t s_sample_buffer[BUFFER_SIZE];
static int s_buffer_idx = 0;
static uint64_t s_logging_start_time_us = 0;  // Start time in microseconds

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

// GPTimer ISR callback: reads ADC and enqueues sample
static bool IRAM_ATTR timer_isr_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *ed, void *user_data) {
    BaseType_t must_yield = pdFALSE;
    adc_sample_t sample;
    
    // Read ADC (oneshot ADC is safe to call from ISR)
    int raw_value = 0;
    esp_err_t err = adc_oneshot_read(adc_handle, adc_chan, &raw_value);
    
    if (err == ESP_OK) {
        // Get timestamp
        sample.timestamp_us = esp_timer_get_time();
        sample.adc_value = raw_value;
        
        // Send to queue from ISR (non-blocking, drops if queue full)
        if (s_sample_queue != NULL) {
            BaseType_t result = xQueueSendFromISR(s_sample_queue, &sample, &must_yield);
            // If queue is full, we drop the sample (better than blocking)
            // The processing task should keep up, but if it doesn't, we lose samples
            // rather than blocking the ISR
        }
    }
    
    // Return whether we need to yield
    return (must_yield == pdTRUE);
}

// Initialize GPTimer for 4000 Hz sampling
static void init_sampling_timer(void) {
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,  // 1 MHz resolution (1 microsecond)
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &s_timer_handle));
    
    // Set alarm callback
    gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_isr_callback,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_timer_handle, &cbs, NULL));
    
    // Enable timer
    ESP_ERROR_CHECK(gptimer_enable(s_timer_handle));
    
    // Configure alarm for 4000 Hz (250µs period)
    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,
        .alarm_count = SAMPLE_INTERVAL_US,  // 250 microseconds
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(s_timer_handle, &alarm_config));
    
    ESP_LOGI(TAG, "GPTimer initialized for %d Hz sampling", SAMPLE_RATE_HZ);
}

// Start the sampling timer (exported for server.c)
void start_sampling_timer(void) {
    if (s_timer_handle != NULL) {
        ESP_ERROR_CHECK(gptimer_start(s_timer_handle));
        ESP_LOGI(TAG, "Sampling timer started");
    }
}

// Stop the sampling timer (exported for server.c)
void stop_sampling_timer(void) {
    if (s_timer_handle != NULL) {
        ESP_ERROR_CHECK(gptimer_stop(s_timer_handle));
        ESP_LOGI(TAG, "Sampling timer stopped");
    }
}

static void cleanup_old_csv_files(void) {
    // Use VFS-compatible directory operations
    struct dirent *entry;
    DIR *dir = opendir("/spiffs");
    if (dir == NULL) {
        ESP_LOGW(TAG, "Failed to open SPIFFS directory for cleanup");
        return;
    }
    
    int deleted_count = 0;
    while ((entry = readdir(dir)) != NULL) {
        // Check if file matches CSV pattern
        if (strstr(entry->d_name, "adc_log_") != NULL && 
            strstr(entry->d_name, ".csv") != NULL) {
            // Use larger buffer to avoid truncation warning
            char filepath[256];
            int len = snprintf(filepath, sizeof(filepath), "/spiffs/%s", entry->d_name);
            if (len > 0 && len < (int)sizeof(filepath)) {
                if (remove(filepath) == 0) {
                    deleted_count++;
                    ESP_LOGI(TAG, "Deleted old CSV file: %s", filepath);
                } else {
                    ESP_LOGW(TAG, "Failed to delete CSV file: %s", filepath);
                }
            } else {
                ESP_LOGW(TAG, "Filename too long, skipping: %s", entry->d_name);
            }
        }
    }
    closedir(dir);
    
    if (deleted_count > 0) {
        ESP_LOGI(TAG, "Cleaned up %d old CSV file(s)", deleted_count);
    }
}

static void create_csv_file(void) {
    // Clean up any existing CSV files first
    cleanup_old_csv_files();
    
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

// Flush buffered samples to CSV file
static void flush_sample_buffer(void) {
    if (s_csv_file == NULL || s_buffer_idx == 0) {
        return;
    }
    
    // Take file operations mutex to prevent conflicts with CSV download
    if (s_file_ops_mutex != NULL) {
        if (xSemaphoreTake(s_file_ops_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to take file ops mutex for flush");
            return;
        }
    }
    
    // Write all buffered samples to file
    for (int i = 0; i < s_buffer_idx; i++) {
        // Convert microseconds to milliseconds for CSV
        uint32_t timestamp_ms = s_sample_buffer[i].timestamp_us / 1000;
        fprintf(s_csv_file, "%lu,%d\n", (unsigned long)timestamp_ms, s_sample_buffer[i].adc_value);
    }
    fflush(s_csv_file);
    
    ESP_LOGD(TAG, "Flushed %d samples to CSV (total: %d)", s_buffer_idx, s_sample_count);
    s_buffer_idx = 0;
    
    // Release mutex
    if (s_file_ops_mutex != NULL) {
        xSemaphoreGive(s_file_ops_mutex);
    }
}

// ADC processing task: processes samples from queue
static void adc_task(void *pvParameters) {
    adc_sample_t sample;
    int samples_since_flush = 0;
    
    ESP_LOGI(TAG, "ADC processing task started");
    
    while (1) {
        // Wait for sample from queue (blocking, allows other tasks to run)
        if (xQueueReceive(s_sample_queue, &sample, portMAX_DELAY) == pdTRUE) {
            // Update current value for web UI (protected by mutex)
            if (s_adc_value_mutex != NULL) {
                if (xSemaphoreTake(s_adc_value_mutex, portMAX_DELAY) == pdTRUE) {
                    s_current_adc_value = sample.adc_value;
                    xSemaphoreGive(s_adc_value_mutex);
                }
            } else {
                s_current_adc_value = sample.adc_value;
            }
            
            // If logging is active, buffer the sample
            if (s_csv_logging_active && s_sample_count < MAX_SAMPLES && s_csv_file != NULL) {
                // Set start time on first sample
                if (s_logging_start_time_us == 0) {
                    s_logging_start_time_us = sample.timestamp_us;
                }
                
                // Calculate timestamp relative to start (in microseconds)
                uint32_t elapsed_us = (uint32_t)(sample.timestamp_us - s_logging_start_time_us);
                
                // Store sample in buffer
                if (s_buffer_idx < BUFFER_SIZE) {
                    s_sample_buffer[s_buffer_idx].timestamp_us = elapsed_us;
                    s_sample_buffer[s_buffer_idx].adc_value = sample.adc_value;
                    s_buffer_idx++;
                } else {
                    // Buffer full, flush before adding new sample
                    flush_sample_buffer();
                    s_sample_buffer[0].timestamp_us = elapsed_us;
                    s_sample_buffer[0].adc_value = sample.adc_value;
                    s_buffer_idx = 1;
                }
                
                s_sample_count++;
                samples_since_flush++;
                
                // Flush buffer periodically to avoid losing data
                if (samples_since_flush >= FLUSH_INTERVAL) {
                    flush_sample_buffer();
                    samples_since_flush = 0;
                }
                
                // Check if we've reached max samples
                if (s_sample_count >= MAX_SAMPLES) {
                    // Take file operations mutex before closing
                    if (s_file_ops_mutex != NULL) {
                        xSemaphoreTake(s_file_ops_mutex, portMAX_DELAY);
                    }
                    
                    // Flush any remaining samples (without mutex since we already have it)
                    if (s_buffer_idx > 0 && s_csv_file != NULL) {
                        for (int i = 0; i < s_buffer_idx; i++) {
                            uint32_t timestamp_ms = s_sample_buffer[i].timestamp_us / 1000;
                            fprintf(s_csv_file, "%lu,%d\n", (unsigned long)timestamp_ms, s_sample_buffer[i].adc_value);
                        }
                        fflush(s_csv_file);
                        s_buffer_idx = 0;
                    }
                    
                    ESP_LOGI(TAG, "Reached max samples (%d), stopping CSV logging", MAX_SAMPLES);
                    fclose(s_csv_file);
                    s_csv_file = NULL;
                    s_csv_logging_active = false;
                    s_logging_start_time_us = 0;  // Reset for next session
                    
                    // Release mutex
                    if (s_file_ops_mutex != NULL) {
                        xSemaphoreGive(s_file_ops_mutex);
                    }
                    // Timer continues running for web UI (don't stop it)
                }
            } else if (!s_csv_logging_active) {
                // Reset start time when logging is inactive
                s_logging_start_time_us = 0;
                s_buffer_idx = 0;
            }
        }
    }
}

// API functions for server.c
int get_current_adc_value(void) {
    int value = 0;
    if (s_adc_value_mutex != NULL) {
        if (xSemaphoreTake(s_adc_value_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            value = s_current_adc_value;
            xSemaphoreGive(s_adc_value_mutex);
        }
    } else {
        value = s_current_adc_value;
    }
    return value;
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

// Get file operations mutex (for server.c to protect CSV download)
SemaphoreHandle_t get_file_ops_mutex(void) {
    return s_file_ops_mutex;
}

// Get SPIFFS storage info (total, used, free in bytes)
void get_spiffs_storage_info(size_t *total_bytes, size_t *used_bytes) {
    *total_bytes = 0;
    *used_bytes = 0;
    
    // Use esp_spiffs_info to get partition info
    size_t total = 0, used = 0;
    esp_err_t ret = esp_spiffs_info(NULL, &total, &used);
    if (ret == ESP_OK) {
        *total_bytes = total;
        *used_bytes = used;
    }
}

// Start CSV logging manually
void start_csv_logging(void) {
    if (s_csv_logging_active) {
        ESP_LOGW(TAG, "CSV logging already active");
        return;
    }
    
    // Reset sample count and timing for new session
    s_sample_count = 0;
    s_buffer_idx = 0;
    s_logging_start_time_us = 0;
    create_csv_file();
    
    // Timer is already running for web UI, no need to start it again
    // Just enable logging flag
}

// Stop CSV logging manually
void stop_csv_logging(void) {
    if (s_csv_logging_active && s_csv_file != NULL) {
        // Take file operations mutex
        if (s_file_ops_mutex != NULL) {
            xSemaphoreTake(s_file_ops_mutex, portMAX_DELAY);
        }
        
        // Flush any remaining buffered samples before closing
        if (s_buffer_idx > 0) {
            // Write directly without mutex (we already have it)
            for (int i = 0; i < s_buffer_idx; i++) {
                uint32_t timestamp_ms = s_sample_buffer[i].timestamp_us / 1000;
                fprintf(s_csv_file, "%lu,%d\n", (unsigned long)timestamp_ms, s_sample_buffer[i].adc_value);
            }
            fflush(s_csv_file);
            s_buffer_idx = 0;
        }
        
        fclose(s_csv_file);
        s_csv_file = NULL;
        s_csv_logging_active = false;
        s_logging_start_time_us = 0;
        
        // Release mutex
        if (s_file_ops_mutex != NULL) {
            xSemaphoreGive(s_file_ops_mutex);
        }
        
        ESP_LOGI(TAG, "CSV logging stopped manually. Total samples: %d", s_sample_count);
        
        // Timer continues running for web UI (don't stop it)
    }
}

// Clear all files from SPIFFS storage
void clear_spiffs_storage(void) {
    // First, close any open CSV file
    if (s_csv_file != NULL) {
        fclose(s_csv_file);
        s_csv_file = NULL;
        s_csv_logging_active = false;
        ESP_LOGI(TAG, "Closed open CSV file before clearing storage");
    }
    
    if (!s_spiffs_mounted) {
        ESP_LOGW(TAG, "SPIFFS not mounted, nothing to clear");
        return;
    }

    // Get storage info before clearing
    size_t total_before = 0, used_before = 0;
    get_spiffs_storage_info(&total_before, &used_before);
    ESP_LOGI(TAG, "Storage BEFORE clear: total=%zu bytes, used=%zu bytes, free=%zu bytes",
             total_before, used_before, (total_before > used_before) ? (total_before - used_before) : 0);
    
    // Unregister SPIFFS before formatting
    ESP_LOGI(TAG, "Unregistering SPIFFS before format...");
    esp_err_t unreg_ret = esp_vfs_spiffs_unregister(s_spiffs_conf.partition_label);
    if (unreg_ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS unregister failed: %s", esp_err_to_name(unreg_ret));
    } else {
        s_spiffs_mounted = false;
    }

    // Format SPIFFS to truly clear all space (this will erase everything)
    ESP_LOGI(TAG, "Formatting SPIFFS partition to free all space...");
    esp_err_t format_ret = esp_spiffs_format(s_spiffs_conf.partition_label);
    if (format_ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS formatted successfully");
    } else {
        ESP_LOGW(TAG, "SPIFFS format failed: %s", esp_err_to_name(format_ret));
    }

    // Re-register/mount SPIFFS so logging can continue
    ESP_LOGI(TAG, "Re-registering SPIFFS after format...");
    esp_err_t reg_ret = esp_vfs_spiffs_register(&s_spiffs_conf);
    if (reg_ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS re-register failed: %s", esp_err_to_name(reg_ret));
        s_spiffs_mounted = false;
    } else {
        s_spiffs_mounted = true;
        ESP_LOGI(TAG, "SPIFFS re-registered after format");
    }
    
    // Get storage info after clearing
    size_t total_after = 0, used_after = 0;
    get_spiffs_storage_info(&total_after, &used_after);
    ESP_LOGI(TAG, "Storage AFTER clear: total=%zu bytes, used=%zu bytes, free=%zu bytes",
             total_after, used_after, (total_after > used_after) ? (total_after - used_after) : 0);
    
    ESP_LOGI(TAG, "Clear storage complete (partition formatted)");
}

void app_main(void) {
    ESP_LOGI(TAG, "Laser ADC with Web Interface");

    // Initialize NVS (required for SPIFFS)
    ESP_ERROR_CHECK(nvs_flash_init());

    // Initialize SPIFFS for CSV storage (optional - continue without it if not configured)
    esp_err_t ret = esp_vfs_spiffs_register(&s_spiffs_conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS not available (%s) - CSV logging disabled", esp_err_to_name(ret));
        ESP_LOGW(TAG, "To enable CSV logging, add a SPIFFS partition in partition table");
        // Continue without CSV logging - WiFi and ADC will still work
    } else {
        ESP_LOGI(TAG, "SPIFFS initialized");
        s_spiffs_mounted = true;
    }

    // Initialize hardware
    laser_init_full_on();
    adc_init_gpio4();

    // Create RTOS primitives
    // Create sample queue for ISR to task communication
    s_sample_queue = xQueueCreate(SAMPLE_QUEUE_SIZE, sizeof(adc_sample_t));
    if (s_sample_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create sample queue");
        return;
    }
    
    // Create mutex for ADC value protection
    s_adc_value_mutex = xSemaphoreCreateMutex();
    if (s_adc_value_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create ADC value mutex");
        return;
    }
    
    // Create mutex for file operations (protects SPIFFS I/O)
    s_file_ops_mutex = xSemaphoreCreateMutex();
    if (s_file_ops_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create file operations mutex");
        return;
    }
    
    // Initialize hardware timer for precise 4000 Hz sampling
    init_sampling_timer();
    
    // Start ADC processing task (processes samples from queue)
    // Pinned to CPU 1, priority 5 (high priority for real-time processing)
    xTaskCreatePinnedToCore(adc_task, "adc_task", 4096, NULL, 5, NULL, 1);
    
    // Start timer immediately for continuous sampling (web UI always needs latest value)
    start_sampling_timer();
    
    ESP_LOGI(TAG, "RTOS architecture initialized: hardware timer + queue-based processing");

    // Don't start CSV logging automatically - wait for user to press start button
    // CSV logging will be started via API call

    // Initialize WiFi AP (self-host)
#if MODE == WIFI
    wifi_init_sta();
#elif MODE == HOST
    wifi_init_softap();
#else
#error "MODE must be WIFI or HOST"
#endif
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Start web server
    httpd_handle_t server = start_webserver_http();
    if (server == NULL) {
        ESP_LOGE(TAG, "Failed to start web server");
        return;
    }

    esp_netif_ip_info_t ip_info;

#if MODE == WIFI
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"),
                          &ip_info);
    ESP_LOGI(TAG, "Web server started (STA). Connected to: %s", WIFI_STA_SSID);

#elif MODE == HOST
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"),
                          &ip_info);
    ESP_LOGI(TAG, "Web server started (AP). SSID: %s Password: %s",
             WIFI_AP_SSID, WIFI_AP_PASSWORD);
#endif

    ESP_LOGI(TAG, "IP address: " IPSTR, IP2STR(&ip_info.ip));

    // Keep running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
