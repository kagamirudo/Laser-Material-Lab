#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "driver/ledc.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_oneshot.h"  // Needed for channel mapping
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_spiffs.h"

#include "wifi.h"
#include "server.h"
#include "sdcard.h"
#include "display.h"

static const char *TAG = "LASER_ADC";

// 4000 samples per second -> 4 per ms

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

// ADC sampling config
#define SAMPLE_RATE_HZ    1000           // Target: 1000 samples per second (minimum: ~611 Hz for ESP32-S3)
#define ADC_MIN_FREQ_HZ   611            // Minimum sampling frequency for ESP32-S3 continuous ADC
#define ADC_CONTINUOUS_BUF_SIZE 4096     // Buffer for continuous ADC (must be multiple of SOC_ADC_DIGI_RESULT_BYTES)
#define ADC_READ_TIMEOUT_MS 100          // Timeout for reading from continuous ADC

// SPIFFS config
#define SPIFFS_MOUNT_POINT "/spiffs"
#define CSV_FILE_PATH_MAX_LEN 64         // Maximum length for CSV filename
#define CSV_QUEUE_SIZE 3000              // Queue size for ADC samples
#define SAMPLE_LIMIT 10000               // Auto-stop after this many samples

// CSV write buffer config
#define CSV_WRITE_BATCH_SIZE 500         // Write CSV in batches to reduce flash wear
#define CSV_FLUSH_INTERVAL 5             // Flush every N batches (reduces flash wear)

static adc_continuous_handle_t adc_handle = NULL;
static adc_channel_t adc_chan;
static adc_unit_t adc_unit;
static uint32_t s_actual_sample_rate_hz = SAMPLE_RATE_HZ;  // Actual rate used (may be clamped to minimum)

// RTOS primitives
static SemaphoreHandle_t s_adc_value_mutex = NULL;
static SemaphoreHandle_t s_csv_file_mutex = NULL;  // Mutex for CSV file access

// CSV sample structure for queue
typedef struct {
    int adc_value;
    uint64_t timestamp_us;
} csv_sample_t;

static QueueHandle_t s_csv_queue = NULL;

// Shared state for web API
static int s_current_adc_value = 0;
static int s_sample_count = 0;
static bool s_adc_stop_requested = false;  // Flag to request ADC stop from ADC task
static bool s_adc_running = false;  // Track if ADC is currently running
static bool s_csv_logging_enabled = false;  // Track if CSV logging is enabled

// Timing for rate calculation
static uint64_t s_logging_start_time_us = 0;  // Start time in microseconds
static uint64_t s_logging_stop_time_us = 0;   // Stop time in microseconds

// CSV file handle
static FILE *s_csv_file = NULL;
static char s_csv_file_path[CSV_FILE_PATH_MAX_LEN] = "/spiffs/data.csv";  // Dynamic CSV filename

// Sample index for consistent timestamps (reset on each logging session)
static uint32_t s_csv_sample_index = 0;

