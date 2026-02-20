#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>
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

#include "esp_sntp.h"

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
#define SAMPLE_RATE_HZ    4000           // Target: 4000 samples per second (minimum: ~611 Hz for ESP32-S3)
#define ADC_MIN_FREQ_HZ   611            // Minimum sampling frequency for ESP32-S3 continuous ADC
#define ADC_CONTINUOUS_BUF_SIZE 4096     // Buffer for continuous ADC (must be multiple of SOC_ADC_DIGI_RESULT_BYTES)
#define ADC_READ_TIMEOUT_MS 100          // Timeout for reading from continuous ADC

// SPIFFS config (fallback when SD card not mounted)
#define SPIFFS_MOUNT_POINT "/spiffs"
// CSV directory on SD (same base as bench; used when sdcard_is_mounted())
#define CSV_SD_DIR          SDCARD_MOUNT_POINT "/laser"
#define CSV_FILE_PATH_MAX_LEN 64         // Maximum length for CSV filename
#define CSV_QUEUE_SIZE 6000              // Queue size for ADC samples (~72KB; 1.5s at 4kHz; reduces drops during SSE/SD contention)
#define SAMPLE_LIMIT 10000000            // Auto-stop after this many samples

// CSV write buffer config
#define CSV_WRITE_BATCH_SIZE 500         // Write CSV in batches to reduce flash wear
#define CSV_FLUSH_INTERVAL 5             // Flush every N batches (reduces flash wear)

// Chunked logging: threshold-triggered, write 1s / skip 1s for PEAK cycles per chunk
#define PEAK 10                          // Number of write/skip cycles per chunk (change as needed)
#define THRESHOLD 200                   // ADC value to trigger writing
#define CHUNK_WRITE_SEC 1                // Seconds to write per cycle
#define CHUNK_SKIP_SEC 1                 // Seconds to skip per cycle
#define CSV_CHUNK_QUEUE_SIZE 16          // Max pending chunks for SSE
#define CSV_CHUNK_DIR_SD    CSV_SD_DIR "/chunks"
#define CSV_CHUNK_DIR_SPIFFS SPIFFS_MOUNT_POINT "/chunks"

static adc_continuous_handle_t adc_handle = NULL;
static adc_channel_t adc_chan;
static adc_unit_t adc_unit;
static uint32_t s_actual_sample_rate_hz = SAMPLE_RATE_HZ;  // Actual rate used (may be clamped to minimum)

// RTOS primitives
static SemaphoreHandle_t s_adc_value_mutex = NULL;
static SemaphoreHandle_t s_csv_file_mutex = NULL;  // Mutex for CSV file access
static SemaphoreHandle_t s_chunk_file_mutex = NULL;  // Mutex for chunk file SD access (writer + HTTP reader)
static SemaphoreHandle_t s_fprintf_mutex = NULL;  // Mutex for fprintf() protection (fprintf is not thread-safe)

// CSV sample structure for queue
typedef struct {
    int adc_value;
    uint64_t timestamp_us;
} csv_sample_t;

/** Binary bench record (must match tools/bin2csv.py). Packed for fixed 12-byte records. */
typedef struct {
    uint64_t timestamp_us;
    int32_t  adc_value;
} __attribute__((packed)) bench_record_t;

static QueueHandle_t s_csv_queue = NULL;

// Bench laser: run for T seconds, write ADC samples to SD. Test CSV and binary separately (one file per run).
// Stream in chunks to avoid allocating 160KB internal RAM (fits in ~16KB).
#define BENCH_LASER_DURATION_SEC 30
#define BENCH_LASER_CHUNK_SIZE   1000
#define BENCH_LASER_DIR          SDCARD_MOUNT_POINT "/laser"
#define BENCH_LASER_CSV_PATH     BENCH_LASER_DIR "/data.csv"
#define BENCH_LASER_BIN_PATH     BENCH_LASER_DIR "/data.bin"
/* Max ms to wait for one chunk before giving up (e.g. 1000/4000 = 250 ms at 4 kHz; use ~1 s). */
#define BENCH_LASER_CHUNK_TIMEOUT_MS  1000

typedef enum { BENCH_FMT_CSV, BENCH_FMT_BIN } bench_fmt_t;

/* Unix time for 2020-01-01 00:00:00 UTC; reject file mtime if system time is before this (not set or epoch). */
#define SANE_TIME_MIN_SEC  ((time_t)1577836800)

/** SNTP sync callback - logs when time sync completes (including background retries). */
static void sntp_sync_time_cb(struct timeval *tv) {
    time_t sec = tv ? (time_t)tv->tv_sec : 0;
    ESP_LOGI(TAG, "SNTP: sync completed (callback) - system time set, sec=%ld", (long)sec);
}

/** Set file access and modification time to current time (so PC shows correct "date modified" when SD is plugged in).
 *  Only sets time if system clock has been set (e.g. via SNTP when WiFi STA is connected); otherwise skips to avoid 1970. */
static void set_file_mtime_now(const char *path) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "set_file_mtime: gettimeofday failed, skipping utime for %s", path);
        return;
    }
    if (tv.tv_sec < SANE_TIME_MIN_SEC) {
        ESP_LOGW(TAG, "set_file_mtime: system time not set (sec=%ld), skipping utime for %s (init SNTP in STA mode for correct time)",
                 (long)tv.tv_sec, path);
        return;
    }
    struct utimbuf ut = {
        .actime  = (time_t)tv.tv_sec,
        .modtime = (time_t)tv.tv_sec,
    };
    if (utime(path, &ut) != 0) {
        ESP_LOGW(TAG, "utime %s failed (errno=%d)", path, errno);
    }
}

static volatile bool s_bench_mode = false;
static csv_sample_t *s_bench_buffer = NULL;
static volatile uint32_t s_bench_count = 0;
static volatile bool s_bench_chunk_ready = false;  // chunk full, ready to write

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

