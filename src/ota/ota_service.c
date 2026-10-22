#include "ota/ota_service.h"
#include "system/core_affinity.h"

#include <stdlib.h>
#include <string.h>

#include "system/firmware_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ota/ota_download.h"
#include "ota/ota_manifest.h"
#include "cloud/cloud_reporting.h"
#include "server/network_services.h"
#include "mbedtls/md.h"
#include <stdio.h>
#include <strings.h>
static const char *TAG = "OTA_SERVICE";

static void secure_zero(void *ptr, size_t len)
{
    volatile uint8_t *bytes = (volatile uint8_t *)ptr;
    while (len-- > 0U) {
        *bytes++ = 0U;
    }
}

static ota_progress_callback_t s_progress_cb;
static ota_status_callback_t s_status_cb;
static TaskHandle_t s_ota_task;
static SemaphoreHandle_t s_ota_mutex;
static bool s_in_progress;
static bool s_cancel_requested;
static bool s_rollback_notification_pending;
static volatile esp_err_t s_last_error = ESP_OK;
static bool s_pending_image_verified;

#define OTA_PENDING_NVS_NAMESPACE "ota_pending"
#define OTA_PENDING_KEY "pending"
#define OTA_PENDING_PART_KEY "part"
#define OTA_PENDING_VER_KEY "version"
#define OTA_PENDING_SHA_KEY "sha256"
#define OTA_PENDING_SIZE_KEY "size"

static esp_err_t ota_pending_store(const esp_partition_t *partition,
                                   const char *version,
                                   const char *sha256,
                                   uint32_t image_size)
{
    if (partition == NULL || version == NULL || version[0] == '\0' ||
        sha256 == NULL || strlen(sha256) != 64U || image_size == 0U ||
        image_size > partition->size) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_PENDING_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, OTA_PENDING_KEY, 1U);
    if (err == ESP_OK) err = nvs_set_str(handle, OTA_PENDING_PART_KEY, partition->label);
    if (err == ESP_OK) err = nvs_set_str(handle, OTA_PENDING_VER_KEY, version);
    if (err == ESP_OK) err = nvs_set_str(handle, OTA_PENDING_SHA_KEY, sha256);
    if (err == ESP_OK) err = nvs_set_u32(handle, OTA_PENDING_SIZE_KEY, image_size);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) s_pending_image_verified = false;
    return err;
}

/* Read integrity metadata separately so older marker records can still be
 * identified and safely discarded rather than silently treated as installable. */
static esp_err_t ota_pending_integrity_read(char *sha256, size_t sha_len,
                                            uint32_t *image_size)
{
    if (sha256 == NULL || sha_len < OTA_MAX_SHA256_LENGTH || image_size == NULL)
        return ESP_ERR_INVALID_ARG;
    sha256[0] = '\0';
    *image_size = 0U;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_PENDING_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t n = sha_len;
    err = nvs_get_str(handle, OTA_PENDING_SHA_KEY, sha256, &n);
    if (err == ESP_OK) err = nvs_get_u32(handle, OTA_PENDING_SIZE_KEY, image_size);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    if (strlen(sha256) != 64U || *image_size == 0U) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

/* Hash exactly the recorded firmware payload from flash using a small fixed
 * buffer. This runs before shutdown; it does not allocate an image-sized block. */
static esp_err_t ota_pending_verify_image(const esp_partition_t *partition,
                                          const char *version)
{
    if (partition == NULL || version == NULL) return ESP_ERR_INVALID_ARG;

    char expected_sha[OTA_MAX_SHA256_LENGTH] = {0};
    uint32_t image_size = 0U;
    esp_err_t err = ota_pending_integrity_read(expected_sha, sizeof(expected_sha),
                                               &image_size);
    if (err != ESP_OK) return err;
    if (image_size > partition->size) return ESP_ERR_INVALID_SIZE;

    char current_version[OTA_MAX_VERSION_LENGTH] = {0};
    err = ota_service_get_current_version(current_version, sizeof(current_version));
    if (err != ESP_OK) return err;
    if (ota_manifest_compare_versions(version, current_version) <= 0) {
        ESP_LOGW(TAG, "Discarding stale staged OTA %s; running version is %s",
                 version, current_version);
        return ESP_ERR_INVALID_VERSION;
    }

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) return ESP_FAIL;
    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    int md_err = mbedtls_md_setup(&md, md_info, 0);
    if (md_err == 0) md_err = mbedtls_md_starts(&md);
    if (md_err != 0) {
        mbedtls_md_free(&md);
        return ESP_FAIL;
    }

    uint8_t chunk[1024];
    uint32_t offset = 0U;
    while (offset < image_size) {
        const size_t amount = (image_size - offset) > sizeof(chunk)
                                  ? sizeof(chunk) : (size_t)(image_size - offset);
        err = esp_partition_read(partition, offset, chunk, amount);
        if (err != ESP_OK) break;
        md_err = mbedtls_md_update(&md, chunk, amount);
        if (md_err != 0) {
            err = ESP_FAIL;
            break;
        }
        offset += (uint32_t)amount;
        taskYIELD();
    }

    uint8_t digest[32] = {0};
    if (err == ESP_OK && md_err == 0) {
        md_err = mbedtls_md_finish(&md, digest);
        if (md_err != 0) err = ESP_FAIL;
    }
    mbedtls_md_free(&md);
    secure_zero(chunk, sizeof(chunk));
    if (err != ESP_OK) return err;

    char actual_sha[OTA_MAX_SHA256_LENGTH] = {0};
    for (size_t i = 0U; i < sizeof(digest); ++i) {
        (void)snprintf(&actual_sha[i * 2U], 3U, "%02x", digest[i]);
    }
    secure_zero(digest, sizeof(digest));
    if (strcasecmp(actual_sha, expected_sha) != 0) {
        ESP_LOGE(TAG, "Staged OTA %s failed SHA-256 verification", version);
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "Staged OTA verified: version=%s partition=%s size=%lu sha256=match",
             version, partition->label, (unsigned long)image_size);
    return ESP_OK;
}

