#include "server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
    
    char response[128];
    snprintf(response, sizeof(response),
             "{\"adc\":%d,\"samples\":%d,\"logging\":%s}",
             adc_value, sample_count, logging ? "true" : "false");
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t csv_download_handler(httpd_req_t *req) {
    FILE *csv = get_csv_file();
    const char *filename = get_csv_filename();
    
    if (csv == NULL || filename == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "CSV file not available", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // Reopen file for reading
    FILE *f = fopen(filename, "r");
    if (f == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Failed to open CSV file", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
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
    
    // Read and send file in chunks
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, n);
    }
    
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); // End response
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

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &css_uri);
    httpd_register_uri_handler(server, &js_uri);
    httpd_register_uri_handler(server, &favicon_uri);
    httpd_register_uri_handler(server, &adc_uri);
    httpd_register_uri_handler(server, &csv_uri);
}

httpd_handle_t start_webserver_http(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
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