// Chunked logging state
#define CHUNK_PATH_MAX 80
typedef struct {
    int index;  // -1 = stream complete (no more chunks)
    char path[CHUNK_PATH_MAX];
    size_t size_bytes;
} chunk_ready_item_t;

static QueueHandle_t s_chunk_ready_queue = NULL;  // Server reads from this for SSE
static SemaphoreHandle_t s_chunk_stop_semaphore = NULL;  // FreeRTOS semaphore for immediate stop signal
static volatile bool s_chunked_logging_enabled = false;
static volatile bool s_chunk_triggered = false;   // Set when ADC >= THRESHOLD
static volatile bool s_chunk_stop_requested = false;

// Chunked writer state (only valid when s_chunked_logging_enabled)
static FILE *s_chunk_file = NULL;
static char s_chunk_file_path[CHUNK_PATH_MAX] = {0};
static int s_chunk_index = 0;
static int s_chunk_phase = 0;       // 0=WRITE, 1=SKIP, 2=POST_CHUNK_PAUSE
static int s_chunk_cycle = 0;
static uint64_t s_chunk_phase_start_us = 0;
static uint64_t s_chunk_pause_until_us = 0;   // End of post-chunk pause window
static uint32_t s_chunk_sample_index = 0;   // Per-chunk index (for local use)
static uint32_t s_chunk_global_sample_index = 0;  // Never reset; timestamps continue across chunks
static bool s_chunk_finish_write_then_stop = false;  // Finish current WRITE cycle then stop
// Per-cycle debug: samples written in current WRITE phase and last timestamp
static uint32_t s_chunk_cycle_write_count = 0;
static uint64_t s_chunk_cycle_last_ts_us = 0;