static esp_err_t ota_pending_read(char *partition_label, size_t label_len,
                                  char *version, size_t version_len)
{
    if (partition_label == NULL || label_len == 0U || version == NULL || version_len == 0U) return ESP_ERR_INVALID_ARG;
    partition_label[0] = '\0'; version[0] = '\0';
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_PENDING_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    uint8_t pending = 0U;
    err = nvs_get_u8(handle, OTA_PENDING_KEY, &pending);
    if (err == ESP_OK && pending == 1U) {
        size_t n = label_len;
        err = nvs_get_str(handle, OTA_PENDING_PART_KEY, partition_label, &n);
        if (err == ESP_OK) { n = version_len; err = nvs_get_str(handle, OTA_PENDING_VER_KEY, version, &n); }
    } else if (err == ESP_OK) err = ESP_ERR_NOT_FOUND;
    nvs_close(handle);
    return err;
}

typedef struct {
    char url[OTA_MAX_URL_LENGTH];
    char expected_version[OTA_MAX_VERSION_LENGTH];
    char expected_sha256[OTA_MAX_SHA256_LENGTH];
    uint32_t expected_size;
    bool from_manifest;
} ota_job_t;

static void notify_status(ota_status_t status, int percent)
{
    ota_status_callback_t callback = NULL;
    if (s_ota_mutex && xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        callback = s_status_cb;
        xSemaphoreGive(s_ota_mutex);
    } else {
        callback = s_status_cb;
    }
    if (callback) {
        callback(status, percent);
    }
}

static void notify_progress(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    ota_progress_callback_t callback = NULL;
    if (s_ota_mutex && xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        callback = s_progress_cb;
        xSemaphoreGive(s_ota_mutex);
    } else {
        callback = s_progress_cb;
    }
    if (callback) {
        callback(percent);
    }
}

static bool cancel_requested(void)
{
    bool requested = false;
    if (s_ota_mutex && xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        requested = s_cancel_requested;
        xSemaphoreGive(s_ota_mutex);
    }
    return requested;
}

static bool cancel_callback(void *context)
{
    (void)context;
    return cancel_requested();
}

static void set_job_finished(void)
{
    if (s_ota_mutex && xSemaphoreTake(s_ota_mutex, portMAX_DELAY) == pdTRUE) {
        s_in_progress = false;
        s_ota_task = NULL;
        xSemaphoreGive(s_ota_mutex);
    } else {
        s_in_progress = false;
        s_ota_task = NULL;
    }
}

static void download_progress_callback(int percent, void *context)
{
    (void)context;
    if (percent == 0) {
        notify_status(OTA_STATUS_DOWNLOADING, 0);
    }
    notify_progress(percent);
}

static void download_verifying_callback(void *context)
{
    (void)context;
    notify_status(OTA_STATUS_VERIFYING, 100);
}

/* uxTaskGetStackHighWaterMark reports the minimum-ever unused stack in words.
 * Log both units so low headroom can be compared with the configured task size. */