// Forward declarations (used before definitions)
void clear_spiffs_storage(void);
void stop_csv_logging(void);
static void generate_csv_filename(void);

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
    // Map GPIO to ADC unit/channel dynamically (using oneshot API to get channel info)
    // Note: We use oneshot API just to get channel mapping, then use continuous API
    adc_oneshot_unit_handle_t oneshot_handle;
    adc_oneshot_unit_init_cfg_t oneshot_unit_cfg = {
        .unit_id = ADC_UNIT_1,  // Try ADC1 first (GPIO4 is typically ADC1_CH3 on S3)
    };
    esp_err_t ret = adc_oneshot_new_unit(&oneshot_unit_cfg, &oneshot_handle);
    if (ret != ESP_OK) {
        // Try ADC2 if ADC1 fails
        oneshot_unit_cfg.unit_id = ADC_UNIT_2;
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&oneshot_unit_cfg, &oneshot_handle));
    }
    
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(ADC_GPIO, &adc_unit, &adc_chan));
    adc_oneshot_del_unit(oneshot_handle);  // Clean up oneshot handle, we only needed it for channel mapping

    // Configure continuous ADC handle
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = ADC_CONTINUOUS_BUF_SIZE,
        .conv_frame_size = ADC_CONTINUOUS_BUF_SIZE,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc_handle));

    // Configure ADC pattern (single channel)
    adc_digi_pattern_config_t adc_pattern = {
        .atten = ADC_ATTEN_DB_12,  // up to ~3.3V
        .channel = adc_chan,
        .unit = adc_unit,
        .bit_width = ADC_BITWIDTH_12,
    };

    // Validate and adjust sample frequency (ESP32-S3 minimum is ~611 Hz)
    s_actual_sample_rate_hz = SAMPLE_RATE_HZ;
    if (SAMPLE_RATE_HZ < ADC_MIN_FREQ_HZ) {
        ESP_LOGW(TAG, "Sample rate %d Hz is below minimum (%d Hz), using %d Hz instead", 
                 SAMPLE_RATE_HZ, ADC_MIN_FREQ_HZ, ADC_MIN_FREQ_HZ);
        s_actual_sample_rate_hz = ADC_MIN_FREQ_HZ;
    }
    
    // Configure continuous ADC
    adc_continuous_config_t cont_cfg = {
        .pattern_num = 1,
        .adc_pattern = &adc_pattern,
        .sample_freq_hz = s_actual_sample_rate_hz,
        .conv_mode = (adc_unit == ADC_UNIT_1) ? ADC_CONV_SINGLE_UNIT_1 : ADC_CONV_SINGLE_UNIT_2,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &cont_cfg));
    
    ESP_LOGI(TAG, "ADC continuous mode initialized on GPIO%d (unit %d, channel %d) @ %d Hz",
             ADC_GPIO, adc_unit, adc_chan, s_actual_sample_rate_hz);
}

// Start continuous ADC sampling (exported for server.c)
void start_sampling_timer(void) {
    if (adc_handle != NULL && !s_adc_running) {
        // Clear stop request flag first
        s_adc_stop_requested = false;
        esp_err_t ret = adc_continuous_start(adc_handle);
        if (ret == ESP_OK) {
            s_adc_running = true;
            ESP_LOGI(TAG, "ADC continuous sampling started @ %d Hz", s_actual_sample_rate_hz);
        } else if (ret == ESP_ERR_INVALID_STATE) {
            // ADC is already started, just update our flag
            s_adc_running = true;
            ESP_LOGI(TAG, "ADC continuous sampling already running @ %d Hz", s_actual_sample_rate_hz);
        } else {
            ESP_LOGE(TAG, "Failed to start ADC continuous sampling: %s", esp_err_to_name(ret));
        }
    } else if (s_adc_running) {
        ESP_LOGI(TAG, "ADC continuous sampling already running");
    }
}

// Request ADC stop (safe to call from any task)
void stop_sampling_timer(void) {
    s_adc_stop_requested = true;
    ESP_LOGI(TAG, "ADC stop requested");
}



