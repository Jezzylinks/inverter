#include "ota/ota_download.h"

#include <ctype.h>
#include <stdio.h>
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

static const char *TAG = "OTA_SERVICE";

/* A dropped connection mid-download is resumed with an HTTP Range request
 * instead of failing the whole update. The SHA-256 and the OTA write offset are
 * already incremental, so resuming costs nothing but a new TLS session.
 *
 * The usual cause on this board is the heap, not the network: TLS needs one
 * contiguous ~16.7 KB block for each full-size record, and with ~30-40 KB free
 * in total the largest block can fall just below that. Closing the connection
 * frees its TLS buffers, so give the heap a moment to recover before
 * reconnecting. */
#define OTA_DOWNLOAD_MAX_RESUMES 4U
#define OTA_RESUME_BACKOFF_MS 1500U
#define OTA_RESUME_HEAP_WAIT_MS 4000U
#define OTA_RESUME_HEAP_POLL_MS 250U
/* Same threshold the TLS diagnostics already warn about. */
#define OTA_RESUME_MIN_LARGEST_BLOCK 20000U

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

/* Wait (bounded) for the largest free block to recover after a connection was
 * closed. Returns ESP_ERR_INVALID_STATE if the user cancelled meanwhile. */
static esp_err_t ota_wait_for_heap(ota_download_cancel_cb_t cancel_cb,
                                   void *context)
{
    uint32_t waited_ms = 0U;
    while (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) <
               OTA_RESUME_MIN_LARGEST_BLOCK &&
           waited_ms < OTA_RESUME_HEAP_WAIT_MS) {
        if (cancel_cb && cancel_cb(context)) {
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_RESUME_HEAP_POLL_MS));
        waited_ms += OTA_RESUME_HEAP_POLL_MS;
    }
    return ESP_OK;
}

/* Re-open the download at byte `received`. Returns ESP_OK with the client
 * positioned at that byte, ESP_ERR_INVALID_STATE if cancelled,
 * ESP_ERR_NOT_SUPPORTED if the server will not honour a Range request (retrying
 * cannot help), or the open/transport error otherwise. The original URL is used
 * rather than the signed redirect target, which may have expired. */
static esp_err_t ota_download_resume(esp_http_client_handle_t client,
                                     const char *url, uint32_t received,
                                     uint32_t expected_size, unsigned attempt,
                                     ota_download_cancel_cb_t cancel_cb,
                                     void *context)
{
    ESP_LOGW(TAG, "Resuming download at byte %u of %u (attempt %u/%u)",
             (unsigned)received, (unsigned)expected_size, attempt,
             (unsigned)OTA_DOWNLOAD_MAX_RESUMES);

    esp_http_client_close(client);
    vTaskDelay(pdMS_TO_TICKS(OTA_RESUME_BACKOFF_MS));
    esp_err_t err = ota_wait_for_heap(cancel_cb, context);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "Heap before resume: free=%u largest_block=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    err = esp_http_client_set_url(client, url);
    if (err != ESP_OK) {
        return err;
    }
    if (received == 0U) {
        /* Nothing downloaded yet: a plain request, no Range header. */
        (void)esp_http_client_delete_header(client, "Range");
    } else {
        char range[32];
        (void)snprintf(range, sizeof(range), "bytes=%u-", (unsigned)received);
        err = esp_http_client_set_header(client, "Range", range);
        if (err != ESP_OK) {
            return err;
        }
    }

    int64_t content_length = 0;
    int status_code = 0;
    err = ota_http_open_following_redirects(client, &content_length,
                                            &status_code);
    if (err != ESP_OK) {
        return err;
    }

    const int expected_status = received == 0U ? 200 : 206;
    if (status_code != expected_status ||
        content_length != (int64_t)(expected_size - received)) {
        ESP_LOGE(TAG, "Resume rejected: HTTP %d, length %lld, expected HTTP %d "
                      "and %u bytes",
                 status_code, (long long)content_length, expected_status,
                 (unsigned)(expected_size - received));
        esp_http_client_close(client);
        return status_code == 200 ? ESP_ERR_NOT_SUPPORTED
                                  : ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
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
    unsigned resumes = 0U;
    bool stream_open = true;
    bool low_heap_failure = false;
    int last_percent = 0;
    if (progress_cb) {
        progress_cb(0, context);
    }
    OTA_HEAP_CHECK("5 after first progress_cb(0)");
    /* This task is not subscribed to the ESP task watchdog, so it does not feed
     * it (feeding from an unsubscribed task only logged "TWDT feed rejected"). */
    while (received < expected_size) {
        if (cancel_cb && cancel_cb(context)) {
            result = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }

        if (!stream_open) {
            /* `result` still holds the failure that closed the stream. */
            if (resumes >= OTA_DOWNLOAD_MAX_RESUMES) {
                ESP_LOGE(TAG, "Download failed after %u resume attempts",
                         resumes);
                if (low_heap_failure) {
                    /* Report the real cause: not enough contiguous heap for
                     * TLS, rather than a generic transport failure. */
                    result = ESP_ERR_NO_MEM;
                }
                goto cleanup;
            }
            resumes++;
            const esp_err_t resume_err = ota_download_resume(
                client, url, received, expected_size, resumes, cancel_cb,
                context);
            if (resume_err == ESP_ERR_INVALID_STATE ||
                resume_err == ESP_ERR_NOT_SUPPORTED) {
                result = resume_err; /* cancelled / cannot resume at all */
                goto cleanup;
            }
            if (resume_err != ESP_OK) {
                ESP_LOGW(TAG, "Resume attempt %u failed: %s", resumes,
                         esp_err_to_name(resume_err));
                if (resume_err == ESP_ERR_NO_MEM ||
                    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) <
                        OTA_RESUME_MIN_LARGEST_BLOCK) {
                    low_heap_failure = true;
                }
                result = resume_err;
                continue; /* counts against the resume budget */
            }
            stream_open = true;
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
            const size_t largest =
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            ESP_LOGW(TAG, "Download interrupted at %u/%u bytes (read=%d); "
                          "heap free=%u largest_block=%u",
                     (unsigned)received, (unsigned)expected_size, read_len,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)largest);
            if (largest < OTA_RESUME_MIN_LARGEST_BLOCK) {
                low_heap_failure = true;
            }
            esp_http_client_close(client);
            stream_open = false;
            result = read_len == 0 ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
            continue;
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
        /* Report only when the percentage changes. This used to fire for every
         * 1 KB chunk (~1,400 times), and each call does a websocket broadcast
         * and LCD update: needless allocation churn right when TLS needs a
         * large contiguous block. */
        const int percent = (int)(((uint64_t)received * 100U) / expected_size);
        if (progress_cb && percent != last_percent) {
            last_percent = percent;
            progress_cb(percent, context);
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