// Forward declarations (used before definitions)
void clear_spiffs_storage(void);
void stop_csv_logging(void);
static void generate_csv_filename(void);
static void bench_laser_run_one(bench_fmt_t fmt);
void start_chunked_logging(void);
void stop_chunked_logging(void);

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

                // Chunked mode: trigger when ADC crosses threshold
                if (s_chunked_logging_enabled && !s_chunk_triggered && raw_value >= THRESHOLD) {
                    s_chunk_triggered = true;
                    s_chunk_phase_start_us = esp_timer_get_time();
                    ESP_LOGI(TAG, "Chunked: threshold (%d) crossed, ADC=%d - starting write cycles", THRESHOLD, raw_value);
                }
                
                // Increment sample count for rate calculation
                s_sample_count++;

                // Bench laser: fill chunk; when full set chunk_ready and pause until reset
                if (s_bench_mode && s_bench_buffer != NULL) {
                    if (s_bench_chunk_ready) {
                        continue;  // wait for bench_laser_run_one() to write and reset
                    }
                    if (s_bench_count < BENCH_LASER_CHUNK_SIZE) {
                        s_bench_buffer[s_bench_count].adc_value = raw_value;
                        s_bench_buffer[s_bench_count].timestamp_us = esp_timer_get_time();
                        s_bench_count++;
                        if (s_bench_count >= BENCH_LASER_CHUNK_SIZE) {
                            s_bench_chunk_ready = true;
                        }
                    }
                    continue;
                }

                // Auto-stop when reaching sample limit
                if (s_csv_logging_enabled && s_sample_count >= SAMPLE_LIMIT) {
                    ESP_LOGI(TAG, "Sample limit (%d) reached, stopping logging", SAMPLE_LIMIT);
                    stop_sampling_timer();  // Stop ADC sampling
                    stop_csv_logging();     // Stop CSV logging
                    // After stopping, skip enqueueing further samples
                    continue;
                }
                
                // Send to CSV queue if logging is enabled (continuous or chunked mode)
                if ((s_csv_logging_enabled || s_chunked_logging_enabled) && s_csv_queue != NULL) {
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
                        if (queue_full_count % 5000 == 0) {
                            ESP_LOGW(TAG, "CSV queue full (%d/%d), samples dropped (total ~%lu)", 
                                    uxQueueMessagesWaiting(s_csv_queue), CSV_QUEUE_SIZE, (unsigned long)queue_full_count);
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
        /* ========== Chunked mode ========== */
        if (s_chunked_logging_enabled && s_csv_queue != NULL) {
            // In chunked mode: manage chunk state transitions based on time
            // Phases:
            //   0 = WRITE, 1 = SKIP, 2 = POST_CHUNK_PAUSE (no SD writes; give 1s window for client to fetch chunk)
            uint64_t now_us = esp_timer_get_time();

            // Handle post-chunk pause window (no SD writes; drain queue only)
            if (s_chunk_triggered && s_chunk_phase == 2) {
                if (now_us < s_chunk_pause_until_us) {
                    // During pause: drop samples quickly to avoid queue backup
                    csv_sample_t sample;
                    uint32_t drained = 0;
                    while (xQueueReceive(s_csv_queue, &sample, 0) == pdTRUE) { 
                        (void)sample;
                        drained++;
                    }
                    if (drained > 0 && (drained % 1000 == 0)) {
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] Pause phase: drained %u samples (remaining pause: %llu us)", 
                                 drained, (unsigned long long)(s_chunk_pause_until_us - now_us));
                    }
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                } else {
                    // Pause window over -> resume ADC and start next chunk (WRITE phase)
                    uint64_t pause_elapsed = now_us - (s_chunk_pause_until_us - 1000000ULL);
                    ESP_LOGI(TAG, "[CHUNK_DEBUG] Pause window expired (elapsed: %llu us), starting next chunk...", 
                             (unsigned long long)pause_elapsed);
                    s_chunk_index++;
                    s_chunk_cycle = 0;
                    s_chunk_phase = 0;
                    s_chunk_sample_index = 0;
                    s_chunk_phase_start_us = now_us;
                    ESP_LOGI(TAG, "[CHUNK_DEBUG] Next chunk: index=%d, cycle=%d, phase=%d", 
                             s_chunk_index, s_chunk_cycle, s_chunk_phase);

                    // Open next chunk file (protect SD I/O with mutex)
                    ESP_LOGI(TAG, "[CHUNK_DEBUG] Taking mutex to open next chunk file...");
                    if (s_chunk_file_mutex != NULL) {
                        if (xSemaphoreTake(s_chunk_file_mutex, portMAX_DELAY) == pdTRUE) {
                            ESP_LOGI(TAG, "[CHUNK_DEBUG] Mutex taken, opening file...");
                        } else {
                            ESP_LOGE(TAG, "[CHUNK_DEBUG] Failed to take mutex for next chunk!");
                        }
                    }
                    snprintf(s_chunk_file_path, CHUNK_PATH_MAX, "%s/chunk_%d.csv",
                             (sdcard_is_mounted() ? CSV_CHUNK_DIR_SD : CSV_CHUNK_DIR_SPIFFS), s_chunk_index);
                    ESP_LOGI(TAG, "[CHUNK_DEBUG] Opening chunk file: %s", s_chunk_file_path);
                    uint64_t open_start_us = esp_timer_get_time();
                    s_chunk_file = fopen(s_chunk_file_path, "w");
                    uint64_t fopen_done_us = esp_timer_get_time();
                    ESP_LOGI(TAG, "[CHUNK_DEBUG] fopen took %llu us", (unsigned long long)(fopen_done_us - open_start_us));
                    if (s_chunk_file) {
                        s_chunk_cycle_write_count = 0;
                        s_chunk_cycle_last_ts_us = 0;
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] File opened, writing header...");
                        uint64_t write_start_us = esp_timer_get_time();
                        if (s_fprintf_mutex != NULL) xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
                        fprintf(s_chunk_file, "timestamp_us,adc_value\n");
                        if (s_fprintf_mutex != NULL) xSemaphoreGive(s_fprintf_mutex);
                        fflush(s_chunk_file);
                        uint64_t flush_done_us = esp_timer_get_time();
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] fprintf+fflush took %llu us", (unsigned long long)(flush_done_us - write_start_us));
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] Syncing directory metadata...");
                        uint64_t sync_start_us = esp_timer_get_time();
                        // fsync() ensures directory metadata is written before releasing mutex
                        fsync(fileno(s_chunk_file));
                        uint64_t sync_done_us = esp_timer_get_time();
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] fsync took %llu us", (unsigned long long)(sync_done_us - sync_start_us));
                        uint64_t total_open_us = sync_done_us - open_start_us;
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] Total file open sequence: %llu us (%.2f ms)", 
                                 (unsigned long long)total_open_us, (double)total_open_us / 1000.0);
                    } else {
                        ESP_LOGE(TAG, "[CHUNK_DEBUG] Failed to open chunk file: errno=%d", errno);
                    }
                    if (s_chunk_file_mutex != NULL) {
                        xSemaphoreGive(s_chunk_file_mutex);
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] Mutex released after opening next chunk");
                    }
                }
            }

            uint64_t elapsed_us = now_us - s_chunk_phase_start_us;
            uint64_t phase_duration_us = (s_chunk_phase == 0) ? (CHUNK_WRITE_SEC * 1000000ULL) : (CHUNK_SKIP_SEC * 1000000ULL);

            if (elapsed_us >= phase_duration_us && s_chunk_triggered && s_chunk_file != NULL) {
                if (s_chunk_phase == 0) {  // WRITE -> SKIP
                    ESP_LOGI(TAG, "[CHUNK_CYCLE] chunk=%d cycle=%d samples_written=%lu last_timestamp_us=%llu",
                            s_chunk_index, s_chunk_cycle, (unsigned long)s_chunk_cycle_write_count,
                            (unsigned long long)s_chunk_cycle_last_ts_us);
                    s_chunk_cycle_write_count = 0;
                    s_chunk_phase = 1;
                    s_chunk_phase_start_us = now_us;
                    // Don't increment cycle here - increment after SKIP completes (a cycle = WRITE+SKIP pair)
                    if (s_chunk_finish_write_then_stop) {
                        // Stop was requested during write - we finished the cycle, now close
                        goto chunk_close_and_push;
                    }
                } else {  // SKIP -> next cycle or close
                    s_chunk_phase_start_us = now_us;
                    // Increment cycle AFTER completing SKIP (cycle = complete WRITE+SKIP pair)
                    s_chunk_cycle++;
                    // Close chunk if: reached PEAK cycles OR stop requested (ensures chunk 0 gets sent even if early)
                    if (s_chunk_cycle >= PEAK || s_chunk_stop_requested) {
chunk_close_and_push:;
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] chunk_close_and_push: chunk_index=%d, cycle=%d, phase=%d", 
                                 s_chunk_index, s_chunk_cycle, s_chunk_phase);
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] Taking mutex for file close...");
                        // Take mutex only for fflush/fclose (actual SD I/O)
                        if (s_chunk_file_mutex != NULL) {
                            if (xSemaphoreTake(s_chunk_file_mutex, portMAX_DELAY) == pdTRUE) {
                                ESP_LOGI(TAG, "[CHUNK_DEBUG] Mutex taken successfully");
                            } else {
                                ESP_LOGE(TAG, "[CHUNK_DEBUG] Failed to take mutex!");
                            }
                        }
                        if (s_chunk_file != NULL) {
                            uint64_t close_start_us = esp_timer_get_time();
                            ESP_LOGI(TAG, "[CHUNK_DEBUG] Flushing chunk file...");
                            uint64_t flush_start_us = esp_timer_get_time();
                            fflush(s_chunk_file);
                            uint64_t flush_done_us = esp_timer_get_time();
                            ESP_LOGI(TAG, "[CHUNK_DEBUG] fflush took %llu us", (unsigned long long)(flush_done_us - flush_start_us));
                            ESP_LOGI(TAG, "[CHUNK_DEBUG] Syncing directory metadata...");
                            uint64_t sync_start_us = esp_timer_get_time();
                            // fsync() ensures directory metadata is written before releasing mutex
                            fsync(fileno(s_chunk_file));
                            uint64_t sync_done_us = esp_timer_get_time();
                            ESP_LOGI(TAG, "[CHUNK_DEBUG] fsync took %llu us", (unsigned long long)(sync_done_us - sync_start_us));
                            ESP_LOGI(TAG, "[CHUNK_DEBUG] Directory metadata synced, closing file...");
                            uint64_t close_start2_us = esp_timer_get_time();
                            fclose(s_chunk_file);
                            uint64_t close_done_us = esp_timer_get_time();
                            ESP_LOGI(TAG, "[CHUNK_DEBUG] fclose took %llu us", (unsigned long long)(close_done_us - close_start2_us));
                            s_chunk_file = NULL;
                            uint64_t total_close_us = close_done_us - close_start_us;
                            ESP_LOGI(TAG, "[CHUNK_DEBUG] Total file close sequence: %llu us (%.2f ms)", 
                                     (unsigned long long)total_close_us, (double)total_close_us / 1000.0);
                        } else {
                            ESP_LOGW(TAG, "[CHUNK_DEBUG] s_chunk_file is NULL, nothing to close");
                        }
                        // Release mutex immediately after file close, before queue operations
                        // (queue operations can trigger priority inheritance which conflicts with held mutex)
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] Releasing mutex...");
                        if (s_chunk_file_mutex != NULL) {
                            xSemaphoreGive(s_chunk_file_mutex);
                            ESP_LOGI(TAG, "[CHUNK_DEBUG] Mutex released");
                        }
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] Getting file size via stat()...");
                        struct stat st;
                        size_t sz = 0;
                        if (stat(s_chunk_file_path, &st) == 0) {
                            sz = (size_t)st.st_size;
                            ESP_LOGI(TAG, "[CHUNK_DEBUG] File size: %zu bytes", sz);
                        } else {
                            ESP_LOGW(TAG, "[CHUNK_DEBUG] stat() failed: errno=%d", errno);
                        }
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] Setting file mtime...");
                        set_file_mtime_now(s_chunk_file_path);
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] Preparing chunk_ready_item...");
                        chunk_ready_item_t item = { .index = s_chunk_index, .size_bytes = sz };
                        strncpy(item.path, s_chunk_file_path, CHUNK_PATH_MAX - 1);
                        item.path[CHUNK_PATH_MAX - 1] = '\0';
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] Sending to chunk_ready_queue (queue=%p)...", (void*)s_chunk_ready_queue);
                        if (s_chunk_ready_queue != NULL) {
                            if (xQueueSend(s_chunk_ready_queue, &item, portMAX_DELAY) == pdTRUE) {
                                ESP_LOGI(TAG, "[CHUNK_DEBUG] Item sent to queue successfully");
                            } else {
                                ESP_LOGE(TAG, "[CHUNK_DEBUG] Failed to send item to queue!");
                            }
                        } else {
                            ESP_LOGW(TAG, "[CHUNK_DEBUG] chunk_ready_queue is NULL, skipping");
                        }
                        ESP_LOGI(TAG, "Chunk %d ready: %zu bytes | Packaging and sending to client", s_chunk_index, sz);

                        if (s_chunk_stop_requested || s_chunk_finish_write_then_stop) {
                            ESP_LOGI(TAG, "[CHUNK_DEBUG] Stop requested, sending done signal...");
                            chunk_ready_item_t done = { .index = -1, .path = {0}, .size_bytes = 0 };
                            if (s_chunk_ready_queue != NULL) xQueueSend(s_chunk_ready_queue, &done, portMAX_DELAY);
                            ESP_LOGI(TAG, "[CHUNK_DEBUG] Disabling chunked logging...");
                            s_chunked_logging_enabled = false;
                            s_chunk_stop_requested = false;
                            s_chunk_finish_write_then_stop = false;
                            s_chunk_triggered = false;
                            // Reset stop semaphore (take any pending signals)
                            if (s_chunk_stop_semaphore != NULL) {
                                xSemaphoreTake(s_chunk_stop_semaphore, 0);
                            }
                            ESP_LOGI(TAG, "[CHUNK_DEBUG] Chunked logging stopped, continuing loop");
                            continue;
                        }
                        // Start a 1s post-chunk pause window before opening the next chunk
                        uint64_t pause_start = esp_timer_get_time();
                        uint64_t close_ops_us = pause_start - (now_us - elapsed_us);  // Approximate time spent in close operations
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] Starting 1s post-chunk pause window...");
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] Close operations took ~%llu us (%.2f ms) before pause", 
                                 (unsigned long long)close_ops_us, (double)close_ops_us / 1000.0);
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] Entering pause window (ADC continues running, samples will be drained)");
                        s_chunk_cycle = 0;
                        s_chunk_phase = 2;  // POST_CHUNK_PAUSE
                        s_chunk_sample_index = 0;
                        s_chunk_pause_until_us = pause_start + 1000000ULL;
                        ESP_LOGI(TAG, "[CHUNK_DEBUG] Pause window: start=%llu, end=%llu (1s duration for client fetch)", 
                                 (unsigned long long)pause_start, (unsigned long long)s_chunk_pause_until_us);
                    } else {
                        s_chunk_phase = 0;
                        // Do NOT increment cycle here - already incremented when SKIP completed (one cycle = WRITE+SKIP)
                    }
                }
            }
            
            // In chunked mode: consume samples, write to files, and manage state
            csv_sample_t sample;
            if (xQueueReceive(s_csv_queue, &sample, pdMS_TO_TICKS(50)) != pdTRUE) {
                // Timeout - continue to check phase transitions
                continue;
            }

            if (!s_chunk_triggered) continue;  // Drop until threshold

            if (s_chunk_phase == 1) {
                /* SKIP phase: drain queue aggressively (0 timeout) to avoid backup and ADC drops */
                while (xQueueReceive(s_csv_queue, &sample, 0) == pdTRUE) { (void)sample; }
                continue;
            }

            // WRITE phase: write sample to file
            if (s_chunk_file != NULL && s_chunk_phase == 0) {
                const uint64_t interval_us = 1000000ULL / s_actual_sample_rate_hz;
                uint64_t ts = (uint64_t)s_chunk_global_sample_index * interval_us;
                if (s_fprintf_mutex != NULL) xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
                fprintf(s_chunk_file, "%llu,%d\n", (unsigned long long)ts, sample.adc_value);
                if (s_fprintf_mutex != NULL) xSemaphoreGive(s_fprintf_mutex);
                s_chunk_global_sample_index++;
                s_chunk_sample_index++;
                s_chunk_cycle_write_count++;
                s_chunk_cycle_last_ts_us = ts;
                
                // Check if WRITE phase duration exceeded (transition to SKIP)
                uint64_t now_us = esp_timer_get_time();
                uint64_t elapsed_us = now_us - s_chunk_phase_start_us;
                if (elapsed_us >= CHUNK_WRITE_SEC * 1000000ULL) {
                    ESP_LOGI(TAG, "[CHUNK_CYCLE] chunk=%d cycle=%d samples_written=%lu last_timestamp_us=%llu",
                            s_chunk_index, s_chunk_cycle, (unsigned long)s_chunk_cycle_write_count,
                            (unsigned long long)s_chunk_cycle_last_ts_us);
                    s_chunk_cycle_write_count = 0;
                    s_chunk_phase = 1;
                    s_chunk_phase_start_us = now_us;
                    // Don't increment cycle here - already done in time-based check at top of loop
                    // Take mutex only for fflush/fsync (actual SD I/O), not for buffered fprintf
                    if (s_chunk_file_mutex != NULL) xSemaphoreTake(s_chunk_file_mutex, portMAX_DELAY);
                    fflush(s_chunk_file);  // Flush before transitioning to SKIP
                    // Note: No fsync() here - we flush periodically during WRITE, full sync happens on close
                    if (s_chunk_file_mutex != NULL) xSemaphoreGive(s_chunk_file_mutex);
                    // If stop requested during WRITE: finish current WRITE cycle and close chunk (ensures chunk 0 gets sent)
                    if (s_chunk_finish_write_then_stop || s_chunk_stop_requested) {
                        goto chunk_close_and_push;
                    }
                }
            }
            continue;
        }

        /* ========== Continuous mode ========== */
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
                        if (s_fprintf_mutex != NULL) xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
                        for (uint32_t i = 0; i < batch_count; i++) {
                            // Calculate timestamp based on sample index and target rate
                            uint64_t timestamp_us = s_csv_sample_index * sample_interval_us;
                            fprintf(s_csv_file, "%llu,%d\n",
                                   (unsigned long long)timestamp_us,
                                   batch[i].adc_value);
                            s_csv_sample_index++;
                        }
                        if (s_fprintf_mutex != NULL) xSemaphoreGive(s_fprintf_mutex);
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
                        if (s_fprintf_mutex != NULL) xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
                        for (uint32_t i = 0; i < batch_count; i++) {
                            uint64_t timestamp_us = s_csv_sample_index * sample_interval_us;
                            fprintf(s_csv_file, "%llu,%d\n",
                                   (unsigned long long)timestamp_us,
                                   batch[i].adc_value);
                            s_csv_sample_index++;
                        }
                        if (s_fprintf_mutex != NULL) xSemaphoreGive(s_fprintf_mutex);
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
                    if (s_fprintf_mutex != NULL) xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
                    for (uint32_t i = 0; i < batch_count; i++) {
                        uint64_t timestamp_us = s_csv_sample_index * sample_interval_us;
                        fprintf(s_csv_file, "%llu,%d\n",
                               (unsigned long long)timestamp_us,
                               batch[i].adc_value);
                        s_csv_sample_index++;
                    }
                    if (s_fprintf_mutex != NULL) xSemaphoreGive(s_fprintf_mutex);
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
    // Return the number of samples actually written to the CSV file,
    // not the total number of ADC samples seen. This aligns the count
    // with what the client downloads.
    return (int)s_csv_sample_index;
}