static void ota_log_task_stack_headroom(const char *stage)
{
    const UBaseType_t words = uxTaskGetStackHighWaterMark(NULL);
    const size_t bytes = (size_t)words * sizeof(StackType_t);
    ESP_LOGI(TAG, "OTA task stack headroom [%s]: %u words (%u bytes)",
             stage ? stage : "unknown", (unsigned)words, (unsigned)bytes);
    if (words < 200U) {
        ESP_LOGW(TAG, "OTA task stack headroom is critically low (<200 words); "
                      "increase OTA_TASK_STACK_SIZE after confirming this measurement");
    }
}

static void ota_task(void *parameter)
{
    ota_job_t *job = (ota_job_t *)parameter;
    if (!job) {
        ota_log_task_stack_headroom("invalid job");
        set_job_finished();
        vTaskDelete(NULL);
        return;
    }

    notify_status(OTA_STATUS_STARTED, 0);
    /* Keep the TLS heap available to the OTA manifest and image sessions. */
    cloud_reporting_set_ota_paused(true);
    network_services_status_t network_status = {0};
    network_services_get_status(&network_status);
    const bool reconnect_mqtt_after_failure = network_status.mqtt_connected;
    if (reconnect_mqtt_after_failure) {
        (void)network_services_mqtt_disconnect();
    }
    /* Allow any already-running report to finish before opening OTA TLS. */
    for (unsigned wait_ms = 0U; wait_ms < 13000U; wait_ms += 250U) {
        cloud_reporting_status_t cloud_status = {0};
        if (cloud_reporting_get_status(&cloud_status) != ESP_OK ||
            !cloud_status.publish_in_progress) {
            break;
        }
        if (cancel_requested()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250U));
    }
    esp_err_t result = ESP_OK;
    if (job->from_manifest) {
        ota_manifest_entry_t entry = {0};
        result = ota_manifest_fetch_and_parse(job->url, &entry,
                                              cancel_callback, NULL);
        if (result == ESP_OK) {
            char current_version[OTA_MAX_VERSION_LENGTH] = {0};
            result = ota_service_get_current_version(current_version,
                                                     sizeof(current_version));
            if (result == ESP_OK &&
                ota_manifest_compare_versions(entry.version, current_version) <= 0) {
                ESP_LOGW(TAG, "Rejecting OTA version %s; current version is %s",
                         entry.version, current_version);
                result = ESP_ERR_INVALID_VERSION;
            }
        }
        if (result == ESP_OK) {
            strncpy(job->url, entry.url, sizeof(job->url) - 1U);
            strncpy(job->expected_version, entry.version,
                    sizeof(job->expected_version) - 1U);
            strncpy(job->expected_sha256, entry.sha256,
                    sizeof(job->expected_sha256) - 1U);
            job->expected_size = entry.image_size;
            job->from_manifest = false;
        }
    }
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "OTA download started for version %s", job->expected_version);
        result = ota_download_firmware(job->url, job->expected_sha256,
                                       job->expected_size, cancel_callback,
                                       download_progress_callback,
                                       download_verifying_callback, NULL);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Verified OTA failed: %s", esp_err_to_name(result));
        s_last_error = result;
        if (result == ESP_ERR_INVALID_STATE) {
            notify_status(OTA_STATUS_CANCELLED, 0);
        } else {
            notify_status(OTA_STATUS_FAILED, 0);
        }
        cloud_reporting_set_ota_paused(false);
        if (reconnect_mqtt_after_failure) {
            (void)network_services_mqtt_connect();
        }
        ota_log_task_stack_headroom("failed/cancelled");
        secure_zero(job, sizeof(*job));
        free(job);
        set_job_finished();
        vTaskDelete(NULL);
        return;
    }

    result = ota_pending_store(esp_ota_get_next_update_partition(NULL),
                               job->expected_version,
                               job->expected_sha256,
                               job->expected_size);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Verified image metadata could not be saved: %s", esp_err_to_name(result));
        s_last_error = result;
        notify_status(OTA_STATUS_FAILED, 0);
        cloud_reporting_set_ota_paused(false);
        if (reconnect_mqtt_after_failure) (void)network_services_mqtt_connect();
        secure_zero(job, sizeof(*job)); free(job); set_job_finished(); vTaskDelete(NULL); return;
    }
    ota_log_task_stack_headroom("verified/pending activation");
    ESP_LOGI(TAG, "OTA image verified and pending explicit activation (version %s)",
             job->expected_version[0] ? job->expected_version : "unknown");
    notify_status(OTA_STATUS_SUCCESS, 100);
    cloud_reporting_set_ota_paused(false);
    if (reconnect_mqtt_after_failure) (void)network_services_mqtt_connect();
    secure_zero(job, sizeof(*job));
    free(job);
    set_job_finished();
    vTaskDelete(NULL);
}

