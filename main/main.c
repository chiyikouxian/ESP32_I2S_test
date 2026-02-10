/**
 * @file main.c
 * @brief ESP32-S3 Speech-to-Text Demo using INMP441 + iFlytek IAT
 *
 * Flow:
 *   1. Connect to WiFi
 *   2. Synchronize time via SNTP (needed for iFlytek auth)
 *   3. Initialize INMP441 microphone
 *   4. Loop: record audio and send to iFlytek for recognition
 *
 * Hardware Connection (ESP32-S3):
 *   INMP441    ESP32-S3
 *   -------    --------
 *   VDD    ->  3.3V
 *   GND    ->  GND
 *   SD     ->  GPIO6 (Data)
 *   WS     ->  GPIO5 (Word Select / LRCLK)
 *   SCK    ->  GPIO4 (Bit Clock / BCLK)
 *   L/R    ->  GND (Left channel)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "inmp441.h"
#include "wifi.h"
#include "sntp_sync.h"
#include "xfyun_iat.h"

static const char *TAG = "MAIN";

/**
 * @brief Speech recognition task
 *
 * Repeatedly performs 5-second recording sessions,
 * sending audio to iFlytek for recognition.
 */
static void speech_task(void *arg)
{
    int session = 0;

    while (1) {
        session++;
        ESP_LOGI(TAG, "--- Session %d: Speak now (5 seconds) ---", session);

        esp_err_t ret = xfyun_iat_recognize(5);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Recognition failed: %s", esp_err_to_name(ret));
        }

        /* Pause between sessions */
        ESP_LOGI(TAG, "Next session in 3 seconds...\n");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "ESP32-S3 Speech-to-Text Demo");
    ESP_LOGI(TAG, "=================================");

    /* Step 1: Connect to WiFi */
    esp_err_t ret = wifi_init_sta();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection failed, cannot proceed");
        return;
    }

    /* Step 2: Synchronize time via SNTP */
    ret = sntp_sync_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Time sync failed, cannot proceed");
        return;
    }

    /* Step 3: Initialize INMP441 microphone */
    inmp441_config_t config = INMP441_DEFAULT_CONFIG();

    ret = inmp441_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize INMP441: %s", esp_err_to_name(ret));
        return;
    }

    /* Step 4: Start speech recognition task */
    ESP_LOGI(TAG, "All initialized. Starting speech recognition...");
    xTaskCreate(speech_task, "speech_task", 8192, NULL, 5, NULL);
}
