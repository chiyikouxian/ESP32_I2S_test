/**
 * @file xfyun_iat.c
 * @brief iFlytek IAT (Speech-to-Text) Implementation
 *
 * Flow:
 *  1. Generate HMAC-SHA256 authentication URL
 *  2. Connect to iFlytek WebSocket server
 *  3. Capture audio from INMP441, convert 32bit->16bit
 *  4. Send audio frames (1280 bytes per frame, every 40ms)
 *  5. Receive and parse JSON recognition results
 */

#include "xfyun_iat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "inmp441.h"

static const char *TAG = "XFYUN_IAT";

/* Event bits for WebSocket status */
#define WS_CONNECTED_BIT    BIT0
#define WS_FINISHED_BIT     BIT1
#define WS_ERROR_BIT        BIT2

static EventGroupHandle_t s_ws_event_group;

/* Buffer to accumulate recognition result */
static char s_result_text[1024];

/* ==================== Base64 Helpers ==================== */

/**
 * @brief Base64 encode data
 * @return Allocated string (caller must free), or NULL on error
 */
static char *base64_encode(const unsigned char *data, size_t data_len)
{
    size_t olen = 0;
    mbedtls_base64_encode(NULL, 0, &olen, data, data_len);

    char *output = malloc(olen + 1);
    if (!output) return NULL;

    if (mbedtls_base64_encode((unsigned char *)output, olen + 1, &olen, data, data_len) != 0) {
        free(output);
        return NULL;
    }
    output[olen] = '\0';
    return output;
}

/* ==================== URL Encoding ==================== */

/**
 * @brief URL-encode a string (RFC 3986)
 * @return Allocated string (caller must free)
 */
static char *url_encode(const char *str)
{
    size_t len = strlen(str);
    /* Worst case: each char becomes %XX (3 chars) */
    char *encoded = malloc(len * 3 + 1);
    if (!encoded) return NULL;

    char *p = encoded;
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = c;
        } else {
            p += sprintf(p, "%%%02X", (unsigned char)c);
        }
    }
    *p = '\0';
    return encoded;
}

/* ==================== HMAC-SHA256 ==================== */

/**
 * @brief Compute HMAC-SHA256
 * @param key Secret key
 * @param key_len Key length
 * @param msg Message to sign
 * @param msg_len Message length
 * @param output 32-byte output buffer
 */
static int hmac_sha256(const unsigned char *key, size_t key_len,
                       const unsigned char *msg, size_t msg_len,
                       unsigned char *output)
{
    return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                           key, key_len, msg, msg_len, output);
}

/* ==================== Auth URL Generation ==================== */

/**
 * @brief Generate iFlytek authenticated WebSocket URL
 *
 * Steps:
 *  1. Get current UTC time in RFC 1123 format
 *  2. Build signature_origin = "host: ...\ndate: ...\nGET /v2/iat HTTP/1.1"
 *  3. signature_sha = HMAC-SHA256(api_secret, signature_origin)
 *  4. signature = Base64(signature_sha)
 *  5. authorization_origin = 'api_key="...",algorithm="hmac-sha256",headers="host date request-line",signature="..."'
 *  6. authorization = Base64(authorization_origin)
 *  7. URL = wss://host/path?authorization=...&date=...&host=...
 *
 * @return Allocated URL string (caller must free), or NULL on error
 */
