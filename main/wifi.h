#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

// WiFi Access Point configuration
#define WIFI_AP_SSID "LASER-LAB"
#define WIFI_AP_PASSWORD "12345678"
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_STA_CONN 4

// WiFi Station configuration
#define WIFI_STA_SSID "Milu"
#define WIFI_STA_PASSWORD "milu15042003"

void wifi_init_softap(void);

void wifi_init_sta(void);

#endif // WIFI_H