// ADC processing task: reads samples from continuous ADC buffer
static void adc_task(void *pvParameters) {
    adc_continuous_data_t *parsed_samples = heap_caps_malloc(sizeof(adc_continuous_data_t) * 256, MALLOC_CAP_DEFAULT);
    if (parsed_samples == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer for parsed ADC samples");
        vTaskDelete(NULL);
        return;
    }
    
    uint32_t dropped_samples = 0;
    
    ESP_LOGI(TAG, "ADC processing task started");
    
    while (1) {
        if (adc_handle == NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // Check if ADC stop was requested (currently not used, but kept for future use)
        if (s_adc_stop_requested) {
            // Note: We don't actually stop ADC anymore to avoid mutex crashes
            // ADC keeps running for web UI, we just stop logging
            s_adc_stop_requested = false;
            ESP_LOGW(TAG, "ADC stop requested but ignored - ADC must stay running");
            continue;
        }
        
        // Skip reading if ADC is not running
        if (!s_adc_running) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // Read and parse samples directly from continuous ADC buffer
        uint32_t num_samples = 256;  // Maximum number of samples to parse
        esp_err_t parse_ret = adc_continuous_read_parse(adc_handle, parsed_samples, 256, &num_samples, ADC_READ_TIMEOUT_MS);
        
        if (parse_ret == ESP_OK && num_samples > 0) {
            // Process each parsed sample
            for (uint32_t i = 0; i < num_samples; i++) {
                if (!parsed_samples[i].valid) {
                    dropped_samples++;
                    continue;
                }
                
                int raw_value = (int)parsed_samples[i].raw_data;
                
                // Update current value for web UI (always update when ADC is running)
                // (protected by mutex)
                if (s_adc_value_mutex != NULL) {
                    if (xSemaphoreTake(s_adc_value_mutex, 0) == pdTRUE) {
                        s_current_adc_value = raw_value;
                        xSemaphoreGive(s_adc_value_mutex);
                    }
                } else {
                    s_current_adc_value = raw_value;
                }
                
                // Increment sample count for rate calculation
                s_sample_count++;

                // Auto-stop when reaching sample limit
                if (s_csv_logging_enabled && s_sample_count >= SAMPLE_LIMIT) {
                    ESP_LOGI(TAG, "Sample limit (%d) reached, stopping logging", SAMPLE_LIMIT);
                    stop_sampling_timer();  // Stop ADC sampling
                    stop_csv_logging();     // Stop CSV logging
                    // After stopping, skip enqueueing further samples
                    continue;
                }
                
                // Send to CSV queue if logging is enabled (non-blocking to maintain ADC rate)
                if (s_csv_logging_enabled && s_csv_queue != NULL) {
                    csv_sample_t sample = {
                        .adc_value = raw_value,
                        .timestamp_us = esp_timer_get_time()
                    };
                    // Non-blocking send (0 timeout) - maintains target ADC rate
                    // If queue is full, sample is dropped rather than blocking ADC task
                    if (xQueueSend(s_csv_queue, &sample, 0) != pdTRUE) {
                        // Queue full - log warning periodically
                        static uint32_t queue_full_count = 0;
                        queue_full_count++;
                        if (queue_full_count % 1000 == 0) {
                            ESP_LOGW(TAG, "CSV queue full (%d/%d), samples dropped to maintain ADC rate", 
                                    uxQueueMessagesWaiting(s_csv_queue), CSV_QUEUE_SIZE);
                        }
                    }
                }
            }
        } else if (parse_ret == ESP_ERR_INVALID_STATE) {
            // Buffer overflow - samples were lost
            dropped_samples += 10; // Estimate lost samples
            ESP_LOGW(TAG, "ADC buffer overflow - samples lost");
        } else if (parse_ret == ESP_ERR_TIMEOUT) {
            // No data available yet, continue
            continue;
        } else if (parse_ret == ESP_ERR_INVALID_SIZE) {
            // Buffer size issue, try again
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        
        // Log dropped samples periodically
        if (dropped_samples > 0 && (s_sample_count % 10000 == 0)) {
            ESP_LOGW(TAG, "Dropped samples so far: %lu", (unsigned long)dropped_samples);
        }
    }
    
    free(parsed_samples);
    vTaskDelete(NULL);
}

// CSV writer task: reads samples from queue and writes to CSV file
static void csv_writer_task(void *pvParameters) {
    csv_sample_t batch[CSV_WRITE_BATCH_SIZE];
    uint32_t batch_count = 0;
    // Use actual sample rate for timestamp calculation (may be clamped to minimum)
    const uint64_t sample_interval_us = 1000000ULL / s_actual_sample_rate_hz;
    
    ESP_LOGI(TAG, "CSV writer task started");
    
    while (1) {
        // Wait for samples from queue (blocking with timeout)
        csv_sample_t sample;
        if (xQueueReceive(s_csv_queue, &sample, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Add to batch
            batch[batch_count] = sample;
            batch_count++;
            
            // Write batch when full or when logging stops
            if (batch_count >= CSV_WRITE_BATCH_SIZE || 
                (!s_csv_logging_enabled && batch_count > 0)) {
                // Write batch directly to CSV file
                if (s_csv_file_mutex != NULL && xSemaphoreTake(s_csv_file_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    if (s_csv_file != NULL) {
                        // Format and write batch to CSV with calculated timestamps (consistent interval)
                        for (uint32_t i = 0; i < batch_count; i++) {
                            // Calculate timestamp based on sample index and target rate
                            uint64_t timestamp_us = s_csv_sample_index * sample_interval_us;
                            fprintf(s_csv_file, "%llu,%d\n",
                                   (unsigned long long)timestamp_us,
                                   batch[i].adc_value);
                            s_csv_sample_index++;
                        }
                        // Flush periodically (every N batches) to reduce flash wear
                        static uint32_t flush_counter = 0;
                        flush_counter++;
                        if (flush_counter >= CSV_FLUSH_INTERVAL || !s_csv_logging_enabled) {
                            fflush(s_csv_file);
                            flush_counter = 0;
                        }
                    }
                    xSemaphoreGive(s_csv_file_mutex);
                }
                batch_count = 0;
            }
        } else {
            // Timeout - flush any remaining batch when logging is disabled
            if (!s_csv_logging_enabled && batch_count > 0) {
                // Write final batch to CSV file
                if (s_csv_file_mutex != NULL && xSemaphoreTake(s_csv_file_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    if (s_csv_file != NULL) {
                        // Format and write final batch with calculated timestamps
                        for (uint32_t i = 0; i < batch_count; i++) {
                            uint64_t timestamp_us = s_csv_sample_index * sample_interval_us;
                            fprintf(s_csv_file, "%llu,%d\n",
                                   (unsigned long long)timestamp_us,
                                   batch[i].adc_value);
                            s_csv_sample_index++;
                        }
                        fflush(s_csv_file);  // Always flush on final batch
                        ESP_LOGI(TAG, "Final batch written (timeout): %d samples (total written: %d)", 
                                batch_count, s_csv_sample_index);
                    }
                    xSemaphoreGive(s_csv_file_mutex);
                }
                batch_count = 0;
            }
        }
        
        // Check for final batch immediately after logging stops (handles case where queue is empty but batch has data)
        if (!s_csv_logging_enabled && batch_count > 0 && 
            (s_csv_queue == NULL || uxQueueMessagesWaiting(s_csv_queue) == 0)) {
            // Write final batch to CSV file
            if (s_csv_file_mutex != NULL && xSemaphoreTake(s_csv_file_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                if (s_csv_file != NULL) {
                    // Format and write final batch with calculated timestamps
                    for (uint32_t i = 0; i < batch_count; i++) {
                        uint64_t timestamp_us = s_csv_sample_index * sample_interval_us;
                        fprintf(s_csv_file, "%llu,%d\n",
                               (unsigned long long)timestamp_us,
                               batch[i].adc_value);
                        s_csv_sample_index++;
                    }
                    fflush(s_csv_file);  // Always flush on final batch
                    ESP_LOGI(TAG, "Final batch written (immediate): %d samples (total written: %d)", 
                            batch_count, s_csv_sample_index);
                }
                xSemaphoreGive(s_csv_file_mutex);
            }
            batch_count = 0;
        }
        
        // Check if queue is getting full (warning)
        if (s_csv_queue != NULL) {
            UBaseType_t queue_waiting = uxQueueMessagesWaiting(s_csv_queue);
            if (queue_waiting > (CSV_QUEUE_SIZE * 0.8)) {
                ESP_LOGW(TAG, "CSV queue nearly full: %d/%d messages waiting", 
                        queue_waiting, CSV_QUEUE_SIZE);
            }
        }
    }
    
    vTaskDelete(NULL);
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

bool is_csv_logging_active(void) {
    // Return true if ADC is running (for API compatibility)
    return s_adc_running;
}

int get_sample_count(void) {
    return s_sample_count;
}

// Get logging statistics (sample count, elapsed time, and rate)
void get_logging_stats(int *sample_count, uint64_t *elapsed_time_ms, float *rate_hz) {
    *sample_count = s_sample_count;
    
    // Calculate elapsed time and rate from start to stop
    if (s_logging_start_time_us > 0) {
        uint64_t stop_time = s_logging_stop_time_us;
        
        // If not stopped yet, use current time
        if (stop_time == 0) {
            stop_time = esp_timer_get_time();
        }
        
        // Calculate elapsed time in microseconds
        if (stop_time > s_logging_start_time_us) {
            uint64_t elapsed_us = stop_time - s_logging_start_time_us;
            *elapsed_time_ms = elapsed_us / 1000;
            
            // Calculate actual rate: samples / elapsed_time_in_seconds
            // elapsed_time_in_seconds = elapsed_us / 1000000
            // rate = samples / (elapsed_us / 1000000) = samples * 1000000 / elapsed_us
            if (elapsed_us > 0 && s_sample_count > 0) {
                *rate_hz = ((float)s_sample_count * 1000000.0f) / (float)elapsed_us;
            } else {
                *rate_hz = 0.0f;
            }
        } else {
            // Time calculation error
            *elapsed_time_ms = 0;
            *rate_hz = 0.0f;
        }
    } else {
        *elapsed_time_ms = 0;
        *rate_hz = 0.0f;
    }
}

// Get sample queue (for server.c compatibility)
QueueHandle_t get_sample_queue(void) {
    return s_csv_queue;
}

// Get SPIFFS storage info
void get_spiffs_storage_info(size_t *total_bytes, size_t *used_bytes) {
    esp_err_t ret = esp_spiffs_info(NULL, total_bytes, used_bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS info: %s", esp_err_to_name(ret));
        *total_bytes = 0;
        *used_bytes = 0;
    }
}

// Generate CSV filename based on actual and attempted sample rates
static void generate_csv_filename(void) {
    snprintf(s_csv_file_path, sizeof(s_csv_file_path), 
             "/spiffs/data_%uHz_attempt_%d.csv",
             (unsigned int)s_actual_sample_rate_hz, SAMPLE_RATE_HZ);
    ESP_LOGI(TAG, "CSV filename: %s", s_csv_file_path);
}

// Start ADC sampling and CSV logging
void start_csv_logging(void) {
    // Generate CSV filename based on actual and attempted rates
    generate_csv_filename();
    
    // Protect file access with mutex
    if (s_csv_file_mutex != NULL && xSemaphoreTake(s_csv_file_mutex, portMAX_DELAY) == pdTRUE) {
        // Close any existing CSV file handle first
        if (s_csv_file != NULL) {
            fflush(s_csv_file);
            fclose(s_csv_file);
            s_csv_file = NULL;
        }
        
        // Clean up any previous files (check if they exist first)
        struct stat st;
        if (stat(s_csv_file_path, &st) == 0) {
            ESP_LOGI(TAG, "Found existing CSV file, deleting...");
            if (remove(s_csv_file_path) == 0) {
                ESP_LOGI(TAG, "CSV file deleted: %s", s_csv_file_path);
            } else {
                ESP_LOGW(TAG, "Failed to delete CSV file (errno: %d)", errno);
            }
        }
        
        // Check SPIFFS space before opening file
        size_t total = 0, used = 0;
        esp_err_t ret = esp_spiffs_info(NULL, &total, &used);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SPIFFS: %d KB used of %d KB total", used / 1024, total / 1024);
            // If SPIFFS is nearly full (>90%), format it
            if (used > (total * 9 / 10)) {
                ESP_LOGW(TAG, "SPIFFS nearly full (%d%%), formatting...", (used * 100) / total);
                esp_vfs_spiffs_unregister(NULL);
                esp_vfs_spiffs_conf_t conf = {
                    .base_path = SPIFFS_MOUNT_POINT,
                    .partition_label = NULL,
                    .max_files = 5,
                    .format_if_mount_failed = true
                };
                ret = esp_vfs_spiffs_register(&conf);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "SPIFFS reformatted successfully");
                } else {
                    ESP_LOGE(TAG, "Failed to reformat SPIFFS: %s", esp_err_to_name(ret));
                }
            }
        }
        
        // Open CSV file for writing
        s_csv_file = fopen(s_csv_file_path, "w");
        if (s_csv_file == NULL) {
            ESP_LOGE(TAG, "Failed to open CSV file: %s (errno: %d)", s_csv_file_path, errno);
            if (errno == 28) {  // ENOSPC - No space left
                ESP_LOGE(TAG, "SPIFFS is full! Attempting to format...");
                // Unregister and reformat SPIFFS
                esp_vfs_spiffs_unregister(NULL);
                esp_vfs_spiffs_conf_t conf = {
                    .base_path = SPIFFS_MOUNT_POINT,
                    .partition_label = NULL,
                    .max_files = 5,
                    .format_if_mount_failed = true
                };
                ret = esp_vfs_spiffs_register(&conf);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "SPIFFS reformatted, retrying file open...");
                    s_csv_file = fopen(s_csv_file_path, "w");
                    if (s_csv_file != NULL) {
                        fprintf(s_csv_file, "timestamp_us,adc_value\n");
                        fflush(s_csv_file);
                        ESP_LOGI(TAG, "CSV file opened after reformat: %s", s_csv_file_path);
                    } else {
                        ESP_LOGE(TAG, "Still failed to open CSV after reformat (errno: %d)", errno);
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to reformat SPIFFS: %s", esp_err_to_name(ret));
                }
            }
        } else {
            // Write CSV header
            fprintf(s_csv_file, "timestamp_us,adc_value\n");
            fflush(s_csv_file);
            ESP_LOGI(TAG, "CSV file opened: %s", s_csv_file_path);
        }
        xSemaphoreGive(s_csv_file_mutex);
    }
    
    // Clear queue
    if (s_csv_queue != NULL) {
        xQueueReset(s_csv_queue);
    }
    
    // Set start time BEFORE enabling logging (fixes timestamp race condition)
    s_logging_start_time_us = esp_timer_get_time();
    s_sample_count = 0;  // Reset sample count
    s_logging_stop_time_us = 0;  // Reset stop time
    s_csv_sample_index = 0;  // Reset CSV sample index for consistent timestamps
    
    // Enable CSV logging AFTER start time is set
    s_csv_logging_enabled = true;
    
    // Start ADC sampling
    start_sampling_timer();
    ESP_LOGI(TAG, "ADC sampling started @ %d Hz (CSV file logging)", SAMPLE_RATE_HZ);
}

// Stop ADC sampling and CSV logging
void stop_csv_logging(void) {
    // Disable CSV logging first (stops adc_task from sending to queue)
    s_csv_logging_enabled = false;

    // Wait until writer drains the queue before closing the file
    // Keep waiting until queue is empty AND writer task has had time to process
    int queue_empty_count = 0;
    while (s_csv_queue != NULL) {
        uint32_t queue_count = uxQueueMessagesWaiting(s_csv_queue);
        if (queue_count == 0) {
            queue_empty_count++;
            // Queue has been empty for 3 consecutive checks (30ms) - likely fully drained
            if (queue_empty_count >= 3) {
                break;
            }
        } else {
            queue_empty_count = 0;  // Reset counter if queue has items
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // Give writer task additional time to write any remaining batch
    // The writer task processes batches, so we need to wait for it to finish
    // Wait up to 500ms to ensure all batches are written
    for (int i = 0; i < 50; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        // Try to take mutex briefly - if we can get it, writer task is not writing
        if (s_csv_file_mutex != NULL && xSemaphoreTake(s_csv_file_mutex, 0) == pdTRUE) {
            // We got the mutex, writer task is not writing - safe to close
            xSemaphoreGive(s_csv_file_mutex);
            break;
        }
    }

    // Close CSV file (protected by mutex)
    if (s_csv_file_mutex != NULL && xSemaphoreTake(s_csv_file_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_csv_file != NULL) {
            fflush(s_csv_file);
            fclose(s_csv_file);
            s_csv_file = NULL;
            ESP_LOGI(TAG, "CSV file closed");
        }
        xSemaphoreGive(s_csv_file_mutex);
    }

    // Record stop time for rate calculation
    s_logging_stop_time_us = esp_timer_get_time();
    ESP_LOGI(TAG, "ADC sampling and CSV logging stopped. Total samples read: %d", s_sample_count);
}

// Clear storage (for server.c compatibility)
void clear_spiffs_storage(void) {
    // Delete CSV file (use current filename)
    if (remove(s_csv_file_path) == 0) {
        ESP_LOGI(TAG, "CSV file deleted: %s", s_csv_file_path);
    }
}

// Get CSV file path (exported for server.c)
const char* get_csv_file_path(void) {
    return s_csv_file_path;
}

// Initialize SPIFFS
static esp_err_t init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_MOUNT_POINT,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }
    
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d KB, used: %d KB", total / 1024, used / 1024);
    }
    
    return ESP_OK;
}

void app_main(void) {
    ESP_LOGI(TAG, "Laser ADC - Reading Rate Only");

    // Initialize NVS (required for WiFi)
    ESP_ERROR_CHECK(nvs_flash_init());

    // Initialize OLED display (if present)
    if (display_init() == ESP_OK) {
        display_show_status("Laser Logger", "Initializing...");
    } else {
        ESP_LOGW(TAG, "Display initialization failed - continuing without display");
    }

    // Initialize SPIFFS (for CSV file storage)
    ESP_ERROR_CHECK(init_spiffs());

    // Initialize SD card and test
    esp_err_t sdcard_ret = sdcard_init();
    if (sdcard_ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card initialized successfully");
        // Test SD card: read capacity and verify read/write functionality
        esp_err_t sd_test_ret = sdcard_test_read();
        if (sd_test_ret == ESP_OK) {
            display_show_status("SD card", "OK");
        } else {
            display_show_status("SD card", "TEST FAILED");
        }
    } else {
        ESP_LOGW(TAG, "SD card initialization failed: %s (continuing without SD card)", 
                esp_err_to_name(sdcard_ret), sdcard_ret);
        display_show_status("SD card", "INIT FAILED");
    }

    // Initialize hardware
    laser_init_full_on();
    adc_init_gpio4();

    // Create RTOS primitives
    // Create mutex for ADC value protection
    s_adc_value_mutex = xSemaphoreCreateMutex();
    if (s_adc_value_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create ADC value mutex");
        return;
    }
    
    // Create mutex for CSV file access
    s_csv_file_mutex = xSemaphoreCreateMutex();
    if (s_csv_file_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create CSV file mutex");
        return;
    }
    
    // Create queue for CSV samples
    s_csv_queue = xQueueCreate(CSV_QUEUE_SIZE, sizeof(csv_sample_t));
    if (s_csv_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create CSV queue");
        return;
    }
    
    // Start ADC processing task (reads from continuous ADC buffer)
    // Pinned to CPU 1, priority 5 (high priority for real-time processing)
    // Increased stack size to 8192 for DMA buffer allocation
    xTaskCreatePinnedToCore(adc_task, "adc_task", 8192, NULL, 5, NULL, 1);
    
    // Start CSV writer task (writes samples to CSV file)
    // Pinned to CPU 0, priority 3 (lower priority than ADC task)
    // Increased stack size to 16384 for large batch array (500 samples * 12 bytes = 6KB) + fprintf buffers
    xTaskCreatePinnedToCore(csv_writer_task, "csv_writer", 16384, NULL, 3, NULL, 0);
    
    // Don't start ADC automatically - wait for user to press start button
    // ADC will be started when start button is pressed
    
    ESP_LOGI(TAG, "RTOS architecture initialized: continuous ADC @ %d Hz, CSV logging enabled", SAMPLE_RATE_HZ);

    // Initialize WiFi AP (self-host)
#if MODE == WIFI
    wifi_init_sta();
    display_show_status("WiFi (STA)", "Connecting...");
#elif MODE == HOST
    wifi_init_softap();
    display_show_status("WiFi (AP)", "Starting hotspot...");
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
    display_show_status("SSID: " WIFI_AP_SSID, "Password: " WIFI_AP_PASSWORD);
#endif

    ESP_LOGI(TAG, "IP address: " IPSTR, IP2STR(&ip_info.ip));

    // Show web UI IP address on OLED (if initialized)
    char line1[22];
    char line2[22];
    snprintf(line1, sizeof(line1), "IP: " IPSTR, IP2STR(&ip_info.ip));
    snprintf(line2, sizeof(line2), "Ready: web control");
    display_show_status(line1, line2);
    // Keep running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