static char *generate_auth_url(void)
{
    /* Step 1: Get current UTC time in RFC 1123 format */
    time_t now;
    time(&now);
    struct tm tm_info;
    gmtime_r(&now, &tm_info);

    char date_str[64];
    strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", &tm_info);
    ESP_LOGI(TAG, "Auth date: %s", date_str);

    /* Step 2: Build signature_origin */
    char signature_origin[256];
    snprintf(signature_origin, sizeof(signature_origin),
             "host: %s\ndate: %s\nGET %s HTTP/1.1",
             XFYUN_IAT_HOST, date_str, XFYUN_IAT_PATH);

    /* Step 3: HMAC-SHA256 */
    unsigned char hmac_result[32];
    if (hmac_sha256((const unsigned char *)XFYUN_API_SECRET, strlen(XFYUN_API_SECRET),
                    (const unsigned char *)signature_origin, strlen(signature_origin),
                    hmac_result) != 0) {
        ESP_LOGE(TAG, "HMAC-SHA256 failed");
        return NULL;
    }

    /* Step 4: Base64 encode signature */
    char *signature = base64_encode(hmac_result, 32);
    if (!signature) {
        ESP_LOGE(TAG, "Base64 encode signature failed");
        return NULL;
    }

    /* Step 5: Build authorization_origin */
    char auth_origin[512];
    snprintf(auth_origin, sizeof(auth_origin),
             "api_key=\"%s\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"%s\"",
             XFYUN_API_KEY, signature);
    free(signature);

    /* Step 6: Base64 encode authorization */
    char *authorization = base64_encode((const unsigned char *)auth_origin, strlen(auth_origin));
    if (!authorization) {
        ESP_LOGE(TAG, "Base64 encode authorization failed");
        return NULL;
    }

    /* Step 7: URL-encode parameters and build final URL */
    char *date_encoded = url_encode(date_str);
    char *auth_encoded = url_encode(authorization);
    char *host_encoded = url_encode(XFYUN_IAT_HOST);
    free(authorization);

    if (!date_encoded || !auth_encoded || !host_encoded) {
        free(date_encoded);
        free(auth_encoded);
        free(host_encoded);
        return NULL;
    }

    /* Build URL: wss://host/path?authorization=...&date=...&host=... */
    size_t url_len = 64 + strlen(XFYUN_IAT_HOST) + strlen(XFYUN_IAT_PATH) +
                     strlen(auth_encoded) + strlen(date_encoded) + strlen(host_encoded);
    char *url = malloc(url_len);
    if (!url) {
        free(date_encoded);
        free(auth_encoded);
        free(host_encoded);
        return NULL;
    }

    snprintf(url, url_len, "wss://%s%s?authorization=%s&date=%s&host=%s",
             XFYUN_IAT_HOST, XFYUN_IAT_PATH,
             auth_encoded, date_encoded, host_encoded);

    free(date_encoded);
    free(auth_encoded);
    free(host_encoded);

    ESP_LOGI(TAG, "Auth URL generated (length=%d)", strlen(url));
    return url;
}

/* ==================== Result Parsing ==================== */

/**
 * @brief Parse iFlytek IAT response JSON and extract text
 *
 * Response format:
 * {
 *   "code": 0,
 *   "data": {
 *     "result": {
 *       "ws": [
 *         { "cw": [{ "w": "word" }] }
 *       ]
 *     },
 *     "status": 2  // 2 means final result
 *   }
 * }
 */
static void parse_iat_response(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return;
    }

    /* Check error code */
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code && code->valueint != 0) {
        cJSON *message = cJSON_GetObjectItem(root, "message");
        ESP_LOGE(TAG, "IAT error code=%d, message=%s",
                 code->valueint, message ? message->valuestring : "unknown");
        cJSON_Delete(root);
        return;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!data) {
        cJSON_Delete(root);
        return;
    }

    /* Extract text from ws[].cw[].w */
    cJSON *result = cJSON_GetObjectItem(data, "result");
    if (result) {
        cJSON *ws = cJSON_GetObjectItem(result, "ws");
        if (ws && cJSON_IsArray(ws)) {
            int ws_size = cJSON_GetArraySize(ws);
            for (int i = 0; i < ws_size; i++) {
                cJSON *ws_item = cJSON_GetArrayItem(ws, i);
                cJSON *cw = cJSON_GetObjectItem(ws_item, "cw");
                if (cw && cJSON_IsArray(cw)) {
                    int cw_size = cJSON_GetArraySize(cw);
                    for (int j = 0; j < cw_size; j++) {
                        cJSON *cw_item = cJSON_GetArrayItem(cw, j);
                        cJSON *w = cJSON_GetObjectItem(cw_item, "w");
                        if (w && w->valuestring) {
                            /* Append to result buffer */
                            size_t cur_len = strlen(s_result_text);
                            size_t w_len = strlen(w->valuestring);
                            if (cur_len + w_len < sizeof(s_result_text) - 1) {
                                strcat(s_result_text, w->valuestring);
                            }
                        }
                    }
                }
            }
        }
    }

    /* Check if this is the final result (status == 2) */
    cJSON *status = cJSON_GetObjectItem(data, "status");
    if (status && status->valueint == 2) {
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "Recognition result: %s", s_result_text);
        ESP_LOGI(TAG, "========================================");
        xEventGroupSetBits(s_ws_event_group, WS_FINISHED_BIT);
    }

    cJSON_Delete(root);
}

/* ==================== WebSocket Event Handler ==================== */

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected to iFlytek");
        xEventGroupSetBits(s_ws_event_group, WS_CONNECTED_BIT);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01 && data->data_len > 0) {
            /* Text frame - parse JSON response */
            char *json = malloc(data->data_len + 1);
            if (json) {
                memcpy(json, data->data_ptr, data->data_len);
                json[data->data_len] = '\0';
                ESP_LOGD(TAG, "Received: %s", json);
                parse_iat_response(json);
                free(json);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        xEventGroupSetBits(s_ws_event_group, WS_ERROR_BIT);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        xEventGroupSetBits(s_ws_event_group, WS_FINISHED_BIT);
        break;

    default:
        break;
    }
}