// Get logging statistics (sample count, elapsed time, and rate)
void get_logging_stats(int *sample_count, uint64_t *elapsed_time_ms, float *rate_hz) {
    // Use the number of CSV-written samples for stats so that rate and
    // sample count match what is in the file.
    *sample_count = (int)s_csv_sample_index;

    // Derive elapsed time directly from the CSV sample count and the actual
    // sample rate so that the duration shown in the web UI matches the CSV
    // (e.g. 16.67 s instead of the slightly longer wall-clock interval).
    if (s_csv_sample_index > 0 && s_actual_sample_rate_hz > 0) {
        uint64_t elapsed_us = ((uint64_t)s_csv_sample_index * 1000000ULL) / s_actual_sample_rate_hz;
        *elapsed_time_ms = elapsed_us / 1000;
        *rate_hz = (float)s_actual_sample_rate_hz;
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

// Generate CSV path: SD card when mounted, else SPIFFS (avoids crash when SD absent)
static void generate_csv_filename(void) {
    const char *base_dir;
    if (sdcard_is_mounted()) {
        base_dir = CSV_SD_DIR;
    } else {
        base_dir = SPIFFS_MOUNT_POINT;
    }
    snprintf(s_csv_file_path, sizeof(s_csv_file_path), "%s/data.csv",
             base_dir);
    ESP_LOGI(TAG, "CSV path: %s", s_csv_file_path);
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
        
        // Clean up any previous file at this path
        struct stat st;
        if (stat(s_csv_file_path, &st) == 0) {
            if (remove(s_csv_file_path) == 0) {
                ESP_LOGI(TAG, "CSV file deleted: %s", s_csv_file_path);
            } else {
                ESP_LOGW(TAG, "Failed to delete CSV file (errno: %d)", errno);
            }
        }

        bool use_sd = sdcard_is_mounted();
        if (use_sd) {
            // Ensure CSV directory exists on SD. Use mode 0 for FAT (0755 can cause EINVAL on FatFS).
            if (mkdir(CSV_SD_DIR, 0) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "mkdir %s failed (errno=%d)", CSV_SD_DIR, errno);
            }
        } else {
            // SPIFFS: check space and optionally reformat if nearly full
            size_t total = 0, used = 0;
            esp_err_t ret = esp_spiffs_info(NULL, &total, &used);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "SPIFFS: %d KB used of %d KB total", used / 1024, total / 1024);
                if (used > (total * 9 / 10)) {
                    ESP_LOGW(TAG, "SPIFFS nearly full (%d%%), formatting...", (used * 100) / total);
                    esp_vfs_spiffs_unregister(NULL);
                    esp_vfs_spiffs_conf_t conf = {
                        .base_path = SPIFFS_MOUNT_POINT,
                        .partition_label = NULL,
                        .max_files = 5,
                        .format_if_mount_failed = true
                    };
                    if (esp_vfs_spiffs_register(&conf) != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to reformat SPIFFS");
                    }
                }
            }
        }

        // Open CSV file for writing
        s_csv_file = fopen(s_csv_file_path, "w");
        if (s_csv_file == NULL) {
            ESP_LOGE(TAG, "Failed to open CSV file: %s (errno: %d)", s_csv_file_path, errno);
            if (!use_sd && errno == 28) {  // ENOSPC on SPIFFS: reformat and retry once
                esp_vfs_spiffs_unregister(NULL);
                esp_vfs_spiffs_conf_t conf = {
                    .base_path = SPIFFS_MOUNT_POINT,
                    .partition_label = NULL,
                    .max_files = 5,
                    .format_if_mount_failed = true
                };
                if (esp_vfs_spiffs_register(&conf) == ESP_OK) {
                    s_csv_file = fopen(s_csv_file_path, "w");
                    if (s_csv_file != NULL) {
                        if (s_fprintf_mutex != NULL) xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
                        fprintf(s_csv_file, "timestamp_us,adc_value\n");
                        if (s_fprintf_mutex != NULL) xSemaphoreGive(s_fprintf_mutex);
                        fflush(s_csv_file);
                        ESP_LOGI(TAG, "CSV file opened after SPIFFS reformat: %s", s_csv_file_path);
                    }
                }
            }
        } else {
            if (s_fprintf_mutex != NULL) xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
            fprintf(s_csv_file, "timestamp_us,adc_value\n");
            if (s_fprintf_mutex != NULL) xSemaphoreGive(s_fprintf_mutex);
            fflush(s_csv_file);
            ESP_LOGI(TAG, "CSV file opened: %s", s_csv_file_path);
        }
        xSemaphoreGive(s_csv_file_mutex);
    }

    // Only enable logging and start ADC if file opened (avoid silent drop when SD/SPIFFS open fails)
    if (s_csv_file == NULL) {
        ESP_LOGE(TAG, "CSV file not opened; start logging aborted");
        return;
    }
    if (s_csv_queue != NULL) {
        xQueueReset(s_csv_queue);
    }
    s_logging_start_time_us = esp_timer_get_time();
    s_sample_count = 0;
    s_logging_stop_time_us = 0;
    s_csv_sample_index = 0;
    s_csv_logging_enabled = true;
    start_sampling_timer();
    ESP_LOGI(TAG, "ADC sampling started @ %d Hz (CSV logging to %s)", SAMPLE_RATE_HZ,
             sdcard_is_mounted() ? "SD" : "SPIFFS");
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
            set_file_mtime_now(s_csv_file_path);
            ESP_LOGI(TAG, "CSV file closed");
        }
        xSemaphoreGive(s_csv_file_mutex);
    }

    // Record stop time for rate calculation
    s_logging_stop_time_us = esp_timer_get_time();
    // Log the number of samples actually written into the CSV file so the
    // log matches what the client downloads.
    ESP_LOGI(TAG, "ADC sampling and CSV logging stopped. Total CSV samples written: %u", (unsigned)s_csv_sample_index);
}

