#include "ota/ota_download.h"

#include <ctype.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota/ota_manifest.h"
#include "ota/ota_verify.h"
#include "system/task_watchdog.h"

static const char *TAG = "OTA_SERVICE";

/* TEMPORARY DIAGNOSTIC: walk the whole heap at key points of the download and
 * log the first step after which it is corrupted. Set to 0 (or delete the
 * OTA_HEAP_CHECK lines) once the corruption source is found. */
#define OTA_HEAP_DIAG 0
#if OTA_HEAP_DIAG
#define OTA_HEAP_CHECK(label)                                                  \
    do {                                                                       \
        const bool heap_ok = heap_caps_check_integrity_all(true);              \
        if (heap_ok) {                                                         \
            ESP_LOGI(TAG, "HEAPCHK ok      : %s", (label));                    \
        } else {                                                               \
            ESP_LOGE(TAG, "HEAPCHK CORRUPT : %s", (label));                    \
        }                                                                      \
    } while (0)
#else
#define OTA_HEAP_CHECK(label) do { } while (0)
#endif

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

    OTA_HEAP_CHECK("1 entry ota_download_firmware");
    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);
    if (!update_partition || expected_size > update_partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = OTA_HTTP_BUFFER_SIZE,
        .buffer_size_tx = OTA_HTTP_BUFFER_SIZE,
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
    /* GitHub release assets are served through a redirect to a signed URL;
     * follow it here, otherwise the 302 is mistaken for a bad image. */
    int64_t content_length = 0;
    int status_code = 0;
    esp_err_t result = ota_http_open_following_redirects(client, &content_length,
                                                         &status_code);
    if (result != ESP_OK) {
        esp_http_client_cleanup(client);
        return result;
    }
    if (cancel_cb && cancel_cb(context)) {
        result = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    if (status_code != 200 || content_length != (int64_t)expected_size) {
        ESP_LOGE(TAG, "Firmware download rejected: HTTP %d, length %lld, expected %u",
                 status_code, (long long)content_length,
                 (unsigned)expected_size);
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    OTA_HEAP_CHECK("2 after HTTP open, before esp_ota_begin");
    ESP_LOGI(TAG, "Download accepted (%u bytes); preparing partition %s",
             (unsigned)expected_size, update_partition->label);
    /* OTA_WITH_SEQUENTIAL_WRITES erases each sector just before it is written
     * instead of erasing the whole image up front. The up-front erase of a
     * ~1 MB image is one long flash operation (cache disabled, both cores
     * stalled) and was followed by a watchdog reset on this device. */
    result = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES,
                           &ota_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(result));
        goto cleanup;
    }
    OTA_HEAP_CHECK("3 after esp_ota_begin");
    ESP_LOGI(TAG, "OTA partition ready; downloading");
    ota_started = true;
    if (cancel_cb && cancel_cb(context)) {
        result = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    result = ota_verify_begin(&verify_context);
    if (result != ESP_OK) {
        goto cleanup;
    }
    OTA_HEAP_CHECK("4 after ota_verify_begin");

    uint8_t buffer[1024];
    uint32_t received = 0U;
    if (progress_cb) {
        progress_cb(0, context);
    }
    OTA_HEAP_CHECK("5 after first progress_cb(0)");
    while (received < expected_size) {
        task_watchdog_feed();
        if (cancel_cb && cancel_cb(context)) {
            result = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }

#if OTA_HEAP_DIAG
        const bool diag_early = received < (8U * 1024U);
        if (diag_early) {
            OTA_HEAP_CHECK("6 before esp_http_client_read");
        }
#endif
        const int read_len = esp_http_client_read(
            client, (char *)buffer,
            (int)((expected_size - received) > sizeof(buffer)
                      ? sizeof(buffer)
                      : (expected_size - received)));
        if (read_len <= 0) {
            result = read_len == 0 ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
            goto cleanup;
        }
#if OTA_HEAP_DIAG
        if (diag_early) {
            OTA_HEAP_CHECK("7 after esp_http_client_read");
        }
#endif
        result = esp_ota_write(ota_handle, buffer, (size_t)read_len);
        if (result != ESP_OK) {
            goto cleanup;
        }
        result = ota_verify_update(&verify_context, buffer, (size_t)read_len);
        if (result != ESP_OK) {
            goto cleanup;
        }
#if OTA_HEAP_DIAG
        if (diag_early) {
            OTA_HEAP_CHECK("8 after esp_ota_write + sha update");
        }
#endif
        received += (uint32_t)read_len;
        if (progress_cb) {
            progress_cb((int)((received * 100U) / expected_size), context);
        }
#if OTA_HEAP_DIAG
        if (diag_early) {
            OTA_HEAP_CHECK("9 after progress_cb");
        }
#endif
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