/* ==================== Audio Conversion ==================== */

/**
 * @brief Convert INMP441 32-bit samples to 16-bit PCM
 *
 * INMP441 outputs 24-bit data MSB-aligned in 32-bit frame.
 * We shift right by 16 to get the top 16 bits as int16_t.
 */
static void convert_32bit_to_16bit(const int32_t *src, int16_t *dst, size_t sample_count)
{
    for (size_t i = 0; i < sample_count; i++) {
        dst[i] = (int16_t)(src[i] >> 16);
    }
}

/* ==================== Build JSON Frames ==================== */

/**
 * @brief Build the first frame JSON (contains common + business + data)
 */
static char *build_first_frame(const unsigned char *audio_data, size_t audio_len)
{
    char *audio_b64 = base64_encode(audio_data, audio_len);
    if (!audio_b64) return NULL;

    cJSON *root = cJSON_CreateObject();

    /* common */
    cJSON *common = cJSON_CreateObject();
    cJSON_AddStringToObject(common, "app_id", XFYUN_APPID);
    cJSON_AddItemToObject(root, "common", common);

    /* business */
    cJSON *business = cJSON_CreateObject();
    cJSON_AddStringToObject(business, "language", "zh_cn");
    cJSON_AddStringToObject(business, "domain", "iat");
    cJSON_AddStringToObject(business, "accent", "mandarin");
    cJSON_AddNumberToObject(business, "vad_eos", 3000);
    cJSON_AddStringToObject(business, "dwa", "wpgs");
    cJSON_AddItemToObject(root, "business", business);

    /* data */
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "status", IAT_STATUS_FIRST);
    cJSON_AddStringToObject(data, "format", "audio/L16;rate=16000");
    cJSON_AddStringToObject(data, "encoding", "raw");
    cJSON_AddStringToObject(data, "audio", audio_b64);
    cJSON_AddItemToObject(root, "data", data);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(audio_b64);

    return json_str;
}

/**
 * @brief Build a continue/last frame JSON (contains only data)
 */
static char *build_audio_frame(const unsigned char *audio_data, size_t audio_len, int status)
{
    char *audio_b64 = base64_encode(audio_data, audio_len);
    if (!audio_b64) return NULL;

    cJSON *root = cJSON_CreateObject();

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "status", status);
    cJSON_AddStringToObject(data, "format", "audio/L16;rate=16000");
    cJSON_AddStringToObject(data, "encoding", "raw");
    cJSON_AddStringToObject(data, "audio", audio_b64);
    cJSON_AddItemToObject(root, "data", data);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(audio_b64);

    return json_str;
}

/* ==================== Main Recognition Function ==================== */