// Clear CSV file (works for path on SD or SPIFFS; server.c compatibility)
void clear_spiffs_storage(void) {
    if (remove(s_csv_file_path) == 0) {
        ESP_LOGI(TAG, "CSV file deleted: %s", s_csv_file_path);
    }
}

// Get CSV file path (exported for server.c)
const char* get_csv_file_path(void) {
    return s_csv_file_path;
}

// Get chunk ready queue (for server SSE) - DEPRECATED: use direct queue streaming instead
QueueHandle_t get_chunk_ready_queue(void) {
    return s_chunk_ready_queue;
}

bool is_chunked_logging_active(void) {
    return s_chunked_logging_enabled;
}

// Get chunk state for SSE streaming (direct queue access)
bool get_chunk_triggered(void) {
    return s_chunk_triggered;
}

int get_chunk_phase(void) {
    return s_chunk_phase;
}

int get_chunk_cycle(void) {
    return s_chunk_cycle;
}

int get_chunk_index(void) {
    return s_chunk_index;
}

uint64_t get_chunk_phase_start_us(void) {
    return s_chunk_phase_start_us;
}

uint32_t get_chunk_global_sample_index(void) {
    return s_chunk_global_sample_index;
}

// Get chunk stop semaphore (for SSE handler to check stop immediately)
SemaphoreHandle_t get_chunk_stop_semaphore(void) {
    return s_chunk_stop_semaphore;
}

