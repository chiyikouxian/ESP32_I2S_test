/**
 * @file main.c
 * @brief INMP441 Microphone Audio Data Real-time Display Demo
 *
 * This demo reads audio data from INMP441 I2S microphone and displays
 * real-time audio level on the serial monitor.
 *
 * Hardware Connection (ESP32-S3):
 *   INMP441    ESP32-S3
 *   -------    --------
 *   VDD    ->  3.3V
 *   GND    ->  GND
 *   SD     ->  GPIO6 (Data)
 *   WS     ->  GPIO5 (Word Select / LRCLK)
 *   SCK    ->  GPIO4 (Bit Clock / BCLK)
 *   L/R    ->  GND (Left channel) or 3.3V (Right channel)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "inmp441.h"

static const char *TAG = "MAIN";

/* Audio buffer size (samples) */
#define AUDIO_BUFFER_SAMPLES    512
#define AUDIO_BUFFER_SIZE       (AUDIO_BUFFER_SAMPLES * sizeof(int32_t))

/* Display bar configuration */
#define BAR_WIDTH               50

/**
 * @brief Convert 32-bit sample to normalized value
 *        INMP441 outputs 24-bit data in 32-bit frame, MSB aligned
 */
static inline int32_t sample_to_int24(int32_t sample)
{
    return sample >> 8;  // Shift right 8 bits to get 24-bit value
}

/**
 * @brief Calculate RMS (Root Mean Square) of audio samples
 */
static float calculate_rms(int32_t *samples, size_t count)
{
    if (count == 0) return 0;

    double sum_squares = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t value = sample_to_int24(samples[i]);
        sum_squares += (double)value * value;
    }

    return sqrtf(sum_squares / count);
}

/**
 * @brief Calculate peak value of audio samples
 */
static int32_t calculate_peak(int32_t *samples, size_t count)
{
    int32_t peak = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t value = sample_to_int24(samples[i]);
        if (value < 0) value = -value;  // Absolute value
        if (value > peak) peak = value;
    }
    return peak;
}

/**
 * @brief Print audio level bar
 */
static void print_level_bar(float level, float max_level)
{
    int bar_length = (int)((level / max_level) * BAR_WIDTH);
    if (bar_length > BAR_WIDTH) bar_length = BAR_WIDTH;

    char bar[BAR_WIDTH + 1];
    memset(bar, '#', bar_length);
    memset(bar + bar_length, ' ', BAR_WIDTH - bar_length);
    bar[BAR_WIDTH] = '\0';

    printf("\r[%s] %.0f    ", bar, level);
    fflush(stdout);
}

/**
 * @brief Audio capture and display task
 */
static void audio_task(void *arg)
{
    int32_t *audio_buffer = (int32_t *)malloc(AUDIO_BUFFER_SIZE);
    if (audio_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        vTaskDelete(NULL);
        return;
    }

    size_t bytes_read;
    float max_rms = 1000000.0f;  // Initial max RMS for display scaling

    ESP_LOGI(TAG, "Starting audio capture...");
    ESP_LOGI(TAG, "Speak into the microphone to see audio levels");
    printf("\n");

    while (1) {
        /* Read audio data from INMP441 */
        esp_err_t ret = inmp441_read(audio_buffer, AUDIO_BUFFER_SIZE, &bytes_read, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read audio: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t sample_count = bytes_read / sizeof(int32_t);
        if (sample_count == 0) continue;

        /* Calculate audio metrics */
        float rms = calculate_rms(audio_buffer, sample_count);
        int32_t peak = calculate_peak(audio_buffer, sample_count);

        /* Update max RMS for auto-scaling (slow decay) */
        if (rms > max_rms) {
            max_rms = rms;
        } else {
            max_rms *= 0.999f;  // Slow decay
            if (max_rms < 1000000.0f) max_rms = 1000000.0f;  // Minimum threshold
        }

        /* Print level bar */
        print_level_bar(rms, max_rms);

        /* Periodically print detailed info */
        static int counter = 0;
        if (++counter >= 20) {
            counter = 0;
            printf("\n");
            ESP_LOGI(TAG, "RMS: %.0f, Peak: %ld, Samples: %u",
                     rms, (long)peak, (unsigned)sample_count);
        }
    }

    free(audio_buffer);
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "INMP441 Microphone Demo");
    ESP_LOGI(TAG, "=================================");

    /* Initialize INMP441 with default configuration */
    inmp441_config_t config = INMP441_DEFAULT_CONFIG();

    /* You can modify the configuration here if needed */
    // config.sck_io = GPIO_NUM_4;
    // config.ws_io = GPIO_NUM_5;
    // config.sd_io = GPIO_NUM_6;
    // config.sample_rate = 44100;

    esp_err_t ret = inmp441_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize INMP441: %s", esp_err_to_name(ret));
        return;
    }

    /* Create audio capture task */
    xTaskCreate(audio_task, "audio_task", 4096, NULL, 5, NULL);
}