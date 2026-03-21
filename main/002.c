#include "driver/ledc.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_oneshot.h" // Needed for channel mapping
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>

#include "esp_sntp.h"

#include "display.h"
#include "sdcard.h"
#include "server.h"
#include "wifi.h"

static const char *TAG = "LASER_ADC";

// 4000 samples per second -> 4 per ms

#define WIFI 1
#define HOST 2
#define MODE WIFI
// #define MODE HOST

// Pin mapping (ESP32-S3)
#define LASER_GPIO 5 // PWM output to laser
#define ADC_GPIO 4   // ADC input from photodiode/sensor (ADC1_CH3 on S3)

// LEDC config
#define LASER_LEDC_MODE LEDC_LOW_SPEED_MODE
#define LASER_LEDC_TIMER LEDC_TIMER_0
#define LASER_LEDC_CH LEDC_CHANNEL_0
#define LASER_LEDC_FREQ 5000 // 5 kHz
#define LASER_LEDC_RES LEDC_TIMER_8_BIT
#define LASER_DUTY_FULL ((1 << LASER_LEDC_RES) - 1) // 255

// ADC sampling config
#define SAMPLE_RATE_HZ                                                         \
    3000 // Target: samples per second (minimum: ~611 Hz for ESP32-S3)
#define ADC_MIN_FREQ_HZ                                                        \
    611 // Minimum sampling frequency for ESP32-S3 continuous ADC
#define ADC_CONTINUOUS_BUF_SIZE                                                \
    4096 // Buffer for continuous ADC (must be multiple of
         // SOC_ADC_DIGI_RESULT_BYTES)
#define ADC_READ_TIMEOUT_MS 100 // Timeout for reading from continuous ADC

// SPIFFS config (fallback when SD card not mounted)
#define SPIFFS_MOUNT_POINT "/spiffs"
// CSV directory on SD (same base as bench; used when sdcard_is_mounted())
#define CSV_SD_DIR SDCARD_MOUNT_POINT "/laser"
#define CSV_FILE_PATH_MAX_LEN 64 // Maximum length for CSV filename
#define CSV_QUEUE_SIZE                                                         \
    48000 // ADC sample queue size (~288KB in SPIRAM, holds ~6 seconds at 4kHz;
          // helps prevent data loss during SD/SSE delays)
#define SAMPLE_LIMIT 10000000 // Auto-stop after this many samples

// CSV write buffer config
#define CSV_WRITE_BATCH_SIZE 500 // Write CSV in batches to reduce flash wear
#define CSV_FLUSH_INTERVAL 5     // Flush every N batches (reduces flash wear)

// Chunked logging: continuous capture split into time-based chunks (no cycles,
// no threshold)
#define CHUNK_CONTINUOUS_SECS 50 // Seconds per chunk (change as needed)
#define CSV_CHUNK_QUEUE_SIZE 16  // Max pending chunks for client download
#define CSV_CHUNK_DIR_SD CSV_SD_DIR "/chunks"
#define CSV_CHUNK_DIR_SPIFFS SPIFFS_MOUNT_POINT
#define TESTBENCH_DIR_SD SDCARD_MOUNT_POINT "/tb/chunks"
#define TESTBENCH_DIR_SPIFFS SPIFFS_MOUNT_POINT "/tb/cks"

static adc_continuous_handle_t adc_handle = NULL;
static adc_channel_t adc_chan;
static adc_unit_t adc_unit;
static uint32_t s_actual_sample_rate_hz =
    SAMPLE_RATE_HZ; // Actual rate used (may be clamped to minimum)

// RTOS primitives
static SemaphoreHandle_t s_adc_value_mutex = NULL;
static SemaphoreHandle_t s_csv_file_mutex = NULL; // Mutex for CSV file access
static SemaphoreHandle_t s_chunk_file_mutex =
    NULL; // Mutex for chunk file SD access (writer + HTTP reader)
static SemaphoreHandle_t s_fprintf_mutex =
    NULL; // Mutex for fprintf() protection (fprintf is not thread-safe)

// CSV sample structure for queue
typedef struct {
    int adc_value;
    uint64_t timestamp_us;
} csv_sample_t;

/** Binary bench record (must match tools/bin2csv.py). Packed for fixed 12-byte
 * records. */
typedef struct {
    uint64_t timestamp_us;
    int32_t adc_value;
} __attribute__((packed)) bench_record_t;

static QueueHandle_t s_csv_queue = NULL;
/** CSV queue storage in SPIRAM (~72KB) to free internal RAM; never freed. */
static uint8_t *s_csv_queue_storage = NULL;
static StaticQueue_t s_csv_queue_struct;

// Bench laser: run for T seconds, write ADC samples to SD. Test CSV and binary
// separately (one file per run). Stream in chunks to avoid allocating 160KB
// internal RAM (fits in ~16KB).
#define BENCH_LASER_DURATION_SEC 30
#define BENCH_LASER_CHUNK_SIZE 1000
#define BENCH_LASER_DIR SDCARD_MOUNT_POINT "/laser"
#define BENCH_LASER_CSV_PATH BENCH_LASER_DIR "/data.csv"
#define BENCH_LASER_BIN_PATH BENCH_LASER_DIR "/data.bin"
/* Max ms to wait for one chunk before giving up (e.g. 1000/4000 = 250 ms at 4
 * kHz; use ~1 s). */
#define BENCH_LASER_CHUNK_TIMEOUT_MS 1000

typedef enum { BENCH_FMT_CSV, BENCH_FMT_BIN } bench_fmt_t;

/* Unix time for 2020-01-01 00:00:00 UTC; reject file mtime if system time is
 * before this (not set or epoch). */
#define SANE_TIME_MIN_SEC ((time_t)1577836800)

/** SNTP sync callback - logs when time sync completes (including background
 * retries). */
static void sntp_sync_time_cb(struct timeval *tv) {
    time_t sec = tv ? (time_t)tv->tv_sec : 0;
    ESP_LOGI(TAG, "SNTP: sync completed (callback) - system time set, sec=%ld",
             (long)sec);
}