// Get chunk file mutex (serialize SD access between csv_writer and HTTP chunk_get_handler)
SemaphoreHandle_t get_chunk_file_mutex(void) {
    return s_chunk_file_mutex;
}

// Check if we should finish current WRITE cycle before stopping
bool get_chunk_finish_write_then_stop(void) {
    return s_chunk_finish_write_then_stop;
}

// Start chunked logging: wait for threshold, then write 1s/skip 1s for PEAK cycles per chunk
void start_chunked_logging(void) {
    if (s_chunked_logging_enabled) {
        ESP_LOGW(TAG, "Chunked logging already active");
        return;
    }
    s_chunk_stop_requested = false;
    s_chunk_finish_write_then_stop = false;
    s_chunk_triggered = false;
    s_chunk_index = 0;
    s_chunk_cycle = 0;
    s_chunk_phase = 0;
    s_chunk_sample_index = 0;
    s_chunk_global_sample_index = 0;  /* Timestamps start at 0 for new session */
    s_chunk_pause_until_us = 0;
    
    // Reset stop semaphore (take any pending signals)
    if (s_chunk_stop_semaphore != NULL) {
        xSemaphoreTake(s_chunk_stop_semaphore, 0);
    }

    const char *chunk_dir = sdcard_is_mounted() ? CSV_CHUNK_DIR_SD : CSV_CHUNK_DIR_SPIFFS;
    if (mkdir(chunk_dir, 0) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed (errno=%d)", chunk_dir, errno);
        return;
    }

    // Take mutex only for fopen/fflush (actual SD I/O)
    if (s_chunk_file_mutex != NULL) xSemaphoreTake(s_chunk_file_mutex, portMAX_DELAY);
    snprintf(s_chunk_file_path, CHUNK_PATH_MAX, "%s/chunk_0.csv", chunk_dir);
    s_chunk_file = fopen(s_chunk_file_path, "w");
    if (s_chunk_file == NULL) {
        if (s_chunk_file_mutex != NULL) xSemaphoreGive(s_chunk_file_mutex);
        ESP_LOGE(TAG, "Failed to open chunk file: %s", s_chunk_file_path);
        return;
    }
    if (s_fprintf_mutex != NULL) xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
    fprintf(s_chunk_file, "timestamp_us,adc_value\n");
    if (s_fprintf_mutex != NULL) xSemaphoreGive(s_fprintf_mutex);
    fflush(s_chunk_file);
    // fsync() ensures directory metadata is written before releasing mutex
    fsync(fileno(s_chunk_file));
    // Release mutex after file operations (fprintf is buffered, doesn't need mutex)
    if (s_chunk_file_mutex != NULL) xSemaphoreGive(s_chunk_file_mutex);

    if (s_csv_queue) xQueueReset(s_csv_queue);
    s_chunk_phase_start_us = esp_timer_get_time();
    s_chunked_logging_enabled = true;
    start_sampling_timer();
    ESP_LOGI(TAG, "Chunked logging started: waiting for ADC >= %d, then %d cycles (write %ds/skip %ds)", THRESHOLD, PEAK, CHUNK_WRITE_SEC, CHUNK_SKIP_SEC);
}

