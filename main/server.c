#include "server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <stdio.h>
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
int get_current_adc_value(void);
bool is_csv_logging_active(void);
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
    bool logging = is_csv_logging_active();
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
    char buffer[512];
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

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &css_uri);
    httpd_register_uri_handler(server, &js_uri);
    httpd_register_uri_handler(server, &favicon_uri);
    httpd_register_uri_handler(server, &adc_uri);
    httpd_register_uri_handler(server, &start_uri);
    httpd_register_uri_handler(server, &stop_uri);
    httpd_register_uri_handler(server, &csv_uri);
}

httpd_handle_t start_webserver_http(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Increase max URI handlers if needed
    config.max_uri_handlers = 10;
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