/** Set file access and modification time to current time (so PC shows correct
 * "date modified" when SD is plugged in). Only sets time if system clock has
 * been set (e.g. via SNTP when WiFi STA is connected); otherwise skips to avoid
 * 1970. */
static void set_file_mtime_now(const char *path) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG,
                 "set_file_mtime: gettimeofday failed, skipping utime for %s",
                 path);
        return;
    }
    if (tv.tv_sec < SANE_TIME_MIN_SEC) {
        ESP_LOGW(TAG,
                 "set_file_mtime: system time not set (sec=%ld), skipping "
                 "utime for %s (init SNTP in STA mode for correct time)",
                 (long)tv.tv_sec, path);
        return;
    }
    struct utimbuf ut = {
        .actime = (time_t)tv.tv_sec,
        .modtime = (time_t)tv.tv_sec,
    };
    if (utime(path, &ut) != 0) {
        ESP_LOGW(TAG, "utime %s failed (errno=%d)", path, errno);
    }
}

static volatile bool s_bench_mode = false;
static csv_sample_t *s_bench_buffer = NULL;
static volatile uint32_t s_bench_count = 0;
static volatile bool s_bench_chunk_ready = false; // chunk full, ready to write

// Shared state for web API
static int s_current_adc_value = 0;
static int s_sample_count = 0;
static bool s_adc_stop_requested =
    false;                         // Flag to request ADC stop from ADC task
static bool s_adc_running = false; // Track if ADC is currently running
static bool s_csv_logging_enabled = false; // Track if CSV logging is enabled

// Timing for rate calculation
static uint64_t s_logging_start_time_us = 0; // Start time in microseconds
static uint64_t s_logging_stop_time_us = 0;  // Stop time in microseconds

// CSV file handle
static FILE *s_csv_file = NULL;
static char s_csv_file_path[CSV_FILE_PATH_MAX_LEN] =
    "/spiffs/data.csv"; // Dynamic CSV filename

// Sample index for consistent timestamps (reset on each logging session)
static uint32_t s_csv_sample_index = 0;

// Chunked logging state
#define CHUNK_PATH_MAX 128
typedef struct {
    int index; // -1 = stream complete (no more chunks)
    char path[CHUNK_PATH_MAX];
    size_t size_bytes;
} chunk_ready_item_t;

static QueueHandle_t s_chunk_ready_queue =
    NULL; // Server reads from this for SSE
static SemaphoreHandle_t s_chunk_stop_semaphore =
    NULL; // FreeRTOS semaphore for immediate stop signal
static volatile bool s_chunked_logging_enabled = false;
static volatile bool s_chunk_stop_requested = false;

// Chunked writer state (only valid when s_chunked_logging_enabled)
static FILE *s_chunk_file = NULL;
static char s_chunk_file_path[CHUNK_PATH_MAX] = {0};
#define CHUNK_DIR_MAX 100
static char s_chunk_dir[CHUNK_DIR_MAX] = {
    0}; // Active chunk directory (set in start_chunked_logging)
static int s_chunk_index = 0;
static int s_chunks_ready_count = 0;
static uint64_t s_chunk_write_start_us =
    0;                                    // When current chunk started writing
static uint32_t s_chunk_sample_index = 0; // Per-chunk sample count
static uint32_t s_chunk_global_sample_index =
    0; // Never reset; timestamps continue across chunks
static int s_testbench_run_index =
    0; // Persists across calls; incremented per test bench run
static bool s_testbench_mode = false;

// Legacy getters kept for server.c compatibility (simplified for continuous
// mode) s_chunk_triggered is always true once logging starts (no threshold)
// s_chunk_phase is always 0 (always writing, no skip/pause)

// Forward declarations (used before definitions)
void clear_spiffs_storage(void);
void stop_csv_logging(void);
static void generate_csv_filename(void);
static void bench_laser_run_one(bench_fmt_t fmt);
bool start_chunked_logging(bool testbench);
void stop_chunked_logging(void);

