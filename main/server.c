#include "server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

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

// ADC/CSV functions from 002.c
int get_current_adc_value(void);
const char *get_csv_filename(void);
FILE *get_csv_file(void);
bool is_csv_logging_active(void);
int get_sample_count(void);
void get_spiffs_storage_info(size_t *total_bytes, size_t *used_bytes);
void start_csv_logging(void);
void stop_csv_logging(void);
void clear_spiffs_storage(void);
void stop_sampling_timer(void);
void start_sampling_timer(void);
SemaphoreHandle_t get_file_ops_mutex(void);
QueueHandle_t get_sample_queue(void);

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
    
    size_t total_bytes = 0, used_bytes = 0;
    get_spiffs_storage_info(&total_bytes, &used_bytes);
    size_t free_bytes = (total_bytes > used_bytes) ? (total_bytes - used_bytes) : 0;
    
    char response[256];
    snprintf(response, sizeof(response),
             "{\"adc\":%d,\"samples\":%d,\"logging\":%s,\"storage\":{\"total\":%zu,\"used\":%zu,\"free\":%zu}}",
             adc_value, sample_count, logging ? "true" : "false",
             total_bytes, used_bytes, free_bytes);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t csv_download_handler(httpd_req_t *req) {
    const char *filename = get_csv_filename();
    
    // Check if filename is valid (file may be closed after max samples reached)
    if (filename == NULL || strlen(filename) == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"No CSV file available. Please start recording first.\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // Timer should already be stopped from stop_csv_logging(), but ensure it's stopped
    // and wait for any queued samples to be processed
    ESP_LOGI(SERVER_TAG, "Ensuring sampling timer is stopped for CSV download");
    stop_sampling_timer();
    
    // Wait for ADC task to finish processing any remaining queued samples
    QueueHandle_t sample_queue = get_sample_queue();
    if (sample_queue != NULL) {
        int wait_count = 0;
        while (uxQueueMessagesWaiting(sample_queue) > 0 && wait_count < 50) {
            vTaskDelay(pdMS_TO_TICKS(10));  // Check every 10ms
            wait_count++;
        }
        if (wait_count > 0) {
            ESP_LOGI(SERVER_TAG, "Waited %d ms for queue to drain", wait_count * 10);
        }
    }
    
    // Additional delay to ensure all file operations complete
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Get file operations mutex to prevent concurrent access with ADC task
    SemaphoreHandle_t file_mutex = get_file_ops_mutex();
    if (file_mutex == NULL) {
        ESP_LOGE(SERVER_TAG, "File operations mutex not available");
        start_sampling_timer();
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Internal error: mutex not available\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // Take mutex with timeout to ensure ADC task finishes any file operations
    if (xSemaphoreTake(file_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(SERVER_TAG, "Failed to acquire file operations mutex (timeout)");
        start_sampling_timer();
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"File busy, please try again\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // Reopen file for reading (works even if file was closed)
    FILE *f = fopen(filename, "r");
    if (f == NULL) {
        // Release mutex and restart timer even if file open fails
        xSemaphoreGive(file_mutex);
        start_sampling_timer();
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"CSV file not found. Please start recording first.\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // Check if file has data (more than just the header line)
    // Read first line (header) and check if there's a second line
    char line[256];
    if (fgets(line, sizeof(line), f) == NULL) {
        // File is empty or can't read header
        fclose(f);
        xSemaphoreGive(file_mutex);
        start_sampling_timer();
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"CSV file is empty. No data recorded yet.\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // Check if there's at least one data line (second line)
    if (fgets(line, sizeof(line), f) == NULL) {
        // Only header, no data
        fclose(f);
        xSemaphoreGive(file_mutex);
        start_sampling_timer();
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"CSV file contains no data. Please record some samples first.\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // File has data, rewind to beginning and send it
    rewind(f);
    
    // Get filename without path for download
    const char *basename = strrchr(filename, '/');
    if (basename == NULL) {
        basename = filename;
    } else {
        basename++; // skip '/'
    }
    
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", 
                       "attachment; filename=\"adc_data.csv\"");
    
    // Read and send file in larger chunks with error handling
    // Larger buffer reduces number of send calls for big files
    char buf[4096];  // Increased from 512 to 4096 bytes
    size_t n;
    esp_err_t ret = ESP_OK;
    
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        ret = httpd_resp_send_chunk(req, buf, n);
        if (ret != ESP_OK) {
            ESP_LOGE(SERVER_TAG, "Failed to send CSV chunk: %s", esp_err_to_name(ret));
            break;
        }
    }
    
    fclose(f);
    
    // Release mutex before restarting timer
    xSemaphoreGive(file_mutex);
    
    // Restart the sampling timer after file I/O completes
    ESP_LOGI(SERVER_TAG, "Resuming sampling timer after CSV download");
    start_sampling_timer();
    
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0); // End response
    }
    return ret;
}

static esp_err_t start_logging_handler(httpd_req_t *req) {
    start_csv_logging();
    
    char response[128];
    snprintf(response, sizeof(response), "{\"status\":\"started\",\"samples\":%d}",
             get_sample_count());
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t stop_logging_handler(httpd_req_t *req) {
    stop_csv_logging();
    
    // Small delay to ensure file is fully closed and all buffers flushed
    // This helps prevent connection issues when client immediately requests CSV download
    vTaskDelay(pdMS_TO_TICKS(100));
    
    char response[128];
    snprintf(response, sizeof(response), "{\"status\":\"stopped\",\"samples\":%d}",
             get_sample_count());
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t clear_storage_handler(httpd_req_t *req) {
    clear_spiffs_storage();
    
    char response[128];
    snprintf(response, sizeof(response), "{\"status\":\"cleared\"}");
    
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
    httpd_uri_t csv_uri = {
        .uri = "/api/csv", .method = HTTP_GET, .handler = csv_download_handler};
    httpd_uri_t start_uri = {
        .uri = "/api/start", .method = HTTP_POST, .handler = start_logging_handler};
    httpd_uri_t stop_uri = {
        .uri = "/api/stop", .method = HTTP_POST, .handler = stop_logging_handler};
    // Clear storage handler disabled - feature removed from UI
    // httpd_uri_t clear_uri = {
    //     .uri = "/api/clear", .method = HTTP_POST, .handler = clear_storage_handler};

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &css_uri);
    httpd_register_uri_handler(server, &js_uri);
    httpd_register_uri_handler(server, &favicon_uri);
    httpd_register_uri_handler(server, &adc_uri);
    httpd_register_uri_handler(server, &csv_uri);
    httpd_register_uri_handler(server, &start_uri);
    httpd_register_uri_handler(server, &stop_uri);
    // httpd_register_uri_handler(server, &clear_uri);  // Disabled
}

httpd_handle_t start_webserver_http(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Increase max URI handlers if needed
    config.max_uri_handlers = 10;
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