esp_err_t xfyun_iat_recognize(int record_seconds)
{
    esp_err_t ret = ESP_OK;

    if (record_seconds < 1) record_seconds = 1;
    if (record_seconds > 60) record_seconds = 60;

    /* Clear result buffer */
    memset(s_result_text, 0, sizeof(s_result_text));

    /* Create event group */
    s_ws_event_group = xEventGroupCreate();
    if (!s_ws_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    /* Step 1: Generate authentication URL */
    ESP_LOGI(TAG, "Generating auth URL...");
    char *url = generate_auth_url();
    if (!url) {
        ESP_LOGE(TAG, "Failed to generate auth URL");
        vEventGroupDelete(s_ws_event_group);
        return ESP_FAIL;
    }

    /* Step 2: Connect WebSocket */
    ESP_LOGI(TAG, "Connecting to iFlytek...");
    esp_websocket_client_config_t ws_cfg = {
        .uri = url,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    if (!client) {
        ESP_LOGE(TAG, "WebSocket client init failed");
        free(url);
        vEventGroupDelete(s_ws_event_group);
        return ESP_FAIL;
    }

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    ret = esp_websocket_client_start(client);
    free(url);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(client);
        vEventGroupDelete(s_ws_event_group);
        return ret;
    }

    /* Wait for connection */
    EventBits_t bits = xEventGroupWaitBits(s_ws_event_group,
                                           WS_CONNECTED_BIT | WS_ERROR_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(10000));
    if (!(bits & WS_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WebSocket connection timeout");
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        vEventGroupDelete(s_ws_event_group);
        return ESP_ERR_TIMEOUT;
    }

    /* Step 3: Capture and send audio */
    ESP_LOGI(TAG, "Recording for %d seconds... Speak now!", record_seconds);

    /* Allocate buffers */
    /* Read 640 16-bit samples = 1280 bytes of 16-bit PCM (= IAT_FRAME_SIZE) */
    /* That's 640 32-bit samples from I2S = 2560 bytes */
    const size_t samples_per_frame = IAT_FRAME_SIZE / sizeof(int16_t);  /* 640 */
    const size_t i2s_read_size = samples_per_frame * sizeof(int32_t);   /* 2560 */

    int32_t *i2s_buffer = malloc(i2s_read_size);
    int16_t *pcm_buffer = malloc(IAT_FRAME_SIZE);

    if (!i2s_buffer || !pcm_buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffers");
        free(i2s_buffer);
        free(pcm_buffer);
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        vEventGroupDelete(s_ws_event_group);
        return ESP_ERR_NO_MEM;
    }

    int total_frames = (record_seconds * 1000) / IAT_SEND_INTERVAL;
    int frame_count = 0;
    bool first_frame = true;

    while (frame_count < total_frames) {
        /* Check for early termination */
        bits = xEventGroupGetBits(s_ws_event_group);
        if (bits & (WS_FINISHED_BIT | WS_ERROR_BIT)) {
            ESP_LOGW(TAG, "Session ended early");
            break;
        }

        /* Read audio from INMP441 */
        size_t bytes_read = 0;
        ret = inmp441_read(i2s_buffer, i2s_read_size, &bytes_read, 1000);
        if (ret != ESP_OK || bytes_read == 0) {
            ESP_LOGW(TAG, "Audio read failed, skipping frame");
            vTaskDelay(pdMS_TO_TICKS(IAT_SEND_INTERVAL));
            frame_count++;
            continue;
        }

        size_t samples_read = bytes_read / sizeof(int32_t);

        /* Convert 32-bit I2S data to 16-bit PCM */
        convert_32bit_to_16bit(i2s_buffer, pcm_buffer, samples_read);
        size_t pcm_bytes = samples_read * sizeof(int16_t);

        /* Build and send JSON frame */
        char *json = NULL;
        if (first_frame) {
            json = build_first_frame((const unsigned char *)pcm_buffer, pcm_bytes);
            first_frame = false;
            ESP_LOGI(TAG, "Sent first frame (%d bytes PCM)", pcm_bytes);
        } else if (frame_count == total_frames - 1) {
            /* Last frame */
            json = build_audio_frame((const unsigned char *)pcm_buffer, pcm_bytes, IAT_STATUS_LAST);
            ESP_LOGI(TAG, "Sent last frame");
        } else {
            json = build_audio_frame((const unsigned char *)pcm_buffer, pcm_bytes, IAT_STATUS_CONTINUE);
        }

        if (json) {
            if (esp_websocket_client_is_connected(client)) {
                esp_websocket_client_send_text(client, json, strlen(json), pdMS_TO_TICKS(5000));
            }
            free(json);
        }

        frame_count++;

        /* Print progress every 25 frames (~1 second) */
        if (frame_count % 25 == 0) {
            ESP_LOGI(TAG, "Recording... %d/%d seconds",
                     frame_count * IAT_SEND_INTERVAL / 1000, record_seconds);
        }

        vTaskDelay(pdMS_TO_TICKS(IAT_SEND_INTERVAL));
    }

    /* If we haven't sent the last frame yet, send it now */
    if (!(xEventGroupGetBits(s_ws_event_group) & (WS_FINISHED_BIT | WS_ERROR_BIT))) {
        if (!first_frame) {
            /* Send empty last frame to signal end */
            char *json = build_audio_frame((const unsigned char *)"", 0, IAT_STATUS_LAST);
            if (json && esp_websocket_client_is_connected(client)) {
                esp_websocket_client_send_text(client, json, strlen(json), pdMS_TO_TICKS(5000));
                free(json);
            }
            ESP_LOGI(TAG, "Sent end-of-audio signal");
        }
    }

    /* Wait for final result (up to 10 seconds) */
    ESP_LOGI(TAG, "Waiting for recognition result...");
    xEventGroupWaitBits(s_ws_event_group,
                        WS_FINISHED_BIT | WS_ERROR_BIT,
                        pdFALSE, pdFALSE,
                        pdMS_TO_TICKS(10000));

    /* Print final result */
    if (strlen(s_result_text) > 0) {
        printf("\n>>> Recognition result: %s\n\n", s_result_text);
    } else {
        ESP_LOGW(TAG, "No speech recognized");
    }

    /* Cleanup */
    free(i2s_buffer);
    free(pcm_buffer);
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    vEventGroupDelete(s_ws_event_group);

    return ESP_OK;
}