static void laser_init_full_on(void) {
    ledc_timer_config_t tcfg = {
        .speed_mode = LASER_LEDC_MODE,
        .duty_resolution = LASER_LEDC_RES,
        .timer_num = LASER_LEDC_TIMER,
        .freq_hz = LASER_LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ccfg = {
        .speed_mode = LASER_LEDC_MODE,
        .channel = LASER_LEDC_CH,
        .timer_sel = LASER_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LASER_GPIO,
        .duty = LASER_DUTY_FULL,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
    ESP_ERROR_CHECK(ledc_update_duty(LASER_LEDC_MODE, LASER_LEDC_CH));
    ESP_LOGI(TAG, "Laser PWM set full on @ GPIO%d", LASER_GPIO);
}

static void adc_init_gpio4(void) {
    // Map GPIO to ADC unit/channel dynamically (using oneshot API to get
    // channel info) Note: We use oneshot API just to get channel mapping, then
    // use continuous API
    adc_oneshot_unit_handle_t oneshot_handle;
    adc_oneshot_unit_init_cfg_t oneshot_unit_cfg = {
        .unit_id =
            ADC_UNIT_1, // Try ADC1 first (GPIO4 is typically ADC1_CH3 on S3)
    };
    esp_err_t ret = adc_oneshot_new_unit(&oneshot_unit_cfg, &oneshot_handle);
    if (ret != ESP_OK) {
        // Try ADC2 if ADC1 fails
        oneshot_unit_cfg.unit_id = ADC_UNIT_2;
        ESP_ERROR_CHECK(
            adc_oneshot_new_unit(&oneshot_unit_cfg, &oneshot_handle));
    }

    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(ADC_GPIO, &adc_unit, &adc_chan));
    adc_oneshot_del_unit(oneshot_handle); // Clean up oneshot handle, we only
                                          // needed it for channel mapping

    // Configure continuous ADC handle
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = ADC_CONTINUOUS_BUF_SIZE,
        .conv_frame_size = ADC_CONTINUOUS_BUF_SIZE,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc_handle));

    // Configure ADC pattern (single channel)
    adc_digi_pattern_config_t adc_pattern = {
        .atten = ADC_ATTEN_DB_12, // up to ~3.3V
        .channel = adc_chan,
        .unit = adc_unit,
        .bit_width = ADC_BITWIDTH_12,
    };

    // Validate and adjust sample frequency (ESP32-S3 minimum is ~611 Hz)
    s_actual_sample_rate_hz = SAMPLE_RATE_HZ;
    if (SAMPLE_RATE_HZ < ADC_MIN_FREQ_HZ) {
        ESP_LOGW(
            TAG,
            "Sample rate %d Hz is below minimum (%d Hz), using %d Hz instead",
            SAMPLE_RATE_HZ, ADC_MIN_FREQ_HZ, ADC_MIN_FREQ_HZ);
        s_actual_sample_rate_hz = ADC_MIN_FREQ_HZ;
    }

    // Configure continuous ADC
    adc_continuous_config_t cont_cfg = {
        .pattern_num = 1,
        .adc_pattern = &adc_pattern,
        .sample_freq_hz = s_actual_sample_rate_hz,
        .conv_mode = (adc_unit == ADC_UNIT_1) ? ADC_CONV_SINGLE_UNIT_1
                                              : ADC_CONV_SINGLE_UNIT_2,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &cont_cfg));

    ESP_LOGI(TAG,
             "ADC continuous mode initialized on GPIO%d (unit %d, channel %d) "
             "@ %d Hz",
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
            ESP_LOGI(TAG, "ADC continuous sampling started @ %d Hz",
                     s_actual_sample_rate_hz);
        } else if (ret == ESP_ERR_INVALID_STATE) {
            // ADC is already started, just update our flag
            s_adc_running = true;
            ESP_LOGI(TAG, "ADC continuous sampling already running @ %d Hz",
                     s_actual_sample_rate_hz);
        } else {
            ESP_LOGE(TAG, "Failed to start ADC continuous sampling: %s",
                     esp_err_to_name(ret));
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
    adc_continuous_data_t *parsed_samples = heap_caps_malloc(
        sizeof(adc_continuous_data_t) * 256, MALLOC_CAP_DEFAULT);
    if (parsed_samples == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer for parsed ADC samples");
        vTaskDelete(NULL);
        return;
    }

    uint32_t dropped_samples = 0;
    const uint64_t chunk_interval_us =
        (s_actual_sample_rate_hz > 0) ? (1000000ULL / s_actual_sample_rate_hz)
                                       : 0;

    ESP_LOGI(TAG, "ADC processing task started");

    while (1) {
        if (adc_handle == NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Check if ADC stop was requested (currently not used, but kept for
        // future use)
        if (s_adc_stop_requested) {
            // Note: We don't actually stop ADC anymore to avoid mutex crashes
            // ADC keeps running for web UI, we just stop logging
            s_adc_stop_requested = false;
            ESP_LOGW(TAG,
                     "ADC stop requested but ignored - ADC must stay running");
            continue;
        }

        // Skip reading if ADC is not running
        if (!s_adc_running) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Read and parse samples directly from continuous ADC buffer
        uint32_t num_samples = 256; // Maximum number of samples to parse
        esp_err_t parse_ret = adc_continuous_read_parse(
            adc_handle, parsed_samples, 256, &num_samples, ADC_READ_TIMEOUT_MS);

        if (parse_ret == ESP_OK && num_samples > 0) {
            // Process each parsed sample
            for (uint32_t i = 0; i < num_samples; i++) {
                if (!parsed_samples[i].valid) {
                    dropped_samples++;
                    continue;
                }

                int raw_value = (int)parsed_samples[i].raw_data;

                // Update current value for web UI (always update when ADC is
                // running) (protected by mutex)
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

                // Bench laser: fill chunk; when full set chunk_ready and pause
                // until reset
                if (s_bench_mode && s_bench_buffer != NULL) {
                    if (s_bench_chunk_ready) {
                        continue; // wait for bench_laser_run_one() to write and
                                  // reset
                    }
                    if (s_bench_count < BENCH_LASER_CHUNK_SIZE) {
                        s_bench_buffer[s_bench_count].adc_value = raw_value;
                        s_bench_buffer[s_bench_count].timestamp_us =
                            esp_timer_get_time();
                        s_bench_count++;
                        if (s_bench_count >= BENCH_LASER_CHUNK_SIZE) {
                            s_bench_chunk_ready = true;
                        }
                    }
                    continue;
                }

                // Auto-stop when reaching sample limit
                if (s_csv_logging_enabled && s_sample_count >= SAMPLE_LIMIT) {
                    ESP_LOGI(TAG, "Sample limit (%d) reached, stopping logging",
                             SAMPLE_LIMIT);
                    stop_sampling_timer(); // Stop ADC sampling
                    stop_csv_logging();    // Stop CSV logging
                    // After stopping, skip enqueueing further samples
                    continue;
                }

                // No-buffer chunked mode:
                // - ADC writes directly to the active chunk file.
                // - If the chunk mutex is held by HTTP/rotation, we drop samples
                //   immediately (intentional "ACCEPT lost").
                if (s_chunked_logging_enabled) {
                    if (s_chunk_file_mutex != NULL &&
                        xSemaphoreTake(s_chunk_file_mutex, 0) == pdTRUE) {
                        if (s_chunk_file != NULL) {
                            uint64_t ts =
                                (uint64_t)s_chunk_global_sample_index *
                                chunk_interval_us;
                            fprintf(s_chunk_file, "%llu,%d\n",
                                    (unsigned long long)ts, raw_value);
                            s_chunk_global_sample_index++;
                            s_chunk_sample_index++;
                        }
                        xSemaphoreGive(s_chunk_file_mutex);
                    } else {
                        dropped_samples++;
                    }
                } else if (s_csv_logging_enabled && s_csv_queue != NULL) {
                    // Continuous mode keeps the queue to avoid SD/HTTP latency
                    // blocking the ADC.
                    csv_sample_t sample = {.adc_value = raw_value,
                                           .timestamp_us =
                                               esp_timer_get_time()};
                    if (xQueueSend(s_csv_queue, &sample, 0) != pdTRUE) {
                        // Queue full - log warning periodically
                        static uint32_t queue_full_count = 0;
                        queue_full_count++;
                        if (queue_full_count % 5000 == 0) {
                            ESP_LOGW(TAG,
                                     "CSV queue full (%d/%d), samples dropped "
                                     "(total ~%lu)",
                                     uxQueueMessagesWaiting(s_csv_queue),
                                     CSV_QUEUE_SIZE,
                                     (unsigned long)queue_full_count);
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
            ESP_LOGW(TAG, "Dropped samples so far: %lu",
                     (unsigned long)dropped_samples);
        }
    }

    free(parsed_samples);
    vTaskDelete(NULL);
}

// CSV writer task: reads samples from queue and writes to CSV file
static void csv_writer_task(void *pvParameters) {
    csv_sample_t batch[CSV_WRITE_BATCH_SIZE];
    uint32_t batch_count = 0;
    // Use actual sample rate for timestamp calculation (may be clamped to
    // minimum)
    const uint64_t sample_interval_us = 1000000ULL / s_actual_sample_rate_hz;

    ESP_LOGI(TAG, "CSV writer task started");

    while (1) {
        /* ========== Chunked mode (continuous capture, time-based chunks)
         * ========== */
        if (s_chunked_logging_enabled) {
            uint64_t now_us = esp_timer_get_time();

            // Check if current chunk should be closed (time expired or stop
            // requested)
            bool close_chunk = false;
            if (s_chunk_stop_requested && s_chunk_file != NULL) {
                close_chunk = true;
            } else if (s_chunk_file != NULL) {
                uint64_t elapsed_us = now_us - s_chunk_write_start_us;
                if (elapsed_us >=
                    (uint64_t)CHUNK_CONTINUOUS_SECS * 1000000ULL) {
                    close_chunk = true;
                }
            }

            if (close_chunk) {
                uint32_t samples_in_chunk = 0;
                size_t sz = 0;
                int ready_index = -1;
                bool stop_now = false;
                char closed_path[CHUNK_PATH_MAX] = {0};

                // Hold mutex across close+rotate so ADC never writes to the
                // wrong chunk file.
                if (s_chunk_file_mutex != NULL)
                    xSemaphoreTake(s_chunk_file_mutex, portMAX_DELAY);

                samples_in_chunk = s_chunk_sample_index;
                ready_index = s_chunk_index;
                strncpy(closed_path, s_chunk_file_path,
                        CHUNK_PATH_MAX - 1);
                closed_path[CHUNK_PATH_MAX - 1] = '\0';

                if (s_chunk_file != NULL) {
                    fflush(s_chunk_file);
                    fsync(fileno(s_chunk_file));
                    fclose(s_chunk_file);
                    s_chunk_file = NULL;
                }

                // Update timestamp + size for the closed chunk
                set_file_mtime_now(s_chunk_file_path);
                struct stat st;
                if (stat(s_chunk_file_path, &st) == 0)
                    sz = (size_t)st.st_size;

                s_chunks_ready_count = ready_index + 1;
                stop_now = s_chunk_stop_requested;

                if (!stop_now) {
                    // Open next chunk file while still holding mutex.
                    s_chunk_index++;
                    s_chunk_sample_index = 0;
                    s_chunk_write_start_us = esp_timer_get_time();

                    snprintf(s_chunk_file_path, CHUNK_PATH_MAX, "%s/%d.csv",
                             s_chunk_dir, s_chunk_index);

                    s_chunk_file = fopen(s_chunk_file_path, "w");
                    if (s_chunk_file != NULL) {
                        if (s_fprintf_mutex != NULL)
                            xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
                        fprintf(s_chunk_file, "timestamp_us,adc_value\n");
                        if (s_fprintf_mutex != NULL)
                            xSemaphoreGive(s_fprintf_mutex);
                        fflush(s_chunk_file);
                        fsync(fileno(s_chunk_file));
                    } else {
                        ESP_LOGE(TAG,
                                 "Failed to open chunk file: %s (errno=%d)",
                                 s_chunk_file_path, errno);
                    }
                }

                if (s_chunk_file_mutex != NULL)
                    xSemaphoreGive(s_chunk_file_mutex);

                // Notify: update count (polling API) + non-blocking push to
                // legacy SSE queue
                chunk_ready_item_t item = {
                    .index = ready_index,
                    .size_bytes = sz,
                };
                strncpy(item.path, closed_path, CHUNK_PATH_MAX - 1);
                item.path[CHUNK_PATH_MAX - 1] = '\0';
                if (s_chunk_ready_queue != NULL) {
                    xQueueSend(s_chunk_ready_queue, &item, 0);
                }
                ESP_LOGI(TAG, "Chunk %d ready: %zu bytes, %lu samples",
                         ready_index, sz,
                         (unsigned long)samples_in_chunk);

                // If stop was requested, we're done
                if (stop_now) {
                    stop_sampling_timer();
                    chunk_ready_item_t done = {
                        .index = -1, .path = {0}, .size_bytes = 0};
                    if (s_chunk_ready_queue != NULL)
                        xQueueSend(s_chunk_ready_queue, &done, 0);
                    s_chunk_stop_requested = false;
                    if (s_chunk_stop_semaphore != NULL)
                        xSemaphoreTake(s_chunk_stop_semaphore, 0);
                    ESP_LOGI(TAG, "Chunked logging stopped. Total samples: %lu",
                             (unsigned long)s_chunk_global_sample_index);
                    s_chunked_logging_enabled = false;
                    continue;
                }
            }

            // No rotation needed; yield so ADC can acquire the mutex.
            // With CONFIG_FREERTOS_HZ=100, pdMS_TO_TICKS(5) becomes 0, which
            // can starve IDLE0 and trigger task_wdt.
            vTaskDelay(pdMS_TO_TICKS(20));
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
                if (s_csv_file_mutex != NULL &&
                    xSemaphoreTake(s_csv_file_mutex, pdMS_TO_TICKS(1000)) ==
                        pdTRUE) {
                    if (s_csv_file != NULL) {
                        // Format and write batch to CSV with calculated
                        // timestamps (consistent interval)
                        if (s_fprintf_mutex != NULL)
                            xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
                        for (uint32_t i = 0; i < batch_count; i++) {
                            // Calculate timestamp based on sample index and
                            // target rate
                            uint64_t timestamp_us =
                                s_csv_sample_index * sample_interval_us;
                            fprintf(s_csv_file, "%llu,%d\n",
                                    (unsigned long long)timestamp_us,
                                    batch[i].adc_value);
                            s_csv_sample_index++;
                        }
                        if (s_fprintf_mutex != NULL)
                            xSemaphoreGive(s_fprintf_mutex);
                        // Flush periodically (every N batches) to reduce flash
                        // wear
                        static uint32_t flush_counter = 0;
                        flush_counter++;
                        if (flush_counter >= CSV_FLUSH_INTERVAL ||
                            !s_csv_logging_enabled) {
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
                if (s_csv_file_mutex != NULL &&
                    xSemaphoreTake(s_csv_file_mutex, pdMS_TO_TICKS(1000)) ==
                        pdTRUE) {
                    if (s_csv_file != NULL) {
                        // Format and write final batch with calculated
                        // timestamps
                        if (s_fprintf_mutex != NULL)
                            xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
                        for (uint32_t i = 0; i < batch_count; i++) {
                            uint64_t timestamp_us =
                                s_csv_sample_index * sample_interval_us;
                            fprintf(s_csv_file, "%llu,%d\n",
                                    (unsigned long long)timestamp_us,
                                    batch[i].adc_value);
                            s_csv_sample_index++;
                        }
                        if (s_fprintf_mutex != NULL)
                            xSemaphoreGive(s_fprintf_mutex);
                        fflush(s_csv_file); // Always flush on final batch
                        ESP_LOGI(TAG,
                                 "Final batch written (timeout): %d samples "
                                 "(total written: %d)",
                                 batch_count, s_csv_sample_index);
                    }
                    xSemaphoreGive(s_csv_file_mutex);
                }
                batch_count = 0;
            }
        }

        // Check for final batch immediately after logging stops (handles case
        // where queue is empty but batch has data)
        if (!s_csv_logging_enabled && batch_count > 0 &&
            (s_csv_queue == NULL || uxQueueMessagesWaiting(s_csv_queue) == 0)) {
            // Write final batch to CSV file
            if (s_csv_file_mutex != NULL &&
                xSemaphoreTake(s_csv_file_mutex, pdMS_TO_TICKS(1000)) ==
                    pdTRUE) {
                if (s_csv_file != NULL) {
                    // Format and write final batch with calculated timestamps
                    if (s_fprintf_mutex != NULL)
                        xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
                    for (uint32_t i = 0; i < batch_count; i++) {
                        uint64_t timestamp_us =
                            s_csv_sample_index * sample_interval_us;
                        fprintf(s_csv_file, "%llu,%d\n",
                                (unsigned long long)timestamp_us,
                                batch[i].adc_value);
                        s_csv_sample_index++;
                    }
                    if (s_fprintf_mutex != NULL)
                        xSemaphoreGive(s_fprintf_mutex);
                    fflush(s_csv_file); // Always flush on final batch
                    ESP_LOGI(TAG,
                             "Final batch written (immediate): %d samples "
                             "(total written: %d)",
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
    if (s_chunked_logging_enabled) {
        return (int)s_chunk_global_sample_index;
    }
    return (int)s_csv_sample_index;
}

uint32_t get_actual_sample_rate_hz(void) { return s_actual_sample_rate_hz; }

int get_chunk_continuous_secs(void) { return CHUNK_CONTINUOUS_SECS; }

// Get logging statistics (sample count, elapsed time, and rate)
void get_logging_stats(int *sample_count, uint64_t *elapsed_time_ms,
                       float *rate_hz) {
    uint32_t idx = s_chunked_logging_enabled ? s_chunk_global_sample_index
                                             : s_csv_sample_index;
    *sample_count = (int)idx;

    if (idx > 0 && s_actual_sample_rate_hz > 0) {
        uint64_t elapsed_us =
            ((uint64_t)idx * 1000000ULL) / s_actual_sample_rate_hz;
        *elapsed_time_ms = elapsed_us / 1000;
        *rate_hz = (float)s_actual_sample_rate_hz;
    } else {
        *elapsed_time_ms = 0;
        *rate_hz = 0.0f;
    }
}

// Get sample queue (for server.c compatibility)
QueueHandle_t get_sample_queue(void) { return s_csv_queue; }

// Get SPIFFS storage info
void get_spiffs_storage_info(size_t *total_bytes, size_t *used_bytes) {
    esp_err_t ret = esp_spiffs_info(NULL, total_bytes, used_bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS info: %s", esp_err_to_name(ret));
        *total_bytes = 0;
        *used_bytes = 0;
    }
}

// Generate CSV path: SD card when mounted, else SPIFFS (avoids crash when SD
// absent)
static void generate_csv_filename(void) {
    const char *base_dir;
    if (sdcard_is_mounted()) {
        base_dir = CSV_SD_DIR;
    } else {
        base_dir = SPIFFS_MOUNT_POINT;
    }
    snprintf(s_csv_file_path, sizeof(s_csv_file_path), "%s/data.csv", base_dir);
    ESP_LOGI(TAG, "CSV path: %s", s_csv_file_path);
}

// Start ADC sampling and CSV logging
void start_csv_logging(void) {
    // Generate CSV filename based on actual and attempted rates
    generate_csv_filename();

    // Protect file access with mutex
    if (s_csv_file_mutex != NULL &&
        xSemaphoreTake(s_csv_file_mutex, portMAX_DELAY) == pdTRUE) {
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
            // Ensure CSV directory exists on SD. Use mode 0 for FAT (0755 can
            // cause EINVAL on FatFS).
            if (mkdir(CSV_SD_DIR, 0) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "mkdir %s failed (errno=%d)", CSV_SD_DIR, errno);
            }
        } else {
            // SPIFFS: check space and optionally reformat if nearly full
            size_t total = 0, used = 0;
            esp_err_t ret = esp_spiffs_info(NULL, &total, &used);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "SPIFFS: %d KB used of %d KB total", used / 1024,
                         total / 1024);
                if (used > (total * 9 / 10)) {
                    ESP_LOGW(TAG, "SPIFFS nearly full (%d%%), formatting...",
                             (used * 100) / total);
                    esp_vfs_spiffs_unregister(NULL);
                    esp_vfs_spiffs_conf_t conf = {
                        .base_path = SPIFFS_MOUNT_POINT,
                        .partition_label = NULL,
                        .max_files = 5,
                        .format_if_mount_failed = true};
                    if (esp_vfs_spiffs_register(&conf) != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to reformat SPIFFS");
                    }
                }
            }
        }

        // Open CSV file for writing
        s_csv_file = fopen(s_csv_file_path, "w");
        if (s_csv_file == NULL) {
            ESP_LOGE(TAG, "Failed to open CSV file: %s (errno: %d)",
                     s_csv_file_path, errno);
            if (!use_sd &&
                errno == 28) { // ENOSPC on SPIFFS: reformat and retry once
                esp_vfs_spiffs_unregister(NULL);
                esp_vfs_spiffs_conf_t conf = {.base_path = SPIFFS_MOUNT_POINT,
                                              .partition_label = NULL,
                                              .max_files = 5,
                                              .format_if_mount_failed = true};
                if (esp_vfs_spiffs_register(&conf) == ESP_OK) {
                    s_csv_file = fopen(s_csv_file_path, "w");
                    if (s_csv_file != NULL) {
                        if (s_fprintf_mutex != NULL)
                            xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
                        fprintf(s_csv_file, "timestamp_us,adc_value\n");
                        if (s_fprintf_mutex != NULL)
                            xSemaphoreGive(s_fprintf_mutex);
                        fflush(s_csv_file);
                        ESP_LOGI(TAG,
                                 "CSV file opened after SPIFFS reformat: %s",
                                 s_csv_file_path);
                    }
                }
            }
        } else {
            if (s_fprintf_mutex != NULL)
                xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
            fprintf(s_csv_file, "timestamp_us,adc_value\n");
            if (s_fprintf_mutex != NULL)
                xSemaphoreGive(s_fprintf_mutex);
            fflush(s_csv_file);
            ESP_LOGI(TAG, "CSV file opened: %s", s_csv_file_path);
        }
        xSemaphoreGive(s_csv_file_mutex);
    }

    // Only enable logging and start ADC if file opened (avoid silent drop when
    // SD/SPIFFS open fails)
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
    ESP_LOGI(TAG, "ADC sampling started @ %d Hz (CSV logging to %s)",
             SAMPLE_RATE_HZ, sdcard_is_mounted() ? "SD" : "SPIFFS");
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
            // Queue has been empty for 3 consecutive checks (30ms) - likely
            // fully drained
            if (queue_empty_count >= 3) {
                break;
            }
        } else {
            queue_empty_count = 0; // Reset counter if queue has items
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Give writer task additional time to write any remaining batch
    // The writer task processes batches, so we need to wait for it to finish
    // Wait up to 500ms to ensure all batches are written
    for (int i = 0; i < 50; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        // Try to take mutex briefly - if we can get it, writer task is not
        // writing
        if (s_csv_file_mutex != NULL &&
            xSemaphoreTake(s_csv_file_mutex, 0) == pdTRUE) {
            // We got the mutex, writer task is not writing - safe to close
            xSemaphoreGive(s_csv_file_mutex);
            break;
        }
    }

    // Close CSV file (protected by mutex)
    if (s_csv_file_mutex != NULL &&
        xSemaphoreTake(s_csv_file_mutex, portMAX_DELAY) == pdTRUE) {
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
    ESP_LOGI(
        TAG,
        "ADC sampling and CSV logging stopped. Total CSV samples written: %u",
        (unsigned)s_csv_sample_index);
}

// Clear CSV file (works for path on SD or SPIFFS; server.c compatibility)
void clear_spiffs_storage(void) {
    if (remove(s_csv_file_path) == 0) {
        ESP_LOGI(TAG, "CSV file deleted: %s", s_csv_file_path);
    }
}

// Get CSV file path (exported for server.c)
const char *get_csv_file_path(void) { return s_csv_file_path; }

// Get chunk ready queue (for server SSE) - DEPRECATED: use direct queue
// streaming instead
QueueHandle_t get_chunk_ready_queue(void) { return s_chunk_ready_queue; }

bool is_chunked_logging_active(void) { return s_chunked_logging_enabled; }

int get_chunk_index(void) { return s_chunk_index; }

int get_chunks_ready_count(void) { return s_chunks_ready_count; }

uint32_t get_chunk_global_sample_index(void) {
    return s_chunk_global_sample_index;
}

SemaphoreHandle_t get_chunk_stop_semaphore(void) {
    return s_chunk_stop_semaphore;
}

SemaphoreHandle_t get_chunk_file_mutex(void) { return s_chunk_file_mutex; }

const char *get_chunk_dir(void) { return s_chunk_dir; }

bool start_chunked_logging(bool testbench) {
    if (s_chunked_logging_enabled) {
        ESP_LOGW(TAG, "Chunked logging already active");
        return false;
    }
    s_chunk_stop_requested = false;
    s_chunk_index = 0;
    s_chunks_ready_count = 0;
    s_chunk_sample_index = 0;
    s_chunk_global_sample_index = 0;
    s_testbench_mode = testbench;

    if (s_chunk_stop_semaphore != NULL) {
        xSemaphoreTake(s_chunk_stop_semaphore, 0);
    }

    // Determine chunk directory (create each level; FAT requires parents to
    // exist)
    if (testbench) {
        const char *mount =
            sdcard_is_mounted() ? SDCARD_MOUNT_POINT : SPIFFS_MOUNT_POINT;
        char p[CHUNK_DIR_MAX];

        snprintf(p, sizeof(p), "%s/tb", mount);
        if (mkdir(p, 0) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir %s failed (errno=%d %s)", p, errno,
                     strerror(errno));
        }
        snprintf(p, sizeof(p), "%s/tb/chunks", mount);
        if (mkdir(p, 0) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir %s failed (errno=%d %s)", p, errno,
                     strerror(errno));
        }
        snprintf(s_chunk_dir, CHUNK_DIR_MAX, "%s/tb/chunks/run_%d", mount,
                 s_testbench_run_index);
        if (mkdir(s_chunk_dir, 0) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "mkdir %s failed (errno=%d %s)", s_chunk_dir, errno,
                     strerror(errno));
            return false;
        }
        s_testbench_run_index++;
    } else {
        const char *dir =
            sdcard_is_mounted() ? CSV_CHUNK_DIR_SD : CSV_CHUNK_DIR_SPIFFS;
        snprintf(s_chunk_dir, CHUNK_DIR_MAX, "%s", dir);
        if (sdcard_is_mounted()) {
            // FAT requires parent directories to exist.
            mkdir(CSV_SD_DIR, 0);
            if (mkdir(s_chunk_dir, 0) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "mkdir %s failed (errno=%d)", s_chunk_dir,
                         errno);
                return false;
            }
        } else {
            // SPIFFS mount point already exists; write chunk files directly as
            // "/spiffs/<index>.csv" (no "/spiffs/chunks" subdir).
        }
    }

    // Open first chunk file
    if (s_chunk_file_mutex != NULL)
        xSemaphoreTake(s_chunk_file_mutex, portMAX_DELAY);
    snprintf(s_chunk_file_path, CHUNK_PATH_MAX, "%s/0.csv", s_chunk_dir);
    s_chunk_file = fopen(s_chunk_file_path, "w");
    if (s_chunk_file == NULL) {
        if (s_chunk_file_mutex != NULL)
            xSemaphoreGive(s_chunk_file_mutex);
        ESP_LOGE(TAG, "Failed to open chunk file: %s (errno=%d)",
                 s_chunk_file_path, errno);
        return false;
    }
    if (s_fprintf_mutex != NULL)
        xSemaphoreTake(s_fprintf_mutex, portMAX_DELAY);
    fprintf(s_chunk_file, "timestamp_us,adc_value\n");
    if (s_fprintf_mutex != NULL)
        xSemaphoreGive(s_fprintf_mutex);
    fflush(s_chunk_file);
    fsync(fileno(s_chunk_file));
    if (s_chunk_file_mutex != NULL)
        xSemaphoreGive(s_chunk_file_mutex);

    if (s_csv_queue)
        xQueueReset(s_csv_queue);
    if (s_chunk_ready_queue)
        xQueueReset(s_chunk_ready_queue);
    s_chunk_write_start_us = esp_timer_get_time();
    s_chunked_logging_enabled = true;
    start_sampling_timer();
    ESP_LOGI(
        TAG,
        "Chunked logging started: continuous capture, %d s/chunk, dir=%s%s",
        CHUNK_CONTINUOUS_SECS, s_chunk_dir, testbench ? " [TESTBENCH]" : "");
    return true;
}

void stop_chunked_logging(void) {
    if (!s_chunked_logging_enabled) {
        stop_sampling_timer();
        return;
    }
    s_chunk_stop_requested = true;
    if (s_chunk_stop_semaphore != NULL) {
        xSemaphoreGive(s_chunk_stop_semaphore);
    }
    ESP_LOGI(TAG, "Chunked logging stop requested, waiting for writer...");
    for (int i = 0; i < 100 && s_chunked_logging_enabled; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    stop_sampling_timer();
    if (s_chunked_logging_enabled) {
        ESP_LOGE(TAG, "Chunked logging stop TIMED OUT (5s) — forcing off");
        s_chunked_logging_enabled = false;
    } else {
        ESP_LOGI(TAG, "Chunked logging stop confirmed");
    }
}

/**
 * Bench laser: run for BENCH_LASER_DURATION_SEC seconds, write ADC samples to
 * SD to a single file (CSV or binary). Streams in chunks
 * (BENCH_LASER_CHUNK_SIZE). Binary format matches tools/bin2csv.py (12-byte
 * records: uint64 timestamp_us, int32 adc_value).
 */
static void bench_laser_run_one(bench_fmt_t fmt) {
    const char *path =
        (fmt == BENCH_FMT_CSV) ? BENCH_LASER_CSV_PATH : BENCH_LASER_BIN_PATH;
    const char *label = (fmt == BENCH_FMT_CSV) ? "CSV" : "BIN";

    if (!sdcard_is_mounted()) {
        ESP_LOGW(TAG, "bench_laser: SD card not mounted, skip");
        return;
    }
    if (mkdir(BENCH_LASER_DIR, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "bench_laser: mkdir %s failed (errno=%d %s)",
                 BENCH_LASER_DIR, errno, strerror(errno));
        return;
    }

    size_t buf_size = (size_t)BENCH_LASER_CHUNK_SIZE * sizeof(csv_sample_t);
    s_bench_buffer =
        (csv_sample_t *)heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL);
    if (s_bench_buffer == NULL) {
        ESP_LOGE(TAG, "bench_laser: alloc %u bytes failed", (unsigned)buf_size);
        return;
    }

    FILE *f = (fmt == BENCH_FMT_CSV) ? fopen(BENCH_LASER_CSV_PATH, "w")
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

    ESP_LOGI(TAG, "bench_laser: %s test running for %d seconds...", label,
             BENCH_LASER_DURATION_SEC);

    while (1) {
        int64_t elapsed_us = esp_timer_get_time() - t0;
        if (elapsed_us >= duration_us) {
            break;
        }
        int64_t chunk_t0 = esp_timer_get_time();
        uint32_t elapsed_ms = 0;
        while (!s_bench_chunk_ready &&
               elapsed_ms < BENCH_LASER_CHUNK_TIMEOUT_MS) {
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
                int nr =
                    fprintf(f, "%llu,%d\n",
                            (unsigned long long)s_bench_buffer[i].timestamp_us,
                            s_bench_buffer[i].adc_value);
                if (nr > 0) {
                    bytes_written += (size_t)nr;
                }
            } else {
                bench_record_t rec = {
                    .timestamp_us = s_bench_buffer[i].timestamp_us,
                    .adc_value = (int32_t)s_bench_buffer[i].adc_value,
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
    double samples_per_sec =
        (time_sec > 0 && total_written > 0) ? (total_written / time_sec) : 0;
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(
        TAG,
        " bench_laser %s: ran %.2f s, %u samples -> %.1f samples/s, %u bytes",
        label, time_sec, (unsigned)total_written, samples_per_sec,
        (unsigned)bytes_written);
    ESP_LOGI(TAG, "========================================");
}

// Initialize SPIFFS
static esp_err_t init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {.base_path = SPIFFS_MOUNT_POINT,
                                  .partition_label = NULL,
                                  .max_files = 5,
                                  .format_if_mount_failed = true};

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)",
                     esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d KB, used: %d KB", total / 1024,
                 used / 1024);
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
        ESP_LOGW(TAG,
                 "Display initialization failed - continuing without display");
    }

    // Initialize SPIFFS (fallback for CSV when SD card not mounted)
    ESP_ERROR_CHECK(init_spiffs());

    // Initialize SD card (used for CSV logging when mounted; no read/write test
    // on startup)
    esp_err_t sdcard_ret = sdcard_init();
    if (sdcard_ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card initialized successfully");
        display_show_status("SD card", "OK");
    } else {
        ESP_LOGW(TAG, "SD card initialization failed: %s (CSV will use SPIFFS)",
                 esp_err_to_name(sdcard_ret));
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

    // Create queue for CSV samples; storage in SPIRAM to free internal RAM
    size_t queue_storage_bytes = (size_t)CSV_QUEUE_SIZE * sizeof(csv_sample_t);
    s_csv_queue_storage =
        heap_caps_malloc(queue_storage_bytes, MALLOC_CAP_SPIRAM);
    if (s_csv_queue_storage == NULL) {
        ESP_LOGW(TAG, "CSV queue SPIRAM alloc failed, using internal RAM");
        s_csv_queue_storage =
            heap_caps_malloc(queue_storage_bytes, MALLOC_CAP_INTERNAL);
    }
    if (s_csv_queue_storage == NULL) {
        ESP_LOGE(TAG, "Failed to allocate CSV queue storage (%zu bytes)",
                 queue_storage_bytes);
        return;
    }
    s_csv_queue = xQueueCreateStatic(CSV_QUEUE_SIZE, sizeof(csv_sample_t),
                                     s_csv_queue_storage, &s_csv_queue_struct);
    if (s_csv_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create CSV queue");
        heap_caps_free(s_csv_queue_storage);
        s_csv_queue_storage = NULL;
        return;
    }
    ESP_LOGI(TAG, "CSV queue created (%u slots)", (unsigned)CSV_QUEUE_SIZE);

    // Create queue for chunked SSE (chunk path when ready)
    s_chunk_ready_queue =
        xQueueCreate(CSV_CHUNK_QUEUE_SIZE, sizeof(chunk_ready_item_t));
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
    // Increased stack size to 16384 for large batch array (500 samples * 12
    // bytes = 6KB) + fprintf buffers
    xTaskCreatePinnedToCore(csv_writer_task, "csv_writer", 16384, NULL, 3, NULL,
                            0);

    // Don't start ADC automatically - wait for user to press start button
    // ADC will be started when start button is pressed

    ESP_LOGI(TAG,
             "RTOS architecture initialized: continuous ADC @ %d Hz, CSV "
             "logging enabled",
             SAMPLE_RATE_HZ);

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
    ESP_LOGI(TAG, "SNTP: starting sync (pool.ntp.org, time.google.com, "
                  "time.cloudflare.com)...");

    for (int i = 0; i < 50; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
        sntp_sync_status_t st = sntp_get_sync_status();
        if (st == SNTP_SYNC_STATUS_COMPLETED) {
            time_t now;
            time(&now);
            ESP_LOGI(TAG, "SNTP: sync completed (sec=%ld) after %.1f s",
                     (long)now, (i + 1) * 0.2f);
            break;
        }
        if (i > 0 && i % 10 == 0) {
            const char *st_str = (st == SNTP_SYNC_STATUS_RESET) ? "RESET"
                                 : (st == SNTP_SYNC_STATUS_IN_PROGRESS)
                                     ? "IN_PROGRESS"
                                     : "UNKNOWN";
            ESP_LOGI(TAG, "SNTP: waiting... status=%s (%.1f s elapsed)", st_str,
                     (i + 1) * 0.2f);
        }
    }
    sntp_sync_status_t final = sntp_get_sync_status();
    if (final == SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "SNTP: sync OK - file timestamps will be correct");
    } else {
        const char *st_str = (final == SNTP_SYNC_STATUS_RESET) ? "RESET"
                             : (final == SNTP_SYNC_STATUS_IN_PROGRESS)
                                 ? "IN_PROGRESS"
                                 : "UNKNOWN";
        ESP_LOGW(TAG,
                 "SNTP: not synced after 10 s (status=%s); file dates may be "
                 "wrong until sync completes in background",
                 st_str);
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

    // Keep running (CSV logging writes to SD when mounted, else SPIFFS; start
    // via web UI)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
