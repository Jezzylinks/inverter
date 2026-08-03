/**
 * @file wifi_controller.c
 * @brief High Level Wi-Fi Controller
 */

#include "wifi_controller.h"
#include <string.h>

#include "esp_log.h"
#include "freertos/semphr.h"

#include "wifi_storage.h"
#include "wifi_manager.h"
#include "wifi_provision.h"
#include "wifi_scan.h"
#include "wifi_monitor.h"

static const char *TAG = "WIFI_CONTROLLER";

static wifi_controller_state_t s_state = WIFI_CONTROLLER_IDLE;
static bool s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;

static void wifi_controller_lock(void)
{
    if (s_mutex != NULL)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void wifi_controller_unlock(void)
{
    if (s_mutex != NULL)
    {
        xSemaphoreGive(s_mutex);
    }
}

static void wifi_controller_provision_complete(void)
{
    ESP_LOGI(TAG, "Provisioning complete");

    /*
     * New credentials are already stored in NVS.
     * Start normal STA connection.
     *
     * NOTE: This callback runs in the system event task context.
     * wifi_manager_connect() must remain non-blocking here.
     */

    wifi_controller_lock();
    s_state = WIFI_CONTROLLER_CONNECTING;
    wifi_controller_unlock();

    wifi_manager_connect();
}

esp_err_t wifi_controller_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    esp_err_t err;

    /* Create mutex for thread-safe state access */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize storage */
    err = wifi_storage_init();
    if (err != ESP_OK)
    {
        goto rollback;
    }

    /* Initialize WiFi manager */
    err = wifi_manager_init();
    if (err != ESP_OK)
    {
        goto rollback_storage;
    }

    /* Initialize WiFi scan */
    err = wifi_scan_init();
    if (err != ESP_OK)
    {
        goto rollback_manager;
    }

    /* Initialize WiFi monitor */
    err = wifi_monitor_init();
    if (err != ESP_OK)
    {
        goto rollback_scan;
    }

    /* Register provisioning completion callback */
    err = wifi_provision_register_complete_callback(wifi_controller_provision_complete);
    if (err != ESP_OK)
    {
        goto rollback_monitor;
    }

    wifi_controller_lock();
    s_state = WIFI_CONTROLLER_IDLE;
    s_initialized = true;
    wifi_controller_unlock();

    ESP_LOGI(TAG, "WiFi controller initialized");

    return ESP_OK;

rollback_monitor:
    wifi_monitor_deinit();
rollback_scan:
    /* No wifi_scan_deinit available */
rollback_manager:
    wifi_manager_deinit();
rollback_storage:
    /* No wifi_storage_deinit available */
rollback:
    /* Clean up partial initialization */
    if (s_mutex != NULL)
    {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    return err;
}

esp_err_t wifi_controller_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;

    err = wifi_provision_stop();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Provision stop failed: %s", esp_err_to_name(err));
    }

    err = wifi_manager_stop();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Manager stop failed: %s", esp_err_to_name(err));
    }

    err = wifi_manager_deinit();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Manager deinit failed: %s", esp_err_to_name(err));
    }

    wifi_monitor_deinit();

    wifi_controller_lock();
    s_state = WIFI_CONTROLLER_IDLE;
    s_initialized = false;
    wifi_controller_unlock();

    if (s_mutex != NULL)
    {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    ESP_LOGI(TAG, "WiFi controller deinitialized");

    return ESP_OK;
}

esp_err_t wifi_controller_start(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;

    /* Check saved credentials */
    if (wifi_storage_has_credentials())
    {
        ESP_LOGI(TAG, "Credentials found");

        err = wifi_manager_start();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start WiFi manager: %s", esp_err_to_name(err));
            return err;
        }

        err = wifi_manager_connect();
        if (err == ESP_OK)
        {
            wifi_controller_lock();
            s_state = WIFI_CONTROLLER_CONNECTING;
            wifi_controller_unlock();
        }

        /* Start monitoring */
        wifi_monitor_start();

        return err;
    }

    /* No credentials — enter provisioning mode */
    ESP_LOGW(TAG, "No credentials");

    err = wifi_provision_start();
    if (err == ESP_OK)
    {
        wifi_controller_lock();
        s_state = WIFI_CONTROLLER_PROVISIONING;
        wifi_controller_unlock();
    }
    return err;
}

esp_err_t wifi_controller_stop(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;

    wifi_monitor_stop();

    err = wifi_provision_stop();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to stop provisioning: %s", esp_err_to_name(err));
    }

    err = wifi_manager_stop();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to stop manager: %s", esp_err_to_name(err));
    }

    wifi_controller_lock();
    s_state = WIFI_CONTROLLER_IDLE;
    wifi_controller_unlock();

    return ESP_OK;
}

esp_err_t wifi_controller_reconnect(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = wifi_manager_reconnect();
    if (err == ESP_OK)
    {
        wifi_controller_lock();
        s_state = WIFI_CONTROLLER_CONNECTING;
        wifi_controller_unlock();
    }
    return err;
}

esp_err_t wifi_controller_start_provisioning(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = wifi_provision_start();
    if (err == ESP_OK)
    {
        wifi_controller_lock();
        s_state = WIFI_CONTROLLER_PROVISIONING;
        wifi_controller_unlock();
    }
    return err;
}

esp_err_t wifi_controller_stop_provisioning(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = wifi_provision_stop();
    if (err == ESP_OK)
    {
        /* After stopping provisioning, try STA connection */
        if (wifi_storage_has_credentials())
        {
            wifi_manager_connect();
            wifi_controller_lock();
            s_state = WIFI_CONTROLLER_CONNECTING;
            wifi_controller_unlock();
        }
        else
        {
            wifi_controller_lock();
            s_state = WIFI_CONTROLLER_IDLE;
            wifi_controller_unlock();
        }
    }
    return err;
}

wifi_controller_state_t wifi_controller_get_state(void)
{
    wifi_controller_state_t state;

    wifi_controller_lock();
    state = s_state;
    wifi_controller_unlock();

    /* Update state based on actual WiFi status */
    if (s_initialized)
    {
        wifi_connection_state_t wifi_state = wifi_manager_get_state();
        switch (wifi_state)
        {
        case WIFI_STATE_CONNECTED:
            state = WIFI_CONTROLLER_CONNECTED;
            break;
        case WIFI_STATE_CONNECTING:
        case WIFI_STATE_RECONNECTING:
            state = WIFI_CONTROLLER_CONNECTING;
            break;
        case WIFI_STATE_FAILED:
            state = WIFI_CONTROLLER_ERROR;
            break;
        case WIFI_STATE_PROVISIONING:
            state = WIFI_CONTROLLER_PROVISIONING;
            break;
        default:
            break;
        }
    }

    return state;
}

bool wifi_controller_is_connected(void)
{
    if (!s_initialized)
    {
        return false;
    }

    return wifi_manager_is_connected();
}

const wifi_status_t *wifi_controller_get_status(void)
{
    if (!s_initialized)
    {
        return NULL;
    }

    return wifi_manager_get_status();
}

esp_err_t wifi_controller_set_config(const wifi_manager_config_t *config)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return wifi_manager_set_config(config);
}

esp_err_t wifi_controller_get_config(wifi_manager_config_t *config)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return wifi_manager_get_config(config);
}