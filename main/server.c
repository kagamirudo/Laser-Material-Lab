#include "server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SERVER_TAG "SERVER"

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
void get_logging_stats(int *sample_count, uint64_t *elapsed_time_ms, float *rate_hz);
void get_spiffs_storage_info(size_t *total_bytes, size_t *used_bytes);
void start_csv_logging(void);
void stop_csv_logging(void);
void clear_spiffs_storage(void);
void stop_sampling_timer(void);
void start_sampling_timer(void);
QueueHandle_t get_sample_queue(void);
const char* get_csv_file_path(void);

// Chunked logging (must match 002.c)
#define CHUNK_PATH_MAX 80
typedef struct {
    int index;
    char path[CHUNK_PATH_MAX];
    size_t size_bytes;
} chunk_ready_item_t;
QueueHandle_t get_chunk_ready_queue(void);
void start_chunked_logging(void);
void stop_chunked_logging(void);
bool is_chunked_logging_active(void);

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
    }
    
    char response[160];
    if (logging) {
        snprintf(response, sizeof(response),
                 "{\"adc\":%d,\"samples\":%d,\"logging\":%s,\"elapsed_ms\":%llu}",
                 adc_value, sample_count, logging ? "true" : "false",
                 (unsigned long long)elapsed_ms);
    } else {
        snprintf(response, sizeof(response),
                 "{\"adc\":%d,\"samples\":%d,\"logging\":%s}",
                 adc_value, sample_count, logging ? "true" : "false");
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t start_logging_handler(httpd_req_t *req) {
    start_csv_logging();
    
    char response[128];
    snprintf(response, sizeof(response), "{\"status\":\"started\",\"samples\":%d,\"rate_target\":%d}",
             get_sample_count(), 4000);  // Target rate is 4000 Hz
    
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
             "{\"status\":\"stopped\",\"samples\":%d,\"elapsed_ms\":%llu,\"rate_hz\":%.2f,\"csv_available\":true}",
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
        filename++;  // Skip the '/'
    }
    
    // Set headers for file download
    httpd_resp_set_type(req, "text/csv");
    char content_disposition[128];
    snprintf(content_disposition, sizeof(content_disposition), "attachment; filename=%s", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", content_disposition);
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
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

// SSE: stream CSV file to client in small buffers (avoids malloc of ~270KB on ESP32)
// Use heap buffer: httpd task has limited stack (~4KB); 4.4KB on stack caused Guru panic
#define SSE_READ_BUF  4096
#define SSE_LINE_MAX  320
#define SSE_CARRY_MAX 320
static esp_err_t send_sse_csv_from_file(httpd_req_t *req, FILE *f) {
    char *buf = malloc(SSE_READ_BUF + SSE_CARRY_MAX);
    if (buf == NULL) {
        ESP_LOGE(SERVER_TAG, "SSE: malloc(%d) failed for stream buffer", SSE_READ_BUF + SSE_CARRY_MAX);
        return ESP_ERR_NO_MEM;
    }
    size_t data_len = 0;
    char sse_line[SSE_LINE_MAX + 16];
    esp_err_t ret = ESP_OK;

    while (1) {
        size_t nread = fread(buf + data_len, 1, SSE_READ_BUF, f);
        data_len += nread;
        if (data_len == 0) break;

        const char *end = buf + data_len;
        const char *last_nl = NULL;
        const char *start = buf;

        while (start < end) {
            const char *nl = start;
            while (nl < end && *nl != '\n' && *nl != '\r') nl++;
            if (nl < end) last_nl = nl;
            size_t line_len = (size_t)(nl - start);
            if (line_len > 0 && line_len < SSE_LINE_MAX) {
                int n = snprintf(sse_line, sizeof(sse_line), "data: %.*s\n", (int)line_len, start);
                if (n > 0) {
                    esp_err_t r = httpd_resp_send_chunk(req, sse_line, (size_t)n);
                    if (r != ESP_OK) { ret = r; goto cleanup; }
                }
            }
            start = nl;
            if (start < end && (*start == '\n' || *start == '\r')) start++;
        }

        if (last_nl != NULL) {
            const char *tail = last_nl + 1;
            if (tail < end && *last_nl == '\n' && *tail == '\r') tail++;
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
                data_len = 0; /* overflow, drop */
            }
        }

        if (nread < SSE_READ_BUF) break;
    }

    if (data_len > 0 && data_len < SSE_LINE_MAX) {
        int n = snprintf(sse_line, sizeof(sse_line), "data: %.*s\n", (int)data_len, buf);
        if (n > 0 && ret == ESP_OK) ret = httpd_resp_send_chunk(req, sse_line, (size_t)n);
    }
    if (ret == ESP_OK) ret = httpd_resp_send_chunk(req, "\n", 1);
cleanup:
    free(buf);
    return ret;
}

static esp_err_t csv_stream_handler(httpd_req_t *req) {
    ESP_LOGI(SERVER_TAG, "SSE: client connected to /api/csv_stream");

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    QueueHandle_t q = get_chunk_ready_queue();
    if (q == NULL) {
        ESP_LOGE(SERVER_TAG, "SSE: chunk queue is NULL, rejecting client");
        httpd_resp_send_chunk(req, "data: error\n\n", 12);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    int keepalive_count = 0;
    while (1) {
        chunk_ready_item_t item;
        if (xQueueReceive(q, &item, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (item.index < 0) {
                ESP_LOGI(SERVER_TAG, "SSE: received done sentinel, sending data:done to client");
                esp_err_t ret = httpd_resp_send_chunk(req, "data: done\n\n", 12);
                if (ret != ESP_OK) {
                    ESP_LOGW(SERVER_TAG, "SSE: send done failed ret=%d (client may have disconnected)", ret);
                    return ret;
                }
                ESP_LOGI(SERVER_TAG, "SSE: stream complete, closing connection");
                break;
            }

            ESP_LOGI(SERVER_TAG, "SSE: received chunk %d from queue (%zu bytes), opening %s", item.index, item.size_bytes, item.path);

            FILE *f = fopen(item.path, "r");
            if (f == NULL) {
                ESP_LOGW(SERVER_TAG, "SSE: cannot open chunk %s", item.path);
                continue;
            }
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (fsize <= 0 || fsize > 1024 * 1024) {
                ESP_LOGW(SERVER_TAG, "SSE: chunk %d invalid size %ld, skipping", item.index, fsize);
                fclose(f);
                continue;
            }

            ESP_LOGI(SERVER_TAG, "SSE: streaming chunk %d (%ld bytes) to client...", item.index, fsize);
            esp_err_t ret = send_sse_csv_from_file(req, f);
            fclose(f);
            if (ret != ESP_OK) {
                ESP_LOGW(SERVER_TAG, "SSE: send_sse_csv_event failed ret=%d for chunk %d (client disconnected?)", ret, item.index);
                return ret;
            }
            ESP_LOGI(SERVER_TAG, "SSE: chunk %d sent successfully (%zu bytes)", item.index, item.size_bytes);
        } else {
            keepalive_count++;
            esp_err_t ret = httpd_resp_send_chunk(req, ": keepalive\n\n", 13);
            if (ret != ESP_OK) {
                ESP_LOGW(SERVER_TAG, "SSE: keepalive send failed ret=%d (client disconnected?)", ret);
                return ret;
            }
            if (keepalive_count % 6 == 1) {
                ESP_LOGI(SERVER_TAG, "SSE: keepalive #%d (waiting for chunks, ~%d s idle)", keepalive_count, keepalive_count * 5);
            }
        }
    }

    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(SERVER_TAG, "SSE: handler finished, connection closed");
    return ESP_OK;
}

static esp_err_t start_chunk_handler(httpd_req_t *req) {
    start_chunked_logging();
    char response[128];
    snprintf(response, sizeof(response), "{\"status\":\"chunk_started\",\"threshold\":%d,\"peak\":%d}",
             1200, 10);  // THRESHOLD and PEAK
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t stop_chunk_handler(httpd_req_t *req) {
    stop_chunked_logging();
    char response[128];
    snprintf(response, sizeof(response), "{\"status\":\"chunk_stopped\"}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
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
    httpd_uri_t start_uri = {
        .uri = "/api/start", .method = HTTP_POST, .handler = start_logging_handler};
    httpd_uri_t stop_uri = {
        .uri = "/api/stop", .method = HTTP_POST, .handler = stop_logging_handler};
    httpd_uri_t csv_uri = {
        .uri = "/api/csv", .method = HTTP_GET, .handler = csv_download_handler};
    httpd_uri_t csv_stream_uri = {
        .uri = "/api/csv_stream", .method = HTTP_GET, .handler = csv_stream_handler};
    httpd_uri_t start_chunk_uri = {
        .uri = "/api/start_chunk", .method = HTTP_POST, .handler = start_chunk_handler};
    httpd_uri_t stop_chunk_uri = {
        .uri = "/api/stop_chunk", .method = HTTP_POST, .handler = stop_chunk_handler};

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &css_uri);
    httpd_register_uri_handler(server, &js_uri);
    httpd_register_uri_handler(server, &favicon_uri);
    httpd_register_uri_handler(server, &adc_uri);
    httpd_register_uri_handler(server, &start_uri);
    httpd_register_uri_handler(server, &stop_uri);
    httpd_register_uri_handler(server, &csv_uri);
    httpd_register_uri_handler(server, &csv_stream_uri);
    httpd_register_uri_handler(server, &start_chunk_uri);
    httpd_register_uri_handler(server, &stop_chunk_uri);
}

httpd_handle_t start_webserver_http(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Increase max URI handlers if needed
    config.max_uri_handlers = 14;
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
