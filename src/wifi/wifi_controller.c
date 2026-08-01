/**
 * @file wifi_controller.c
 * @brief High Level Wi-Fi Controller
 */

#include "wifi_controller.h"

#include <string.h>

#include "esp_log.h"

#include "wifi_storage.h"
#include "wifi_manager.h"
#include "wifi_provision.h"

static const char *TAG =
    "WIFI_CONTROLLER";

/*==========================================================
 *
 *              PRIVATE DATA
 *
 *=========================================================*/

static wifi_controller_state_t
    s_state = WIFI_CONTROLLER_IDLE;

static bool s_initialized = false;

/*==========================================================
 *
 *              PROVISION CALLBACK
 *
 *=========================================================*/

static void wifi_controller_provision_complete(void)
{
    ESP_LOGI(TAG,
             "Provisioning complete");

    /*
     * New credentials are already
     * stored in NVS.
     *
     * Start normal STA connection.
     */

    s_state =
        WIFI_CONTROLLER_CONNECTING;

    wifi_manager_connect();
}

/*==========================================================
 *
 *              INITIALIZATION
 *
 *=========================================================*/

esp_err_t wifi_controller_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    esp_err_t err;

    /*
     * Initialize storage
     */

    err =
        wifi_storage_init();

    if (err != ESP_OK)
    {
        return err;
    }

    /*
     * Initialize WiFi manager
     */

    err =
        wifi_manager_init();

    if (err != ESP_OK)
    {
        return err;
    }

    wifi_scan_init();

    /*
     * Register provisioning completion
     */

    err =
        wifi_provision_register_complete_callback(
            wifi_controller_provision_complete);

    if (err != ESP_OK)
    {
        return err;
    }

    s_state =
        WIFI_CONTROLLER_IDLE;

    s_initialized = true;

    ESP_LOGI(TAG,
             "WiFi controller initialized");

    return ESP_OK;
}

/*==========================================================
 *
 *              START WIFI
 *
 *=========================================================*/

esp_err_t wifi_controller_start(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Check saved credentials
     */

    if (wifi_storage_has_credentials())
    {
        ESP_LOGI(TAG,
                 "Credentials found");

        s_state =
            WIFI_CONTROLLER_CONNECTING;

        return wifi_manager_connect();
    }

    /*
     * No credentials
     */

    ESP_LOGW(TAG,
             "No credentials");

    s_state =
        WIFI_CONTROLLER_PROVISIONING;

    return wifi_provision_start();
}

/*==========================================================
 *
 *              STOP WIFI
 *
 *=========================================================*/

esp_err_t wifi_controller_stop(void)
{
    esp_err_t err;

    err =
        wifi_provision_stop();

    if (err != ESP_OK)
    {
        return err;
    }

    err =
        wifi_manager_stop();

    s_state =
        WIFI_CONTROLLER_IDLE;

    return err;
}

/*==========================================================
 *
 *              RECONNECT
 *
 *=========================================================*/

esp_err_t wifi_controller_reconnect(void)
{
    s_state =
        WIFI_CONTROLLER_CONNECTING;

    return wifi_manager_reconnect();
}

/*==========================================================
 *
 *              PROVISIONING CONTROL
 *
 *=========================================================*/

esp_err_t wifi_controller_start_provisioning(void)
{
    s_state =
        WIFI_CONTROLLER_PROVISIONING;

    return wifi_provision_start();
}

esp_err_t wifi_controller_stop_provisioning(void)
{
    return wifi_provision_stop();
}

/*==========================================================
 *
 *              STATUS
 *
 *=========================================================*/

wifi_controller_state_t
wifi_controller_get_state(void)
{
    return s_state;
}

bool wifi_controller_is_connected(void)
{
    return wifi_manager_is_connected();
}

const wifi_status_t *
wifi_controller_get_status(void)
{
    return wifi_manager_get_status();
}

/*==========================================================
 *
 *              CONFIGURATION
 *
 *=========================================================*/

esp_err_t wifi_controller_set_config(
    const wifi_manager_config_t *config)
{
    return wifi_manager_set_config(config);
}

esp_err_t wifi_controller_get_config(
    wifi_manager_config_t *config)
{
    return wifi_manager_get_config(config);
}