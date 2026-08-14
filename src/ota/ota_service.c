#include "ota_service.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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

typedef struct {
    char url[OTA_MAX_URL_LENGTH];
    char expected_version[OTA_MAX_VERSION_LENGTH];
} ota_job_t;

static bool is_https_url(const char *url)
{
    return url && strncasecmp(url, "https://", 8) == 0 && strlen(url) < OTA_MAX_URL_LENGTH;
}

static char *trim(char *text)
{
    while (*text && isspace((unsigned char)*text)) {
        ++text;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
    return text;
}

static bool parse_u32(const char *text, uint32_t *value)
{
    if (!text || !*text || !value) {
        return false;
    }
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *trim(end) != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool is_sha256(const char *text)
{
    if (!text || !*text) {
        return true;
    }
    if (strlen(text) != 64U) {
        return false;
    }
    for (size_t i = 0; i < 64U; ++i) {
        if (!isxdigit((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

esp_err_t ota_manifest_parse_csv(const char *csv, size_t csv_len,
                                 ota_manifest_entry_t *entry)
{
    if (!csv || csv_len == 0U || csv_len > OTA_MAX_MANIFEST_BYTES || !entry) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(entry, 0, sizeof(*entry));

    char *copy = calloc(1, csv_len + 1U);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, csv, csv_len);
    copy[csv_len] = '\0';

    char *save_line = NULL;
    char *line = strtok_r(copy, "\n", &save_line);
    while (line) {
        char *current = trim(line);
        if (*current == '\0' || *current == '#') {
            line = strtok_r(NULL, "\n", &save_line);
            continue;
        }

        char *fields[4] = {0};
        char *save_field = NULL;
        size_t field_count = 0;
        char *field = strtok_r(current, ",", &save_field);
        while (field && field_count < 4U) {
            fields[field_count++] = trim(field);
            field = strtok_r(NULL, ",", &save_field);
        }
        if (field != NULL || field_count < 2U) {
            free(copy);
            return ESP_ERR_INVALID_SIZE;
        }
        if (strcasecmp(fields[0], "version") == 0) {
            line = strtok_r(NULL, "\n", &save_line);
            continue;
        }
        if (strlen(fields[0]) >= OTA_MAX_VERSION_LENGTH ||
            strlen(fields[1]) >= OTA_MAX_URL_LENGTH ||
            !is_https_url(fields[1]) ||
            (field_count >= 3U && !is_sha256(fields[2]))) {
            free(copy);
            return ESP_ERR_INVALID_ARG;
        }

        strncpy(entry->version, fields[0], sizeof(entry->version) - 1U);
        strncpy(entry->url, fields[1], sizeof(entry->url) - 1U);
        if (field_count >= 3U && fields[2][0] != '\0') {
            strncpy(entry->sha256, fields[2], sizeof(entry->sha256) - 1U);
        }
        if (field_count >= 4U && fields[3][0] != '\0' &&
            !parse_u32(fields[3], &entry->image_size)) {
            free(copy);
            return ESP_ERR_INVALID_ARG;
        }
        free(copy);
        return ESP_OK;
    }

    free(copy);
    return ESP_ERR_NOT_FOUND;
}

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

static void ota_event_handler(void *handler_arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    (void)handler_arg;
    (void)base;
    (void)event_data;
    switch ((esp_https_ota_event_t)event_id) {
    case ESP_HTTPS_OTA_START:
        ESP_LOGI(TAG, "OTA started");
        notify_status(OTA_STATUS_STARTED, 0);
        break;
    case ESP_HTTPS_OTA_CONNECTED:
        notify_status(OTA_STATUS_DOWNLOADING, 0);
        break;
    case ESP_HTTPS_OTA_GET_IMG_DESC:
        notify_status(OTA_STATUS_VERIFYING, 0);
        break;
    case ESP_HTTPS_OTA_FINISH:
        notify_status(OTA_STATUS_VERIFYING, 100);
        break;
    case ESP_HTTPS_OTA_ABORT:
        ESP_LOGW(TAG, "OTA transport aborted");
        break;
    default:
        break;
    }
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

static void ota_task(void *parameter)
{
    ota_job_t *job = (ota_job_t *)parameter;
    esp_https_ota_handle_t ota_handle = NULL;
    bool event_registered = false;
    esp_err_t result = ESP_FAIL;

    if (!job) {
        set_job_finished();
        vTaskDelete(NULL);
        return;
    }

    result = esp_event_handler_register(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID,
                                        ota_event_handler, NULL);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "failed to register OTA handler: %s", esp_err_to_name(result));
        goto cleanup;
    }
    event_registered = true;

    esp_http_client_config_t http_config = {
        .url = job->url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota_config = {.http_config = &http_config};

    result = esp_https_ota_begin(&ota_config, &ota_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(result));
        goto cleanup;
    }

    while ((result = esp_https_ota_perform(ota_handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (cancel_requested()) {
            ESP_LOGW(TAG, "OTA cancellation requested");
            esp_https_ota_abort(ota_handle);
            ota_handle = NULL;
            notify_status(OTA_STATUS_CANCELLED, 0);
            result = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
        const int image_size = esp_https_ota_get_image_size(ota_handle);
        const int image_read = esp_https_ota_get_image_len_read(ota_handle);
        if (image_size > 0) {
            notify_progress((image_read * 100) / image_size);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "OTA transfer failed: %s", esp_err_to_name(result));
        if (ota_handle) {
            esp_https_ota_abort(ota_handle);
            ota_handle = NULL;
        }
        notify_status(OTA_STATUS_FAILED, 0);
        goto cleanup;
    }

    notify_status(OTA_STATUS_VERIFYING, 100);
    result = esp_https_ota_finish(ota_handle);
    ota_handle = NULL;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "OTA verification failed: %s", esp_err_to_name(result));
        notify_status(OTA_STATUS_FAILED, 0);
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA verified successfully%s%s",
             job->expected_version[0] ? "; manifest version " : "",
             job->expected_version[0] ? job->expected_version : "");
    notify_status(OTA_STATUS_SUCCESS, 100);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

cleanup:
    if (ota_handle) {
        esp_https_ota_abort(ota_handle);
    }
    if (event_registered) {
        esp_event_handler_unregister(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID,
                                     ota_event_handler);
    }
    secure_zero(job, sizeof(*job));
    free(job);
    set_job_finished();
    vTaskDelete(NULL);
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
    return ESP_OK;
}

static esp_err_t start_job(const char *url, const char *expected_version)
{
    if (!is_https_url(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ota_mutex) {
        esp_err_t init_err = ota_service_init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_in_progress) {
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ota_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_NO_MEM;
    }
    strncpy(job->url, url, sizeof(job->url) - 1U);
    if (expected_version) {
        strncpy(job->expected_version, expected_version, sizeof(job->expected_version) - 1U);
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

esp_err_t ota_service_start(const char *url)
{
    return start_job(url, NULL);
}

static esp_err_t http_read_bounded(const char *url, char *buffer, size_t capacity,
                                   size_t *out_len)
{
    if (!is_https_url(url) || !buffer || capacity < 2U || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0U;
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        const int64_t content_length = esp_http_client_fetch_headers(client);
        if (content_length > (int64_t)(capacity - 1U)) {
            err = ESP_ERR_INVALID_SIZE;
        } else if (esp_http_client_get_status_code(client) != 200) {
            err = ESP_FAIL;
        } else {
            while (*out_len < capacity - 1U) {
                const int read_len = esp_http_client_read(client, buffer + *out_len,
                                                          capacity - 1U - *out_len);
                if (read_len < 0) {
                    err = ESP_FAIL;
                    break;
                }
                if (read_len == 0) {
                    break;
                }
                *out_len += (size_t)read_len;
            }
            buffer[*out_len] = '\0';
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t ota_service_start_from_csv(const char *csv_url)
{
    char manifest[OTA_MAX_MANIFEST_BYTES + 1U] = {0};
    size_t manifest_len = 0U;
    esp_err_t err = http_read_bounded(csv_url, manifest, sizeof(manifest), &manifest_len);
    if (err != ESP_OK) {
        return err;
    }
    ota_manifest_entry_t entry;
    err = ota_manifest_parse_csv(manifest, manifest_len, &entry);
    if (err != ESP_OK) {
        return err;
    }
    return start_job(entry.url, entry.version);
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

esp_err_t ota_service_check_version(const char *version_url, char *new_version, size_t version_len)
{
    if (!new_version || version_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    char response[OTA_MAX_MANIFEST_BYTES + 1U] = {0};
    size_t response_len = 0U;
    esp_err_t err = http_read_bounded(version_url, response, sizeof(response), &response_len);
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

    char *version = trim(response);
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