void stop_chunked_logging(void) {
    if (!s_chunked_logging_enabled) return;
    s_chunk_stop_requested = true;
    // Always finish current WRITE cycle if in WRITE phase (ensures chunk 0 gets sent even if early)
    if (s_chunk_phase == 0) {
        s_chunk_finish_write_then_stop = true;  // Finish current WRITE cycle then stop (for csv_writer_task)
    }
    // Special case: if chunk 0 is active, ensure it gets sent even if not in WRITE phase
    // (will be handled by cycle check: s_chunk_index == 0 && s_chunk_stop_requested)
    // Signal stop semaphore immediately for high-priority stop handling (SSE handler stops immediately)
    if (s_chunk_stop_semaphore != NULL) {
        xSemaphoreGive(s_chunk_stop_semaphore);
        // Give semaphore multiple times to ensure it's seen even if already taken
        // (binary semaphore can only hold one, but this ensures it's available)
        vTaskDelay(pdMS_TO_TICKS(1));
        xSemaphoreGive(s_chunk_stop_semaphore);
    }
    ESP_LOGI(TAG, "Chunked logging stop requested (semaphore signaled - SSE will stop immediately)");
}

/**
 * Bench laser: run for BENCH_LASER_DURATION_SEC seconds, write ADC samples to SD
 * to a single file (CSV or binary). Streams in chunks (BENCH_LASER_CHUNK_SIZE).
 * Binary format matches tools/bin2csv.py (12-byte records: uint64 timestamp_us, int32 adc_value).
 */