static esp_err_t queue_job(ota_job_t *job)
{
    if (!job) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ota_mutex) {
        const esp_err_t init_err = ota_service_init();
        if (init_err != ESP_OK) {
            secure_zero(job, sizeof(*job));
            free(job);
            return init_err;
        }
    }
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) {
        secure_zero(job, sizeof(*job));
        free(job);
        return ESP_ERR_TIMEOUT;
    }
    if (s_in_progress) {
        secure_zero(job, sizeof(*job));
        free(job);
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_cancel_requested = false;
    s_in_progress = true;
    const BaseType_t created = xTaskCreatePinnedToCore(ota_task, "ota_task", OTA_TASK_STACK_SIZE,
                                           job, OTA_TASK_PRIORITY, &s_ota_task, APP_CORE_SYSTEM);
    if (created != pdPASS) {
        secure_zero(job, sizeof(*job));
        free(job);
        s_in_progress = false;
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_ota_mutex);
    return ESP_OK;
}

esp_err_t ota_service_init(void)
{
    if (!s_ota_mutex) {
        s_ota_mutex = xSemaphoreCreateMutex();
        if (!s_ota_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition: %s", running ? running->label : "unknown");
#if CONFIG_APP_ROLLBACK_ENABLE
    if (esp_ota_get_last_invalid_partition() != NULL) {
        s_rollback_notification_pending = true;
        ESP_LOGW(TAG, "Previous OTA image was rolled back");
    }
#endif
    return ESP_OK;
}

esp_err_t ota_service_validate_running_app(bool healthy)
{
#if CONFIG_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t state_err = running
                                    ? esp_ota_get_state_partition(running, &state)
                                    : ESP_ERR_NOT_FOUND;
    const bool pending_verify = state_err == ESP_OK &&
                                state == ESP_OTA_IMG_PENDING_VERIFY;
    if (!healthy) {
        if (pending_verify) {
            ESP_LOGE(TAG, "Startup health validation failed; requesting rollback");
            return esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (pending_verify) {
        const esp_err_t mark_err = esp_ota_mark_app_valid_cancel_rollback();
        if (mark_err != ESP_OK) {
            ESP_LOGE(TAG, "Could not mark running firmware valid: %s",
                     esp_err_to_name(mark_err));
            return mark_err;
        }
        ESP_LOGI(TAG, "Startup health validation passed; firmware marked valid");
    }
    if (s_rollback_notification_pending) {
        (void)esp_ota_erase_last_boot_app_partition();
    }
    return ESP_OK;
#else
    (void)healthy;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool ota_service_rollback_notification_pending(void)
{
    bool pending = false;
    if (s_ota_mutex && xSemaphoreTake(s_ota_mutex, portMAX_DELAY) == pdTRUE) {
        pending = s_rollback_notification_pending;
        s_rollback_notification_pending = false;
        xSemaphoreGive(s_ota_mutex);
    } else {
        pending = s_rollback_notification_pending;
        s_rollback_notification_pending = false;
    }
    return pending;
}

esp_err_t ota_service_start(const char *url)
{
    /* Direct URL updates are intentionally disabled: verified manifest
     * metadata is required for every firmware update. */
    (void)url;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ota_service_start_from_csv(const char *csv_url)
{
    if (!ota_manifest_is_https_url(csv_url)) {
        return ESP_ERR_INVALID_ARG;
    }
    ota_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        return ESP_ERR_NO_MEM;
    }
    strncpy(job->url, csv_url, sizeof(job->url) - 1U);
    job->from_manifest = true;
    return queue_job(job);
}

esp_err_t ota_service_check_csv_manifest(const char *csv_url,
                                         ota_manifest_entry_t *entry,
                                         bool *update_available)
{
    if (!entry || !update_available) {
        return ESP_ERR_INVALID_ARG;
    }
    *update_available = false;
    esp_err_t err = ota_manifest_fetch_and_parse(csv_url, entry, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    char current_version[OTA_MAX_VERSION_LENGTH] = {0};
    err = ota_service_get_current_version(current_version, sizeof(current_version));
    if (err != ESP_OK) {
        return err;
    }
    *update_available = ota_manifest_compare_versions(entry->version,
                                                      current_version) > 0;
    return ESP_OK;
}

esp_err_t ota_service_cancel(void)
{
    if (!s_ota_mutex) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_in_progress) {
        s_cancel_requested = true;
    }
    xSemaphoreGive(s_ota_mutex);
    return ESP_OK;
}

esp_err_t ota_service_check_version(const char *version_url, char *new_version,
                                    size_t version_len)
{
    if (!new_version || version_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    char response[OTA_MAX_MANIFEST_BYTES + 1U] = {0};
    size_t response_len = 0U;
    esp_err_t err = ota_manifest_fetch_text(version_url, response, sizeof(response),
                                            &response_len, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    if (strchr(response, ',') != NULL) {
        ota_manifest_entry_t entry;
        err = ota_manifest_parse_csv(response, response_len, &entry);
        if (err != ESP_OK) {
            return err;
        }
        if (strlen(entry.version) >= version_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        strcpy(new_version, entry.version);
        return ESP_OK;
    }

    char *version = ota_manifest_trim(response);
    if (*version == '\0' || strlen(version) >= version_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(new_version, version);
    return ESP_OK;
}

esp_err_t ota_service_get_last_error(void)
{
    return s_last_error;
}

esp_err_t ota_service_get_current_version(char *buffer, size_t len)
{
    if (!buffer || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(buffer, INVERTER_FIRMWARE_VERSION, len - 1U);
    buffer[len - 1U] = '\0';
    return ESP_OK;
}

esp_err_t ota_service_register_progress_callback(ota_progress_callback_t callback)
{
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ota_mutex && ota_service_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    s_progress_cb = callback;
    xSemaphoreGive(s_ota_mutex);
    return ESP_OK;
}

esp_err_t ota_service_register_status_callback(ota_status_callback_t callback)
{
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ota_mutex && ota_service_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    s_status_cb = callback;
    xSemaphoreGive(s_ota_mutex);
    return ESP_OK;
}


bool ota_service_update_pending(void)
{
    char version[OTA_MAX_VERSION_LENGTH] = {0};
    return ota_service_get_pending_version(version, sizeof(version)) == ESP_OK;
}

bool ota_service_pending_image_verified(void)
{
    return s_pending_image_verified && ota_service_update_pending();
}

static esp_err_t ota_pending_discard_partition(const esp_partition_t *partition)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_err_t clear_err = ota_service_clear_pending();
    if (clear_err != ESP_OK) return clear_err;

    /* Never erase the running image or current boot target, even when metadata
     * is stale. Clearing the marker is enough to make it non-installable. */
    if (partition == NULL || partition == running || partition == boot) {
        ESP_LOGW(TAG, "Pending marker cleared; staged partition retained for safety");
        return ESP_OK;
    }
    const esp_err_t erase_err =
        esp_partition_erase_range(partition, 0U, partition->size);
    if (erase_err != ESP_OK) {
        ESP_LOGW(TAG, "Pending marker cleared but staged partition erase failed: %s",
                 esp_err_to_name(erase_err));
    }
    return erase_err;
}

esp_err_t ota_service_get_pending_version(char *buffer, size_t len)
{
    if (buffer == NULL || len == 0U) return ESP_ERR_INVALID_ARG;
    char label[17] = {0};
    esp_err_t err = ota_pending_read(label, sizeof(label), buffer, len);
    if (err != ESP_OK) return err;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
    if (partition == NULL) {
        (void)ota_pending_discard_partition(NULL);
        buffer[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    if (partition == esp_ota_get_running_partition()) {
        (void)ota_pending_discard_partition(partition);
        buffer[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    /* Old markers without digest/size are not trusted as staged images. */
    char pending_sha[OTA_MAX_SHA256_LENGTH] = {0};
    uint32_t pending_size = 0U;
    err = ota_pending_integrity_read(pending_sha, sizeof(pending_sha), &pending_size);
    if (err != ESP_OK || pending_size > partition->size) {
        ESP_LOGW(TAG, "Discarding staged OTA with missing/invalid integrity metadata");
        (void)ota_pending_discard_partition(partition);
        buffer[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    char current_version[OTA_MAX_VERSION_LENGTH] = {0};
    if (ota_service_get_current_version(current_version, sizeof(current_version)) == ESP_OK &&
        ota_manifest_compare_versions(buffer, current_version) <= 0) {
        ESP_LOGW(TAG, "Staged OTA %s is not newer than running %s; discarding",
                 buffer, current_version);
        (void)ota_pending_discard_partition(partition);
        buffer[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

/* Verify the staged bytes before the application shuts down inverter output.
 * Invalid, stale, or legacy metadata is cleared so the same broken image is not
 * offered on every boot. */
esp_err_t ota_service_validate_pending_for_apply(void)
{
    s_pending_image_verified = false;
    char label[17] = {0}, version[OTA_MAX_VERSION_LENGTH] = {0};
    esp_err_t err = ota_pending_read(label, sizeof(label), version, sizeof(version));
    if (err != ESP_OK) return err;

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
    if (partition == NULL) {
        (void)ota_pending_discard_partition(NULL);
        return ESP_ERR_NOT_FOUND;
    }
    if (partition == esp_ota_get_running_partition()) {
        (void)ota_pending_discard_partition(partition);
        return ESP_ERR_INVALID_STATE;
    }

    err = ota_pending_verify_image(partition, version);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Rejecting staged OTA: version=%s partition=%s result=%s",
                 version, partition->label, esp_err_to_name(err));
        const esp_err_t discard_err = ota_pending_discard_partition(partition);
        if (discard_err != ESP_OK) {
            ESP_LOGE(TAG, "Could not fully discard invalid staged OTA: %s",
                     esp_err_to_name(discard_err));
        }
        return err;
    }
    s_pending_image_verified = true;
    ESP_LOGI(TAG, "Staged OTA preflight verified: version=%s partition=%s",
             version, partition->label);
    return ESP_OK;
}

/* Boot selection is deliberately fast. The background preflight must have
 * hashed the staged image before output shutdown; do not repeat a megabyte-scale
 * flash read in the button callback or after the inverter has been shut down. */
esp_err_t ota_service_select_pending_for_boot(void)
{
    if (!s_pending_image_verified) return ESP_ERR_INVALID_STATE;
    char label[17] = {0}, version[OTA_MAX_VERSION_LENGTH] = {0};
    esp_err_t err = ota_pending_read(label, sizeof(label), version, sizeof(version));
    if (err != ESP_OK) return err;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
    if (partition == NULL) return ESP_ERR_NOT_FOUND;
    if (partition == esp_ota_get_running_partition()) return ESP_ERR_INVALID_STATE;

    char current_version[OTA_MAX_VERSION_LENGTH] = {0};
    err = ota_service_get_current_version(current_version, sizeof(current_version));
    if (err != ESP_OK || ota_manifest_compare_versions(version, current_version) <= 0) {
        s_pending_image_verified = false;
        return err != ESP_OK ? err : ESP_ERR_INVALID_VERSION;
    }

    ESP_LOGI(TAG, "Applying staged OTA: version=%s target=%s preflight=verified",
             version, partition->label);
    err = esp_ota_set_boot_partition(partition);
    ESP_LOGI(TAG, "OTA boot selection result: version=%s target=%s result=%s",
             version, partition->label, esp_err_to_name(err));
    if (err != ESP_OK) return err;

    const esp_err_t clear_err = ota_service_clear_pending();
    if (clear_err != ESP_OK) {
        ESP_LOGW(TAG, "Selected update boot target; pending marker cleanup failed: %s",
                 esp_err_to_name(clear_err));
    }
    return ESP_OK;
}

esp_err_t ota_service_clear_pending(void)
{
    s_pending_image_verified = false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_PENDING_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, OTA_PENDING_KEY, 0U);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t ota_service_delete_pending(void)
{
    char label[17] = {0}, version[OTA_MAX_VERSION_LENGTH] = {0};
    esp_err_t err = ota_pending_read(label, sizeof(label), version, sizeof(version));
    if (err != ESP_OK) return err;

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
    if (partition == NULL) return ESP_ERR_NOT_FOUND;

    /* Never erase the running image or whichever partition is already selected
     * for the next boot. This guards against stale metadata after a partial apply. */
    if (partition == esp_ota_get_running_partition() ||
        partition == esp_ota_get_boot_partition()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Clear the pointer first: even if erase fails, an incomplete image can
     * never remain advertised as installable or be selected by the UI. */
    err = ota_service_clear_pending();
    if (err != ESP_OK) return err;
    return esp_partition_erase_range(partition, 0U, partition->size);
}

bool ota_service_in_progress(void)
{
    if (!s_ota_mutex) {
        return false;
    }
    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    const bool in_progress = s_in_progress;
    xSemaphoreGive(s_ota_mutex);
    return in_progress;
}
