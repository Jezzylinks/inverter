#include "ota_service.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ota_download.h"
#include "ota_manifest.h"
#include "task_watchdog.h"

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

static void ota_task(void *parameter)
{
    task_watchdog_register("ota_task");
    ota_job_t *job = (ota_job_t *)parameter;
    if (!job) {
        set_job_finished();
        vTaskDelete(NULL);
        return;
    }

    notify_status(OTA_STATUS_STARTED, 0);
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
        if (result == ESP_ERR_INVALID_STATE) {
            notify_status(OTA_STATUS_CANCELLED, 0);
        } else {
            notify_status(OTA_STATUS_FAILED, 0);
        }
        secure_zero(job, sizeof(*job));
        free(job);
        set_job_finished();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA verified successfully; manifest version %s",
             job->expected_version[0] ? job->expected_version : "unknown");
    notify_status(OTA_STATUS_SUCCESS, 100);
    secure_zero(job, sizeof(*job));
    free(job);
    set_job_finished();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
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
    const BaseType_t created = xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK_SIZE,
                                           job, OTA_TASK_PRIORITY, &s_ota_task);
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

esp_err_t ota_service_get_current_version(char *buffer, size_t len)
{
    if (!buffer || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (!app_desc) {
        return ESP_FAIL;
    }
    strncpy(buffer, app_desc->version, len - 1U);
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
