/**
 * @file wifi.h
 * @brief WiFi Station Mode Connection Module for ESP32-S3
 */

#ifndef __WIFI_H__
#define __WIFI_H__

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi Configuration */
#define WIFI_SSID           "cdut-yb"
#define WIFI_PASSWORD       "cdutyb218"
#define WIFI_MAX_RETRY      10

/**
 * @brief Initialize WiFi in station mode and connect to AP
 *
 * This function blocks until WiFi is connected or max retries exceeded.
 *
 * @return esp_err_t ESP_OK on successful connection
 */
esp_err_t wifi_init_sta(void);

#ifdef __cplusplus
}
#endif

#endif /* __WIFI_H__ */
