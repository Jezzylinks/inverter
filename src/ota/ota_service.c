/**
 * @file ota_service.c
 * @brief Over-the-Air Firmware Update Service
 */

#include "ota_service.h"
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"

static const char *TAG = "OTA_SERVICE";

static ota_progress_callback_t s_progress_cb = NULL;
static ota_status_callback_t s_status_cb = NULL;
static TaskHandle_t s_ota_task = NULL;
static bool s_in_progress = false;

/*----------------------------------------------------------
 * OTA Event Handler
 *---------------------------------------------------------*/
static esp_err_t ota_event_handler(esp_https_ota_event_t event)
{
    switch (event)
    {
    case ESP_HTTPS_OTA_START:
        ESP_LOGI(TAG, "OTA started");
        if (s_status_cb)
            s_status_cb(OTA_STATUS_STARTED, 0);
        break;
    case ESP_HTTPS_OTA_CONNECTED:
        ESP_LOGI(TAG, "OTA connected to server");
        if (s_status_cb)
            s_status_cb(OTA_STATUS_DOWNLOADING, 0);
        break;
    case ESP_HTTPS_OTA_GET_IMG_DESC:
        ESP_LOGI(TAG, "OTA reading image description");
        break;
    case ESP_HTTPS_OTA_VERIFY_CHIP_ID:
        ESP_LOGI(TAG, "OTA verifying chip ID");
        break;
    case ESP_HTTPS_OTA_DECRYPT_CB:
        break;
    case ESP_HTTPS_OTA_WRITE_FLASH:
        if (s_progress_cb)
            s_progress_cb(50); /* Approximate */
        break;
    case ESP_HTTPS_OTA_FINISH:
        ESP_LOGI(TAG, "OTA download finished");
        if (s_status_cb)
            s_status_cb(OTA_STATUS_VERIFYING, 100);
        break;
    case ESP_HTTPS_OTA_ABORT:
        ESP_LOGE(TAG, "OTA aborted");
        if (s_status_cb)
            s_status_cb(OTA_STATUS_FAILED, 0);
        break;
    default:
        break;
    }
    return ESP_OK;
}

/*----------------------------------------------------------
 * OTA Task
 *---------------------------------------------------------*/
static void ota_task(void *pvParameter)
{
    const char *url = (const char *)pvParameter;

    s_in_progress = true;

    esp_err_t ota_finish_err = ESP_OK;

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .event_handler = ota_event_handler,
    };

    esp_err_t ret = esp_https_ota_begin(&ota_config, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        if (s_status_cb)
            s_status_cb(OTA_STATUS_FAILED, 0);
        goto cleanup;
    }

    /* Perform OTA */
    while (1)
    {
        ret = esp_https_ota_perform(NULL);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        {
            break;
        }

        /* Report progress */
        if (s_progress_cb)
        {
            int64_t dl = esp_https_ota_get_image_len_read(NULL);
            int64_t total = esp_https_ota_get_image_size(NULL);
            if (total > 0)
            {
                s_progress_cb((int)((dl * 100) / total));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(ret));
        if (s_status_cb)
            s_status_cb(OTA_STATUS_FAILED, 0);
        esp_https_ota_abort(NULL);
        goto cleanup;
    }

    ota_finish_err = esp_https_ota_finish(NULL);
    if (ota_finish_err != ESP_OK)
    {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(ota_finish_err));
        if (s_status_cb)
            s_status_cb(OTA_STATUS_FAILED, 0);
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA successful, restarting...");
    if (s_status_cb)
        s_status_cb(OTA_STATUS_SUCCESS, 100);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

cleanup:
    s_in_progress = false;
    s_ota_task = NULL;
    free((void *)url);
    vTaskDelete(NULL);
}

/*----------------------------------------------------------
 * Initialize
 *---------------------------------------------------------*/
esp_err_t ota_service_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition: %s", running->label);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Start OTA
 *---------------------------------------------------------*/
esp_err_t ota_service_start(const char *url)
{
    if (url == NULL || strlen(url) == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_in_progress)
    {
        ESP_LOGW(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    char *url_copy = strdup(url);
    if (url_copy == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK_SIZE,
                                 url_copy, OTA_TASK_PRIORITY, &s_ota_task);
    if (ret != pdPASS)
    {
        free(url_copy);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/*----------------------------------------------------------
 * Cancel OTA
 *---------------------------------------------------------*/
esp_err_t ota_service_cancel(void)
{
    if (!s_in_progress || s_ota_task == NULL)
    {
        return ESP_OK;
    }

    vTaskDelete(s_ota_task);
    s_ota_task = NULL;
    s_in_progress = false;

    ESP_LOGW(TAG, "OTA cancelled");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Check for update
 *---------------------------------------------------------*/
esp_err_t ota_service_check_version(const char *version_url, char *new_version, size_t version_len)
{
    if (version_url == NULL || new_version == NULL || version_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = version_url,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK)
    {
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    int64_t content_len = esp_http_client_get_content_length(client);

    if (status == 200 && content_len > 0 && content_len < (int64_t)version_len)
    {
        int read_len = esp_http_client_read(client, new_version, version_len - 1);
        if (read_len > 0)
        {
            new_version[read_len] = '\0';
            /* Trim whitespace */
            char *end = new_version + strlen(new_version) - 1;
            while (end > new_version && (*end == '\n' || *end == '\r' || *end == ' '))
            {
                *end-- = '\0';
            }
        }
    }

    esp_http_client_cleanup(client);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Get current firmware version
 *---------------------------------------------------------*/
esp_err_t ota_service_get_current_version(char *buffer, size_t len)
{
    if (buffer == NULL || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc == NULL)
    {
        return ESP_FAIL;
    }

    strncpy(buffer, app_desc->version, len - 1);
    buffer[len - 1] = '\0';

    return ESP_OK;
}

/*----------------------------------------------------------
 * Register callbacks
 *---------------------------------------------------------*/
esp_err_t ota_service_register_progress_callback(ota_progress_callback_t callback)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_progress_cb = callback;
    return ESP_OK;
}

esp_err_t ota_service_register_status_callback(ota_status_callback_t callback)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_status_cb = callback;
    return ESP_OK;
}

/*----------------------------------------------------------
 * Is OTA in progress?
 *---------------------------------------------------------*/
bool ota_service_in_progress(void)
{
    return s_in_progress;
}