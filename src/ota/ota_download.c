#include "ota/ota_download.h"

#include <ctype.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota/ota_manifest.h"
#include "ota/ota_verify.h"
#include "system/task_watchdog.h"

static const char *TAG = "OTA_SERVICE";

static bool is_sha256(const char *text)
{
    if (!text || !*text || strlen(text) != 64U) {
        return false;
    }
    for (size_t i = 0U; i < 64U; ++i) {
        if (!isxdigit((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

esp_err_t ota_download_firmware(const char *url, const char *expected_sha256,
                                uint32_t expected_size,
                                ota_download_cancel_cb_t cancel_cb,
                                ota_download_progress_cb_t progress_cb,
                                ota_download_verifying_cb_t verifying_cb,
                                void *context)
{
    if (!ota_manifest_is_https_url(url) || expected_size == 0U ||
        !is_sha256(expected_sha256)) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);
    if (!update_partition || expected_size > update_partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;
    ota_verify_context_t verify_context = {0};
    esp_err_t result = esp_http_client_open(client, 0);
    if (result != ESP_OK) {
        esp_http_client_cleanup(client);
        return result;
    }
    if (cancel_cb && cancel_cb(context)) {
        result = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    const int64_t content_length = esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200 ||
        content_length != (int64_t)expected_size) {
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    result = esp_ota_begin(update_partition, expected_size, &ota_handle);
    if (result != ESP_OK) {
        goto cleanup;
    }
    ota_started = true;
    if (cancel_cb && cancel_cb(context)) {
        result = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    result = ota_verify_begin(&verify_context);
    if (result != ESP_OK) {
        goto cleanup;
    }

    uint8_t buffer[1024];
    uint32_t received = 0U;
    if (progress_cb) {
        progress_cb(0, context);
    }
    while (received < expected_size) {
        task_watchdog_feed();
        if (cancel_cb && cancel_cb(context)) {
            result = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }

        const int read_len = esp_http_client_read(
            client, (char *)buffer,
            (int)((expected_size - received) > sizeof(buffer)
                      ? sizeof(buffer)
                      : (expected_size - received)));
        if (read_len <= 0) {
            result = read_len == 0 ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
            goto cleanup;
        }
        result = esp_ota_write(ota_handle, buffer, (size_t)read_len);
        if (result != ESP_OK) {
            goto cleanup;
        }
        result = ota_verify_update(&verify_context, buffer, (size_t)read_len);
        if (result != ESP_OK) {
            goto cleanup;
        }
        received += (uint32_t)read_len;
        if (progress_cb) {
            progress_cb((int)((received * 100U) / expected_size), context);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (verifying_cb) {
        verifying_cb(context);
    }
    if (cancel_cb && cancel_cb(context)) {
        result = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    result = ota_verify_finish(&verify_context, expected_sha256);
    if (result != ESP_OK) {
        goto cleanup;
    }

    if (cancel_cb && cancel_cb(context)) {
        result = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    result = esp_ota_end(ota_handle);
    ota_started = false;
    if (result == ESP_OK) {
        result = esp_ota_set_boot_partition(update_partition);
    }

cleanup:
    ota_verify_abort(&verify_context);
    if (ota_started) {
        esp_ota_abort(ota_handle);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK) {
        ESP_LOGD(TAG, "OTA download transaction ended: %s", esp_err_to_name(result));
    }
    return result;
}
