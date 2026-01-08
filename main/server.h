#ifndef SERVER_H
#define SERVER_H

#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

// Start plain HTTP server (no TLS)
httpd_handle_t start_webserver_http(void);

// Start HTTPS server (TLS). Returns NULL if TLS init fails.
httpd_handle_t start_webserver_https(void);

#endif // SERVER_H
