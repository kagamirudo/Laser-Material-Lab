#include "server.h"
#include "display.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#define SERVER_TAG "SERVER"

/** Set TZ env from offset minutes so localtime_r() shows local time.
 *  offset_min: e.g. -300 for EST (UTC-5). POSIX format: "XXX5" = 5h behind UTC.
 */
static void set_tz_from_offset_min(int offset_min) {
    int posix_total = -offset_min; /* POSIX sign: west-of-UTC is positive */
    int hours = posix_total / 60;
    int mins = abs(posix_total % 60);
    char tz_buf[24];
    if (mins != 0)
        snprintf(tz_buf, sizeof(tz_buf), "XXX%d:%02d", hours, mins);
    else
        snprintf(tz_buf, sizeof(tz_buf), "XXX%d", hours);
    setenv("TZ", tz_buf, 1);
    tzset();
}

// Time sync status
static bool s_time_synced = false;
static SemaphoreHandle_t s_time_sync_mutex = NULL;
static bool s_display_time_task_started = false;
// Timezone offset in minutes (from client); used for TZ so display shows local
// time
static int s_timezone_offset_min = 0;
static bool s_tz_fetch_started = false;

#define TZ_FETCH_BUF_SIZE 128
static char s_tz_fetch_buf[TZ_FETCH_BUF_SIZE];
static size_t s_tz_fetch_len;