static void bench_laser_run_one(bench_fmt_t fmt)
{
    const char *path = (fmt == BENCH_FMT_CSV) ? BENCH_LASER_CSV_PATH : BENCH_LASER_BIN_PATH;
    const char *label = (fmt == BENCH_FMT_CSV) ? "CSV" : "BIN";

    if (!sdcard_is_mounted()) {
        ESP_LOGW(TAG, "bench_laser: SD card not mounted, skip");
        return;
    }
    if (mkdir(BENCH_LASER_DIR, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "bench_laser: mkdir %s failed (errno=%d %s)", BENCH_LASER_DIR, errno, strerror(errno));
        return;
    }

    size_t buf_size = (size_t)BENCH_LASER_CHUNK_SIZE * sizeof(csv_sample_t);
    s_bench_buffer = (csv_sample_t *)heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL);
    if (s_bench_buffer == NULL) {
        ESP_LOGE(TAG, "bench_laser: alloc %u bytes failed", (unsigned)buf_size);
        return;
    }

    FILE *f = (fmt == BENCH_FMT_CSV)
              ? fopen(BENCH_LASER_CSV_PATH, "w")
              : fopen(BENCH_LASER_BIN_PATH, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "bench_laser: open %s failed (errno=%d)", label, errno);
        heap_caps_free(s_bench_buffer);
        s_bench_buffer = NULL;
        return;
    }

    size_t bytes_written = 0;
    if (fmt == BENCH_FMT_CSV) {
        int nh = fprintf(f, "timestamp_us,adc_value\n");
        bytes_written = (nh > 0) ? (size_t)nh : 0u;
    }
    uint32_t total_written = 0;
    const int64_t duration_us = (int64_t)BENCH_LASER_DURATION_SEC * 1000000;

    s_bench_count = 0;
    s_bench_chunk_ready = false;
    s_bench_mode = true;
    start_sampling_timer();
    int64_t t0 = esp_timer_get_time();

    ESP_LOGI(TAG, "bench_laser: %s test running for %d seconds...", label, BENCH_LASER_DURATION_SEC);

    while (1) {
        int64_t elapsed_us = esp_timer_get_time() - t0;
        if (elapsed_us >= duration_us) {
            break;
        }
        int64_t chunk_t0 = esp_timer_get_time();
        uint32_t elapsed_ms = 0;
        while (!s_bench_chunk_ready && elapsed_ms < BENCH_LASER_CHUNK_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(20));
            elapsed_ms = (uint32_t)((esp_timer_get_time() - chunk_t0) / 1000);
            if ((esp_timer_get_time() - t0) >= duration_us) {
                break;
            }
        }
        if (!s_bench_chunk_ready) {
            ESP_LOGW(TAG, "bench_laser: chunk timeout");
            break;
        }
        uint32_t n = s_bench_count;
        for (uint32_t i = 0; i < n; i++) {
            if (fmt == BENCH_FMT_CSV) {
                int nr = fprintf(f, "%llu,%d\n",
                                 (unsigned long long)s_bench_buffer[i].timestamp_us,
                                 s_bench_buffer[i].adc_value);
                if (nr > 0) {
                    bytes_written += (size_t)nr;
                }
            } else {
                bench_record_t rec = {
                    .timestamp_us = s_bench_buffer[i].timestamp_us,
                    .adc_value    = (int32_t)s_bench_buffer[i].adc_value,
                };
                bytes_written += fwrite(&rec, 1, sizeof(rec), f);
            }
        }
        total_written += n;
        s_bench_count = 0;
        s_bench_chunk_ready = false;
    }

    int64_t t1 = esp_timer_get_time();
    stop_sampling_timer();
    s_bench_mode = false;
    fclose(f);
    set_file_mtime_now(path);
    heap_caps_free(s_bench_buffer);
    s_bench_buffer = NULL;

    double time_sec = (t1 - t0) / 1000000.0;
    double samples_per_sec = (time_sec > 0 && total_written > 0) ? (total_written / time_sec) : 0;
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " bench_laser %s: ran %.2f s, %u samples -> %.1f samples/s, %u bytes",
             label, time_sec, (unsigned)total_written, samples_per_sec, (unsigned)bytes_written);
    ESP_LOGI(TAG, "========================================");
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

    // Initialize SPIFFS (fallback for CSV when SD card not mounted)
    ESP_ERROR_CHECK(init_spiffs());

    // Initialize SD card (used for CSV logging when mounted; no read/write test on startup)
    esp_err_t sdcard_ret = sdcard_init();
    if (sdcard_ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card initialized successfully");
        display_show_status("SD card", "OK");
    } else {
        ESP_LOGW(TAG, "SD card initialization failed: %s (CSV will use SPIFFS)", esp_err_to_name(sdcard_ret));
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
    
    // Create mutex for fprintf() protection (fprintf is not thread-safe)
    s_fprintf_mutex = xSemaphoreCreateMutex();
    if (s_fprintf_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create fprintf mutex");
        return;
    }
    
    // Create queue for CSV samples
    s_csv_queue = xQueueCreate(CSV_QUEUE_SIZE, sizeof(csv_sample_t));
    if (s_csv_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create CSV queue");
        return;
    }

    // Create queue for chunked SSE (chunk path when ready)
    s_chunk_ready_queue = xQueueCreate(CSV_CHUNK_QUEUE_SIZE, sizeof(chunk_ready_item_t));
    if (s_chunk_ready_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create chunk ready queue");
    }
    
    s_chunk_file_mutex = xSemaphoreCreateMutex();
    if (s_chunk_file_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create chunk file mutex");
    }
    // Create binary semaphore for high-priority stop signal
    s_chunk_stop_semaphore = xSemaphoreCreateBinary();
    if (s_chunk_stop_semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create chunk stop semaphore");
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


    // Initialize WiFi
#if MODE == WIFI
    wifi_init_sta();
    display_show_status("WiFi (STA)", "Connecting...");
    // Set system time via SNTP so file mtime on SD card is correct
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "time.cloudflare.com");
    esp_sntp_set_time_sync_notification_cb(sntp_sync_time_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP: starting sync (pool.ntp.org, time.google.com, time.cloudflare.com)...");

    for (int i = 0; i < 50; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
        sntp_sync_status_t st = sntp_get_sync_status();
        if (st == SNTP_SYNC_STATUS_COMPLETED) {
            time_t now;
            time(&now);
            ESP_LOGI(TAG, "SNTP: sync completed (sec=%ld) after %.1f s", (long)now, (i + 1) * 0.2f);
            break;
        }
        if (i > 0 && i % 10 == 0) {
            const char *st_str = (st == SNTP_SYNC_STATUS_RESET) ? "RESET" :
                                (st == SNTP_SYNC_STATUS_IN_PROGRESS) ? "IN_PROGRESS" : "UNKNOWN";
            ESP_LOGI(TAG, "SNTP: waiting... status=%s (%.1f s elapsed)", st_str, (i + 1) * 0.2f);
        }
    }
    sntp_sync_status_t final = sntp_get_sync_status();
    if (final == SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "SNTP: sync OK - file timestamps will be correct");
    } else {
        const char *st_str = (final == SNTP_SYNC_STATUS_RESET) ? "RESET" :
                            (final == SNTP_SYNC_STATUS_IN_PROGRESS) ? "IN_PROGRESS" : "UNKNOWN";
        ESP_LOGW(TAG, "SNTP: not synced after 10 s (status=%s); file dates may be wrong until sync completes in background", st_str);
    }
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

    // Keep running (CSV logging writes to SD when mounted, else SPIFFS; start via web UI)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