static esp_err_t tz_http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (evt->data_len > 0 &&
            s_tz_fetch_len + evt->data_len < TZ_FETCH_BUF_SIZE) {
            memcpy(s_tz_fetch_buf + s_tz_fetch_len, evt->data, evt->data_len);
            s_tz_fetch_len += evt->data_len;
            s_tz_fetch_buf[s_tz_fetch_len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/** Fetch timezone offset from IP geolocation (ip-api.com). Runs once on boot.
 */
static void fetch_timezone_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Wait for SNTP to likely sync */
    s_tz_fetch_len = 0;
    memset(s_tz_fetch_buf, 0, sizeof(s_tz_fetch_buf));

    esp_http_client_config_t config = {
        .url = "http://ip-api.com/json/?fields=offset",
        .event_handler = tz_http_event_handler,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(SERVER_TAG, "TZ fetch: http client init failed");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(SERVER_TAG, "TZ fetch: request failed (%s)",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* Parse {"offset":-18000} - offset in seconds */
    char *p = strstr(s_tz_fetch_buf, "\"offset\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            long offset_sec = strtol(p + 1, NULL, 10);
            int offset_min =
                (int)(offset_sec / 60); /* -18000 -> -300 for EST */
            set_tz_from_offset_min(offset_min);
            s_timezone_offset_min = offset_min;
            if (s_time_sync_mutex) {
                xSemaphoreTake(s_time_sync_mutex, portMAX_DELAY);
                s_time_synced = true;
                xSemaphoreGive(s_time_sync_mutex);
            } else {
                s_time_synced = true;
            }
            ESP_LOGI(SERVER_TAG,
                     "TZ auto-detected from IP: offset %d min (UTC%+d)",
                     offset_min, offset_min / 60);
        }
    } else {
        ESP_LOGW(SERVER_TAG, "TZ fetch: could not parse offset from response");
    }
    vTaskDelete(NULL);
}

// TLS certificate/key (replace with your real cert + key).
extern const unsigned char
    server_cert_pem_start[] asm("_binary_server_cert_pem_start");
extern const unsigned char
    server_cert_pem_end[] asm("_binary_server_cert_pem_end");
extern const unsigned char
    server_key_pem_start[] asm("_binary_server_key_pem_start");
extern const unsigned char
    server_key_pem_end[] asm("_binary_server_key_pem_end");

// ADC functions from 002.c
bool is_csv_logging_active(void);
int get_current_adc_value(void);
int get_sample_count(void);
uint32_t get_actual_sample_rate_hz(void);
void get_logging_stats(int *sample_count, uint64_t *elapsed_time_ms,
                       float *rate_hz);
void get_spiffs_storage_info(size_t *total_bytes, size_t *used_bytes);
void start_csv_logging(void);
void stop_csv_logging(void);
void clear_spiffs_storage(void);
void stop_sampling_timer(void);
void start_sampling_timer(void);
QueueHandle_t get_sample_queue(void);
const char *get_csv_file_path(void);

// Chunked logging (must match 002.c)
#define CHUNK_PATH_MAX 128
typedef struct {
    int index;
    char path[CHUNK_PATH_MAX];
    size_t size_bytes;
} chunk_ready_item_t;

// CSV sample structure (must match 002.c)
typedef struct {
    int adc_value;
    uint64_t timestamp_us;
} csv_sample_t;

QueueHandle_t get_chunk_ready_queue(void);
bool start_chunked_logging(bool testbench);
void stop_chunked_logging(void);
bool is_chunked_logging_active(void);
int get_chunk_index(void);
int get_chunks_ready_count(void);
int get_chunk_continuous_secs(void);
uint32_t get_chunk_global_sample_index(void);
SemaphoreHandle_t get_chunk_stop_semaphore(void);
SemaphoreHandle_t get_chunk_file_mutex(void);
const char *get_chunk_dir(void);

// HTTP handlers
static esp_err_t index_handler(httpd_req_t *req) {
    extern const char index_html_start[] asm("_binary_index_html_start");
    extern const char index_html_end[] asm("_binary_index_html_end");
    size_t html_len = index_html_end - index_html_start;

    while (html_len > 0 && index_html_start[html_len - 1] == '\0') {
        html_len--;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Type", "text/html; charset=utf-8");
    httpd_resp_send(req, index_html_start, html_len);
    return ESP_OK;
}

static esp_err_t css_handler(httpd_req_t *req) {
    extern const char style_css_start[] asm("_binary_style_css_start");
    extern const char style_css_end[] asm("_binary_style_css_end");
    size_t css_len = style_css_end - style_css_start;

    while (css_len > 0 && style_css_start[css_len - 1] == '\0') {
        css_len--;
    }

    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Content-Type", "text/css; charset=utf-8");
    httpd_resp_send(req, style_css_start, css_len);
    return ESP_OK;
}

static esp_err_t js_handler(httpd_req_t *req) {
    extern const char script_js_start[] asm("_binary_script_js_start");
    extern const char script_js_end[] asm("_binary_script_js_end");
    size_t js_len = script_js_end - script_js_start;

    while (js_len > 0 && script_js_start[js_len - 1] == '\0') {
        js_len--;
    }

    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Type",
                       "application/javascript; charset=utf-8");
    httpd_resp_send(req, script_js_start, js_len);
    return ESP_OK;
}

static esp_err_t favicon_handler(httpd_req_t *req) {
    extern const char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const char favicon_ico_end[] asm("_binary_favicon_ico_end");
    size_t ico_len = favicon_ico_end - favicon_ico_start;

    if (ico_len == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "Favicon not found", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, favicon_ico_start, ico_len);
    return ESP_OK;
}

static esp_err_t adc_api_handler(httpd_req_t *req) {
    int adc_value = get_current_adc_value();
    int sample_count = get_sample_count();
    bool logging = is_csv_logging_active() || is_chunked_logging_active();
    uint64_t elapsed_ms = 0;
    float rate_hz = 0.0f;
    if (logging) {
        get_logging_stats(&sample_count, &elapsed_ms, &rate_hz);
    } else {
        rate_hz = (float)get_actual_sample_rate_hz();
    }

    char response[200];
    if (logging) {
        snprintf(response, sizeof(response),
                 "{\"adc\":%d,\"samples\":%d,\"logging\":%s,\"elapsed_ms\":%"
                 "llu,\"rate_hz\":%.2f}",
                 adc_value, sample_count, logging ? "true" : "false",
                 (unsigned long long)elapsed_ms, rate_hz);
    } else {
        snprintf(response, sizeof(response),
                 "{\"adc\":%d,\"samples\":%d,\"logging\":%s,\"rate_hz\":%.2f}",
                 adc_value, sample_count, logging ? "true" : "false", rate_hz);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Helper function to parse client time and timezone from JSON body
// Returns true if time was successfully set
static bool parse_client_time(httpd_req_t *req, char *client_time_str,
                              size_t client_time_len,
                              int *timezone_offset_min) {
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 512) {
        return false; // No body or body too large
    }

    if (timezone_offset_min) {
        *timezone_offset_min = 0; // Default to UTC
    }

    char *buf = malloc(content_len + 1);
    if (buf == NULL) {
        ESP_LOGW(SERVER_TAG, "Failed to allocate buffer for client time");
        return false;
    }

    // Read the full body (may need multiple reads for chunked data)
    int total_read = 0;
    while (total_read < content_len) {
        int ret =
            httpd_req_recv(req, buf + total_read, content_len - total_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue; // Retry on timeout
            }
            free(buf);
            return false;
        }
        total_read += ret;
    }
    buf[total_read] = '\0';

    // Simple JSON parsing: look for "client_time":"..." or
    // "client_timestamp":...
    char *time_ptr = strstr(buf, "\"client_time\"");
    if (time_ptr != NULL) {
        char *colon = strchr(time_ptr, ':');
        if (colon != NULL) {
            char *quote1 = strchr(colon + 1, '"');
            if (quote1 != NULL) {
                char *quote2 = strchr(quote1 + 1, '"');
                if (quote2 != NULL) {
                    size_t len = quote2 - quote1 - 1;
                    if (len < client_time_len) {
                        memcpy(client_time_str, quote1 + 1, len);
                        client_time_str[len] = '\0';
                    }
                }
            }
        }
    }

    // Parse timezone offset (in minutes, e.g., -300 for EST which is UTC-5)
    if (timezone_offset_min) {
        char *tz_ptr = strstr(buf, "\"timezone_offset\"");
        if (tz_ptr != NULL) {
            char *colon = strchr(tz_ptr, ':');
            if (colon != NULL) {
                *timezone_offset_min = (int)strtol(colon + 1, NULL, 10);
            }
        }
    }

    // Also try to parse timestamp for setting system time
    char *ts_ptr = strstr(buf, "\"client_timestamp\"");
    bool time_set = false;
    if (ts_ptr != NULL) {
        char *colon = strchr(ts_ptr, ':');
        if (colon != NULL) {
            long long timestamp_ms = strtoll(colon + 1, NULL, 10);
            if (timestamp_ms > 0) {
                int tz_offset_min =
                    timezone_offset_min ? *timezone_offset_min : 0;
                s_timezone_offset_min = tz_offset_min;
                set_tz_from_offset_min(tz_offset_min);

                // Set system clock to UTC (client_timestamp is UTC). SNTP also
                // sets UTC, so they stay in sync. localtime_r() will convert to
                // local using TZ set above.
                time_t timestamp_sec = timestamp_ms / 1000;
                struct timeval tv = {.tv_sec = timestamp_sec,
                                     .tv_usec = (timestamp_ms % 1000) * 1000};
                settimeofday(&tv, NULL);
                ESP_LOGI(SERVER_TAG,
                         "System time set from client: %lld ms (UTC), timezone "
                         "offset: %d min (%s)",
                         timestamp_ms, tz_offset_min, client_time_str);
                time_set = true;

                // Update time sync status and show on display
                if (s_time_sync_mutex != NULL) {
                    xSemaphoreTake(s_time_sync_mutex, portMAX_DELAY);
                }
                s_time_synced = true;
                if (s_time_sync_mutex != NULL) {
                    xSemaphoreGive(s_time_sync_mutex);
                }

                // Format time for display (extract date and time from ISO
                // string)
                char display_line2[22] = {0};
                char display_line3[22] = {0};

                // Get IP address for first line
                esp_netif_ip_info_t ip_info;
                esp_netif_t *netif = NULL;
                netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (netif == NULL ||
                    esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
                    netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
                    if (netif != NULL) {
                        esp_netif_get_ip_info(netif, &ip_info);
                    }
                }

                char ip_display[22] = {0};
                if (netif != NULL) {
                    snprintf(ip_display, sizeof(ip_display), "IP: " IPSTR,
                             IP2STR(&ip_info.ip));
                } else {
                    snprintf(ip_display, sizeof(ip_display), "IP: --");
                }

                if (client_time_str[0] != '\0') {
                    // Format: "2026-02-19T10:30:45.123Z" -> extract date and
                    // time Copy date part (first 10 chars: "2026-02-19")
                    int date_len = 10;
                    if (strlen(client_time_str) >= date_len) {
                        memcpy(display_line2, client_time_str, date_len);
                        display_line2[date_len] = '\0';
                        // Copy time part (skip 'T', get next 8 chars:
                        // "10:30:45")
                        if (strlen(client_time_str) >= date_len + 9) {
                            memcpy(display_line3,
                                   client_time_str + date_len + 1, 8);
                            display_line3[8] = '\0';
                        }
                    }
                }
                if (display_line2[0] == '\0') {
                    snprintf(display_line2, sizeof(display_line2), "Time set");
                }
                if (display_line3[0] == '\0') {
                    display_line3[0] = '\0';
                }
                display_show_3lines(ip_display, display_line2, display_line3);
            }
        }
    }

    free(buf);
    return time_set;
}

static esp_err_t start_logging_handler(httpd_req_t *req) {
    char client_time[64] = {0};
    int timezone_offset = 0;
    parse_client_time(req, client_time, sizeof(client_time), &timezone_offset);

    start_csv_logging();

    char response[256];
    if (client_time[0] != '\0') {
        snprintf(response, sizeof(response),
                 "{\"status\":\"started\",\"samples\":%d,\"rate_target\":%lu,"
                 "\"client_time_received\":\"%s\"}",
                 get_sample_count(), (unsigned long)get_actual_sample_rate_hz(),
                 client_time);
    } else {
        snprintf(response, sizeof(response),
                 "{\"status\":\"started\",\"samples\":%d,\"rate_target\":%lu}",
                 get_sample_count(),
                 (unsigned long)get_actual_sample_rate_hz());
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t stop_logging_handler(httpd_req_t *req) {
    // Stop logging first to set stop time
    stop_csv_logging();

    // Get statistics after stopping (to get accurate elapsed time)
    int sample_count = 0;
    uint64_t elapsed_time_ms = 0;
    float rate_hz = 0.0f;
    get_logging_stats(&sample_count, &elapsed_time_ms, &rate_hz);

    // Get final sample count
    int final_sample_count = get_sample_count();

    // Build JSON response with CSV download available
    char response[256];
    snprintf(response, sizeof(response),
             "{\"status\":\"stopped\",\"samples\":%d,\"elapsed_ms\":%llu,"
             "\"rate_hz\":%.2f,\"csv_available\":true}",
             final_sample_count, (unsigned long long)elapsed_time_ms, rate_hz);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t csv_download_handler(httpd_req_t *req) {
    const char *csv_path = get_csv_file_path();
    FILE *file = fopen(csv_path, "r");
    if (file == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "CSV file not found", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Extract filename from path for download header
    const char *filename = strrchr(csv_path, '/');
    if (filename == NULL) {
        filename = csv_path;
    } else {
        filename++; // Skip the '/'
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    // Header value buffers — must outlive all httpd_resp_send_chunk calls
    // (httpd_resp_set_hdr stores pointers, not copies)
    char content_disposition[128];
    char content_length[32];
    snprintf(content_disposition, sizeof(content_disposition),
             "attachment; filename=%s", filename);
    snprintf(content_length, sizeof(content_length), "%ld", file_size);

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", content_disposition);
    httpd_resp_set_hdr(req, "Content-Length", content_length);

    // Read and send file in chunks
    char buffer[1024];
    size_t bytes_read;
    long total_sent = 0;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        esp_err_t ret = httpd_resp_send_chunk(req, buffer, bytes_read);
        if (ret != ESP_OK) {
            fclose(file);
            return ret;
        }
        total_sent += bytes_read;
    }

    fclose(file);

    // Send final chunk (empty to signal end)
    httpd_resp_send_chunk(req, NULL, 0);

    ESP_LOGI(SERVER_TAG, "CSV file downloaded: %ld bytes", total_sent);
    return ESP_OK;
}

// SSE: stream CSV file to client in small buffers (avoids malloc of ~270KB on
// ESP32) Use heap buffer: httpd task has limited stack (~4KB); 4.4KB on stack
// caused Guru panic
#define SSE_READ_BUF 4096
#define SSE_LINE_MAX 320
#define SSE_CARRY_MAX 320
static esp_err_t send_sse_csv_from_file(httpd_req_t *req, FILE *f,
                                        SemaphoreHandle_t stop_sem) {
    char *buf = malloc(SSE_READ_BUF + SSE_CARRY_MAX);
    if (buf == NULL) {
        ESP_LOGE(SERVER_TAG, "SSE: malloc(%d) failed for stream buffer",
                 SSE_READ_BUF + SSE_CARRY_MAX);
        return ESP_ERR_NO_MEM;
    }
    size_t data_len = 0;
    char sse_line[SSE_LINE_MAX + 16];
    esp_err_t ret = ESP_OK;
    int read_count = 0;

    while (1) {
        // PRIORITY: Check stop semaphore frequently (every read) to allow
        // immediate stop
        if (stop_sem != NULL && xSemaphoreTake(stop_sem, 0) == pdTRUE) {
            ESP_LOGI(SERVER_TAG,
                     "SSE: stop signaled during file read, aborting");
            ret = ESP_ERR_INVALID_STATE; // Signal that we stopped early
            goto cleanup;
        }

        size_t nread = fread(buf + data_len, 1, SSE_READ_BUF, f);
        data_len += nread;
        if (data_len == 0)
            break;

        read_count++;
        // Yield every 5 reads (more frequent) to prevent blocking HTTP server
        if (read_count % 5 == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            // Check stop again after yield
            if (stop_sem != NULL && xSemaphoreTake(stop_sem, 0) == pdTRUE) {
                ESP_LOGI(SERVER_TAG, "SSE: stop signaled during file read "
                                     "(after yield), aborting");
                ret = ESP_ERR_INVALID_STATE;
                goto cleanup;
            }
        }

        const char *end = buf + data_len;
        const char *last_nl = NULL;
        const char *start = buf;

        while (start < end) {
            const char *nl = start;
            while (nl < end && *nl != '\n' && *nl != '\r')
                nl++;
            if (nl < end)
                last_nl = nl;
            size_t line_len = (size_t)(nl - start);
            if (line_len > 0) {
                // Handle lines that exceed SSE_LINE_MAX by truncating with
                // warning
                size_t send_len =
                    (line_len < SSE_LINE_MAX) ? line_len : SSE_LINE_MAX;
                if (line_len >= SSE_LINE_MAX) {
                    ESP_LOGW(SERVER_TAG,
                             "SSE: CSV line truncated (%zu >= %d bytes)",
                             line_len, SSE_LINE_MAX);
                }
                int n = snprintf(sse_line, sizeof(sse_line), "data: %.*s\n",
                                 (int)send_len, start);
                if (n > 0) {
                    esp_err_t r =
                        httpd_resp_send_chunk(req, sse_line, (size_t)n);
                    if (r != ESP_OK) {
                        ret = r;
                        goto cleanup;
                    }
                }
            }
            start = nl;
            if (start < end && (*start == '\n' || *start == '\r'))
                start++;
        }

        if (last_nl != NULL) {
            const char *tail = last_nl + 1;
            if (tail < end && *last_nl == '\n' && *tail == '\r')
                tail++;
            data_len = (size_t)(end - tail);
            if (data_len > 0 && data_len <= SSE_CARRY_MAX) {
                memmove(buf, tail, data_len);
            } else {
                data_len = 0;
            }
        } else {
            if (data_len <= SSE_CARRY_MAX) {
                /* entire buffer is partial line, keep for next read */
            } else {
                /* Line too long - truncate and keep what fits */
                ESP_LOGW(SERVER_TAG,
                         "SSE: partial line overflow (%zu > %d), truncating",
                         data_len, SSE_CARRY_MAX);
                memmove(buf, buf + data_len - SSE_CARRY_MAX, SSE_CARRY_MAX);
                data_len = SSE_CARRY_MAX;
            }
        }

        if (nread < SSE_READ_BUF)
            break;
    }

    if (data_len > 0 && data_len < SSE_LINE_MAX) {
        int n = snprintf(sse_line, sizeof(sse_line), "data: %.*s\n",
                         (int)data_len, buf);
        if (n > 0 && ret == ESP_OK)
            ret = httpd_resp_send_chunk(req, sse_line, (size_t)n);
    }
    if (ret == ESP_OK)
        ret = httpd_resp_send_chunk(req, "\n", 1);
cleanup:
    free(buf);
    return ret;
}

// Get list of available chunks (for polling API)
// When active: returns chunks ready so far. When inactive: returns last chunks
// count so client can fetch after stop.
static esp_err_t chunks_list_handler(httpd_req_t *req) {
    int chunks_ready = get_chunks_ready_count();
    bool active = is_chunked_logging_active();

    char response[128];
    snprintf(response, sizeof(response), "{\"chunks\":%d,\"active\":%s}",
             chunks_ready, active ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Chunk download read buffer size (heap; avoids httpd task stack overflow ->
// StoreProhibited in uxListRemove)
#define CHUNK_GET_BUF_SIZE 2048

// Get individual chunk file (polling API - no persistent connection)
// URI: /api/chunk?index=0 (query param; ESP-IDF httpd does not match
// /api/chunk/0)
static esp_err_t chunk_get_handler(httpd_req_t *req) {
    int chunk_index = -1;
    char query_str[64];
    char index_val[16];

    if (httpd_req_get_url_query_str(req, query_str, sizeof(query_str)) ==
        ESP_OK) {
        if (httpd_query_key_value(query_str, "index", index_val,
                                  sizeof(index_val)) == ESP_OK) {
            chunk_index = atoi(index_val);
        }
    }

    if (chunk_index < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"invalid chunk index\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Serialize SD access with csv_writer (avoids Guru Meditation from
    // concurrent FatFS/SPI use)
    SemaphoreHandle_t chunk_mutex = get_chunk_file_mutex();
    if (chunk_mutex != NULL) {
        if (xSemaphoreTake(chunk_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_send(req, "{\"error\":\"chunk file busy, try again\"}",
                            HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }

    uint64_t serve_start_us = esp_timer_get_time();

    // Build chunk file path using the active chunk directory from 002.c
    char chunk_path[CHUNK_PATH_MAX];
    const char *cdir = get_chunk_dir();
    if (cdir != NULL && cdir[0] != '\0') {
        snprintf(chunk_path, sizeof(chunk_path), "%s/%d.csv", cdir,
                 chunk_index);
    } else {
        snprintf(chunk_path, sizeof(chunk_path), "/sdcard/laser/chunks/%d.csv",
                 chunk_index);
    }

    FILE *f = fopen(chunk_path, "r");

    // Check if file exists
    if (f == NULL) {
        if (chunk_mutex != NULL)
            xSemaphoreGive(chunk_mutex);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "{\"error\":\"chunk not found\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Heap buffer for streaming (httpd task stack ~4KB; 2KB on stack caused
    // StoreProhibited in uxListRemove)
    char *buffer = malloc(CHUNK_GET_BUF_SIZE);
    if (buffer == NULL) {
        fclose(f);
        if (chunk_mutex != NULL)
            xSemaphoreGive(chunk_mutex);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"out of memory\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Header value buffers — must outlive all httpd_resp_send_chunk calls
    // (httpd_resp_set_hdr stores pointers, not copies)
    char content_disposition[128];
    char content_length[32];
    snprintf(content_disposition, sizeof(content_disposition),
             "attachment; filename=%d.csv", chunk_index);
    snprintf(content_length, sizeof(content_length), "%ld", file_size);

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", content_disposition);
    httpd_resp_set_hdr(req, "Content-Length", content_length);

    // Stream file in chunks (non-blocking)
    size_t bytes_read;
    long total_sent = 0;
    esp_err_t ret = ESP_OK;

    while ((bytes_read = fread(buffer, 1, CHUNK_GET_BUF_SIZE, f)) > 0) {
        ret = httpd_resp_send_chunk(req, buffer, bytes_read);
        if (ret != ESP_OK) {
            break;
        }
        total_sent += bytes_read;
        // Yield periodically to avoid blocking HTTP server
        if (total_sent % (CHUNK_GET_BUF_SIZE * 10) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    free(buffer);
    fclose(f);
    if (chunk_mutex != NULL)
        xSemaphoreGive(chunk_mutex);

    if (ret != ESP_OK) {
        return ret;
    }

    if ((long)total_sent != file_size) {
        ESP_LOGE(SERVER_TAG,
                 "Chunk %d: partial send (file_size=%ld, total_sent=%ld)",
                 chunk_index, file_size, total_sent);
    }
    uint64_t serve_duration_us = esp_timer_get_time() - serve_start_us;
    if (serve_duration_us > 1000000ULL) {
        ESP_LOGW(SERVER_TAG,
                 "Chunk %d download took %.1f ms (> 1000 ms window)",
                 chunk_index, (double)serve_duration_us / 1000.0);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(SERVER_TAG, "Chunk %d downloaded: %ld bytes", chunk_index,
             total_sent);
    return ESP_OK;
}

// OLD SSE handler - kept for compatibility but deprecated
static esp_err_t csv_stream_handler(httpd_req_t *req) {
    ESP_LOGI(
        SERVER_TAG,
        "SSE: client connected to /api/csv_stream (direct queue streaming)");

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    QueueHandle_t sample_queue = get_sample_queue();
    if (sample_queue == NULL) {
        ESP_LOGE(SERVER_TAG, "SSE: sample queue is NULL, rejecting client");
        httpd_resp_send_chunk(req, "data: error\n\n", 12);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    if (!is_chunked_logging_active()) {
        ESP_LOGE(SERVER_TAG,
                 "SSE: chunked logging not active, rejecting client");
        httpd_resp_send_chunk(req, "data: error\n\n", 12);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    // Read chunk files from queue (csv_writer_task writes files and signals via
    // queue)
    QueueHandle_t chunk_queue = get_chunk_ready_queue();
    if (chunk_queue == NULL) {
        ESP_LOGE(SERVER_TAG, "SSE: chunk queue is NULL, rejecting client");
        httpd_resp_send_chunk(req, "data: error\n\n", 12);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    int keepalive_count = 0;
    bool stream_complete = false;
    SemaphoreHandle_t stop_sem = get_chunk_stop_semaphore();

    ESP_LOGI(SERVER_TAG, "SSE: starting file-based streaming (files written by "
                         "csv_writer_task)");

    while (!stream_complete) {
        // PRIORITY: Check stop semaphore first (0 timeout = non-blocking)
        // If stop semaphore is signaled, ALWAYS stop immediately (don't wait
        // for cycle) The csv_writer_task will handle finishing the cycle if
        // needed
        if (stop_sem != NULL && xSemaphoreTake(stop_sem, 0) == pdTRUE) {
            ESP_LOGI(SERVER_TAG,
                     "SSE: stop semaphore signaled, stopping immediately");
            httpd_resp_send_chunk(req, "data: done\n\n", 12);
            stream_complete = true;
            break;
        }

        // Check if chunked logging stopped
        if (!is_chunked_logging_active()) {
            ESP_LOGI(SERVER_TAG, "SSE: chunked logging stopped, sending done");
            httpd_resp_send_chunk(req, "data: done\n\n", 12);
            stream_complete = true;
            break;
        }

        // Wait for chunk ready signal from csv_writer_task (shorter timeout for
        // faster stop response)
        chunk_ready_item_t item;
        if (xQueueReceive(chunk_queue, &item, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (item.index < 0) {
                // Done sentinel
                ESP_LOGI(SERVER_TAG, "SSE: received done sentinel");
                httpd_resp_send_chunk(req, "data: done\n\n", 12);
                stream_complete = true;
                break;
            }

            // Check stop before processing chunk (re-check semaphore)
            if (stop_sem != NULL && xSemaphoreTake(stop_sem, 0) == pdTRUE) {
                ESP_LOGI(
                    SERVER_TAG,
                    "SSE: stop signaled before processing chunk %d, stopping",
                    item.index);
                httpd_resp_send_chunk(req, "data: done\n\n", 12);
                stream_complete = true;
                break;
            }

            ESP_LOGI(SERVER_TAG, "SSE: streaming chunk %d from %s (%zu bytes)",
                     item.index, item.path, item.size_bytes);

            // Read and stream chunk file (non-blocking with frequent stop
            // checks)
            FILE *f = fopen(item.path, "r");
            if (f != NULL) {
                esp_err_t ret = send_sse_csv_from_file(req, f, stop_sem);
                fclose(f);
                if (ret == ESP_ERR_INVALID_STATE) {
                    // Stop was requested during file read
                    ESP_LOGI(
                        SERVER_TAG,
                        "SSE: chunk %d streaming aborted due to stop request",
                        item.index);
                    httpd_resp_send_chunk(req, "data: done\n\n", 12);
                    stream_complete = true;
                    break;
                } else if (ret != ESP_OK) {
                    ESP_LOGW(SERVER_TAG,
                             "SSE: failed to stream chunk %d, ret=%d",
                             item.index, ret);
                    return ret;
                }
                ESP_LOGI(SERVER_TAG, "SSE: chunk %d streamed successfully",
                         item.index);
            } else {
                ESP_LOGW(SERVER_TAG, "SSE: cannot open chunk file %s",
                         item.path);
            }

            // Check stop again after each chunk (before waiting for next)
            if (stop_sem != NULL && xSemaphoreTake(stop_sem, 0) == pdTRUE) {
                ESP_LOGI(
                    SERVER_TAG,
                    "SSE: stop signaled after chunk %d, stopping immediately",
                    item.index);
                httpd_resp_send_chunk(req, "data: done\n\n", 12);
                stream_complete = true;
                break;
            }
            keepalive_count = 0;
        } else {
            // Timeout - check stop FIRST (high priority)
            if (stop_sem != NULL && xSemaphoreTake(stop_sem, 0) == pdTRUE) {
                ESP_LOGI(SERVER_TAG,
                         "SSE: stop signaled (timeout), stopping immediately");
                httpd_resp_send_chunk(req, "data: done\n\n", 12);
                stream_complete = true;
                break;
            }

            // Check if logging stopped
            if (!is_chunked_logging_active()) {
                ESP_LOGI(
                    SERVER_TAG,
                    "SSE: chunked logging stopped (timeout), sending done");
                httpd_resp_send_chunk(req, "data: done\n\n", 12);
                stream_complete = true;
                break;
            }

            // Send keepalive every 5 seconds (50 * 50ms = 2.5s, but we check
            // stop every timeout)
            keepalive_count++;
            if (keepalive_count >= 100) {
                esp_err_t ret =
                    httpd_resp_send_chunk(req, ": keepalive\n\n", 13);
                if (ret != ESP_OK) {
                    ESP_LOGW(SERVER_TAG, "SSE: keepalive failed ret=%d", ret);
                    return ret;
                }
                keepalive_count = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(SERVER_TAG, "SSE: handler finished, connection closed");
    return ESP_OK;
}

static esp_err_t start_chunk_handler(httpd_req_t *req) {
    char client_time[64] = {0};
    long long client_timestamp_ms = 0;
    int timezone_offset = 0;
    bool testbench = false;

    // Parse JSON body for client_time, timezone, and testbench flag
    int content_len = req->content_len;
    if (content_len > 0 && content_len <= 512) {
        char *buf = malloc(content_len + 1);
        if (buf != NULL) {
            int total_read = 0;
            while (total_read < content_len) {
                int ret = httpd_req_recv(req, buf + total_read,
                                         content_len - total_read);
                if (ret <= 0) {
                    if (ret == HTTPD_SOCK_ERR_TIMEOUT)
                        continue;
                    break;
                }
                total_read += ret;
            }
            buf[total_read] = '\0';

            // Parse testbench flag
            if (strstr(buf, "\"testbench\":true") != NULL ||
                strstr(buf, "\"testbench\": true") != NULL) {
                testbench = true;
            }

            // Parse client_time
            char *time_ptr = strstr(buf, "\"client_time\"");
            if (time_ptr != NULL) {
                char *colon = strchr(time_ptr, ':');
                if (colon) {
                    char *q1 = strchr(colon + 1, '"');
                    if (q1) {
                        char *q2 = strchr(q1 + 1, '"');
                        if (q2) {
                            size_t len = q2 - q1 - 1;
                            if (len < sizeof(client_time)) {
                                memcpy(client_time, q1 + 1, len);
                                client_time[len] = '\0';
                            }
                        }
                    }
                }
            }

            // Parse client_timestamp (milliseconds since epoch, UTC)
            char *ts_ptr = strstr(buf, "\"client_timestamp\"");
            if (ts_ptr != NULL) {
                char *colon2 = strchr(ts_ptr, ':');
                if (colon2)
                    client_timestamp_ms = strtoll(colon2 + 1, NULL, 10);
            }

            // Parse timezone_offset
            char *tz_ptr = strstr(buf, "\"timezone_offset\"");
            if (tz_ptr != NULL) {
                char *colon = strchr(tz_ptr, ':');
                if (colon)
                    timezone_offset = atoi(colon + 1);
            }

            free(buf);
        }
    }

    // Apply client time if provided
    if (client_timestamp_ms > 0) {
        s_timezone_offset_min = timezone_offset;
        set_tz_from_offset_min(timezone_offset);

        time_t timestamp_sec = client_timestamp_ms / 1000;
        struct timeval tv = {.tv_sec = timestamp_sec,
                             .tv_usec = (client_timestamp_ms % 1000) * 1000};
        settimeofday(&tv, NULL);
        ESP_LOGI(SERVER_TAG,
                 "System time set from start_chunk: %lld ms (UTC), tz offset: "
                 "%d min (%s)",
                 client_timestamp_ms, timezone_offset, client_time);

        if (s_time_sync_mutex != NULL) {
            xSemaphoreTake(s_time_sync_mutex, portMAX_DELAY);
        }
        s_time_synced = true;
        if (s_time_sync_mutex != NULL) {
            xSemaphoreGive(s_time_sync_mutex);
        }
    }

    bool ok = start_chunked_logging(testbench);

    uint32_t rate = get_actual_sample_rate_hz();
    char response[256];
    if (ok) {
        snprintf(response, sizeof(response),
                 "{\"status\":\"chunk_started\",\"chunk_secs\":%d,\"rate_hz\":%"
                 "lu,\"testbench\":%s}",
                 get_chunk_continuous_secs(), (unsigned long)rate,
                 testbench ? "true" : "false");
    } else {
        httpd_resp_set_status(req, "409 Conflict");
        snprintf(response, sizeof(response),
                 "{\"status\":\"error\",\"error\":\"chunked logging already "
                 "active or failed to start\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t stop_chunk_handler(httpd_req_t *req) {
    bool was_active = is_chunked_logging_active();
    stop_chunked_logging();
    bool stopped = !is_chunked_logging_active();
    char response[128];
    snprintf(
        response, sizeof(response),
        "{\"status\":\"chunk_stopped\",\"was_active\":%s,\"confirmed\":%s}",
        was_active ? "true" : "false", stopped ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Sync time endpoint - called automatically when client connects
static esp_err_t sync_time_handler(httpd_req_t *req) {
    char client_time[64] = {0};
    int timezone_offset = 0;
    bool time_set = parse_client_time(req, client_time, sizeof(client_time),
                                      &timezone_offset);

    char response[256];
    if (time_set) {
        snprintf(response, sizeof(response),
                 "{\"status\":\"time_synced\",\"client_time\":\"%s\","
                 "\"timezone_offset\":%d}",
                 client_time, timezone_offset);
    } else {
        snprintf(
            response, sizeof(response),
            "{\"status\":\"time_sync_failed\",\"error\":\"invalid_request\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Task to periodically update display with IP address, date, and time
static void display_time_task(void *pvParameters) {
    char ip_line[22] = {0};
    char date_line[22] = {0};
    char time_line[22] = {0};
    static char last_ip[22] = {0};
    static char last_date[22] = {0};
    static char last_time[22] = {0};
    static bool last_synced = false;
    struct tm timeinfo;
    time_t now;
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = NULL;

    // Try to get IP address (try STA first, then AP)
    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (netif != NULL) {
            esp_netif_get_ip_info(netif, &ip_info);
        }
    }

    // Format IP address line
    if (netif != NULL) {
        snprintf(ip_line, sizeof(ip_line), "IP: " IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(ip_line, sizeof(ip_line), "IP: --");
    }

    while (1) {
        bool synced = false;
        if (s_time_sync_mutex != NULL) {
            xSemaphoreTake(s_time_sync_mutex, portMAX_DELAY);
            synced = s_time_synced;
            xSemaphoreGive(s_time_sync_mutex);
        }

        if (synced) {
            // Get current time
            time(&now);
            localtime_r(&now, &timeinfo);

            // Format date: "2026-02-19" (max 11 chars: YYYY-MM-DD + null)
            int year = (int)(timeinfo.tm_year + 1900);
            int month = (int)(timeinfo.tm_mon + 1);
            int day = (int)timeinfo.tm_mday;
            // Use larger buffer to avoid format-truncation warning, then copy
            // to date_line
            char date_buf[32];
            snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d", year, month,
                     day);
            strncpy(date_line, date_buf, sizeof(date_line) - 1);
            date_line[sizeof(date_line) - 1] = '\0';

            // Format time: "10:30:45"
            snprintf(time_line, sizeof(time_line), "%02d:%02d:%02d",
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

            /* Only redraw full screen when IP, date, or sync state changed;
             * else just update time line to reduce blink */
            if (!last_synced || strcmp(ip_line, last_ip) != 0 ||
                strcmp(date_line, last_date) != 0) {
                display_show_3lines(ip_line, date_line, time_line);
                strncpy(last_ip, ip_line, sizeof(last_ip) - 1);
                last_ip[sizeof(last_ip) - 1] = '\0';
                strncpy(last_date, date_line, sizeof(last_date) - 1);
                last_date[sizeof(last_date) - 1] = '\0';
                strncpy(last_time, time_line, sizeof(last_time) - 1);
                last_time[sizeof(last_time) - 1] = '\0';
            } else if (strcmp(time_line, last_time) != 0) {
                display_update_3rd_line(time_line);
                strncpy(last_time, time_line, sizeof(last_time) - 1);
                last_time[sizeof(last_time) - 1] = '\0';
            }
            last_synced = true;
        } else {
            /* Not synced: full update only when state or IP changed */
            if (last_synced || strcmp(ip_line, last_ip) != 0) {
                display_show_3lines(ip_line, "Waiting for", "time sync...");
                strncpy(last_ip, ip_line, sizeof(last_ip) - 1);
                last_ip[sizeof(last_ip) - 1] = '\0';
            }
            last_synced = false;
        }

        // Update every second
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void register_routes(httpd_handle_t server) {
    httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET, .handler = index_handler};
    httpd_uri_t css_uri = {
        .uri = "/style.css", .method = HTTP_GET, .handler = css_handler};
    httpd_uri_t js_uri = {
        .uri = "/script.js", .method = HTTP_GET, .handler = js_handler};
    httpd_uri_t favicon_uri = {
        .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler};
    httpd_uri_t adc_uri = {
        .uri = "/api/adc", .method = HTTP_GET, .handler = adc_api_handler};
    httpd_uri_t start_uri = {.uri = "/api/start",
                             .method = HTTP_POST,
                             .handler = start_logging_handler};
    httpd_uri_t stop_uri = {.uri = "/api/stop",
                            .method = HTTP_POST,
                            .handler = stop_logging_handler};
    httpd_uri_t csv_uri = {
        .uri = "/api/csv", .method = HTTP_GET, .handler = csv_download_handler};
    httpd_uri_t csv_stream_uri = {.uri = "/api/csv_stream",
                                  .method = HTTP_GET,
                                  .handler = csv_stream_handler};
    httpd_uri_t chunks_list_uri = {.uri = "/api/chunks",
                                   .method = HTTP_GET,
                                   .handler = chunks_list_handler};
    httpd_uri_t chunk_get_uri = {
        .uri = "/api/chunk", .method = HTTP_GET, .handler = chunk_get_handler};
    httpd_uri_t start_chunk_uri = {.uri = "/api/start_chunk",
                                   .method = HTTP_POST,
                                   .handler = start_chunk_handler};
    httpd_uri_t stop_chunk_uri = {.uri = "/api/stop_chunk",
                                  .method = HTTP_POST,
                                  .handler = stop_chunk_handler};
    httpd_uri_t sync_time_uri = {.uri = "/api/sync_time",
                                 .method = HTTP_POST,
                                 .handler = sync_time_handler};

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &css_uri);
    httpd_register_uri_handler(server, &js_uri);
    httpd_register_uri_handler(server, &favicon_uri);
    httpd_register_uri_handler(server, &adc_uri);
    httpd_register_uri_handler(server, &start_uri);
    httpd_register_uri_handler(server, &stop_uri);
    httpd_register_uri_handler(server, &csv_uri);
    httpd_register_uri_handler(server, &csv_stream_uri);
    httpd_register_uri_handler(server, &chunks_list_uri);
    httpd_register_uri_handler(server, &chunk_get_uri);
    httpd_register_uri_handler(server, &start_chunk_uri);
    httpd_register_uri_handler(server, &stop_chunk_uri);
    httpd_register_uri_handler(server, &sync_time_uri);
}

httpd_handle_t start_webserver_http(void) {
    // Initialize time sync mutex if not already initialized
    if (s_time_sync_mutex == NULL) {
        s_time_sync_mutex = xSemaphoreCreateMutex();
        if (s_time_sync_mutex == NULL) {
            ESP_LOGE(SERVER_TAG, "Failed to create time sync mutex");
        }
    }

    // Start display time update task (only once)
    if (!s_display_time_task_started) {
        BaseType_t ret =
            xTaskCreate(display_time_task, "display_time", 4096, NULL, 5, NULL);
        if (ret == pdPASS) {
            s_display_time_task_started = true;
            ESP_LOGI(SERVER_TAG, "Display time update task started");
        } else {
            ESP_LOGE(SERVER_TAG, "Failed to create display time task");
        }
    }

    /* Auto-fetch timezone from IP geolocation (ip-api.com) - runs once, no
     * client needed */
    if (!s_tz_fetch_started) {
        s_tz_fetch_started = true;
        xTaskCreate(fetch_timezone_task, "tz_fetch", 4096, NULL, 4, NULL);
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Increase max URI handlers if needed
    config.max_uri_handlers = 16;
    config.max_resp_headers = 8;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        register_routes(server);
    } else {
        ESP_LOGE(SERVER_TAG, "Failed to start HTTP server");
    }
    return server;
}

httpd_handle_t start_webserver_https(void) {
    // Initialize time sync mutex if not already initialized
    if (s_time_sync_mutex == NULL) {
        s_time_sync_mutex = xSemaphoreCreateMutex();
        if (s_time_sync_mutex == NULL) {
            ESP_LOGE(SERVER_TAG, "Failed to create time sync mutex");
        }
    }

    // Start display time update task (only once)
    if (!s_display_time_task_started) {
        BaseType_t ret =
            xTaskCreate(display_time_task, "display_time", 4096, NULL, 5, NULL);
        if (ret == pdPASS) {
            s_display_time_task_started = true;
            ESP_LOGI(SERVER_TAG, "Display time update task started");
        } else {
            ESP_LOGE(SERVER_TAG, "Failed to create display time task");
        }
    }

    if (!s_tz_fetch_started) {
        s_tz_fetch_started = true;
        xTaskCreate(fetch_timezone_task, "tz_fetch", 4096, NULL, 4, NULL);
    }

    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.servercert = server_cert_pem_start;
    config.servercert_len = server_cert_pem_end - server_cert_pem_start;
    config.prvtkey_pem = server_key_pem_start;
    config.prvtkey_len = server_key_pem_end - server_key_pem_start;

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_ssl_start(&server, &config);
    if (ret == ESP_OK) {
        register_routes(server);
        ESP_LOGI(SERVER_TAG, "HTTPS server started");
    } else {
        ESP_LOGE(SERVER_TAG, "Failed to start HTTPS server: %s",
                 esp_err_to_name(ret));
        server = NULL;
    }
    return server;
}
