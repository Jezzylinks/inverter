/**
 * @file wifi_provision.c
 * @brief Wi-Fi Provisioning Manager
 */

#include "wifi_provision.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include "wifi_storage.h"
#include "wifi_http_server.h"

static const char *TAG =
    "WIFI_PROVISION";

static wifi_provision_complete_callback_t s_provision_complete_cb = NULL;
/*==========================================================
 *
 *              PRIVATE DATA
 *
 *=========================================================*/

static wifi_provision_state_t
    s_state = WIFI_PROVISION_IDLE;

static esp_netif_t *
    s_ap_netif = NULL;

/*==========================================================
 *
 *              SOFT AP CONFIG
 *
 *=========================================================*/

static esp_err_t wifi_provision_start_ap(void)
{
    wifi_network_config_t config;

    memset(&config,
           0,
           sizeof(config));

    wifi_storage_load_network_config(
        &config);

    /*
     * Create AP interface
     */

    s_ap_netif =
        esp_netif_create_default_wifi_ap();

    if (s_ap_netif == NULL)
    {
        ESP_LOGE(TAG,
                 "AP netif creation failed");

        return ESP_FAIL;
    }

    wifi_config_t ap_config =
        {
            .ap =
                {
                    .channel = config.ap_channel,

                    .max_connection = 4,

                    .authmode = WIFI_AUTH_WPA2_PSK,
                }};

    strncpy(
        (char *)ap_config.ap.ssid,
        config.ap_ssid,
        sizeof(ap_config.ap.ssid) - 1);

    strncpy(
        (char *)ap_config.ap.password,
        config.ap_password,
        sizeof(ap_config.ap.password) - 1);

    if (strlen(config.ap_password) == 0)
    {
        ap_config.ap.authmode =
            WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_AP));

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_AP,
            &ap_config));

    ESP_LOGI(TAG,
             "AP started: %s",
             config.ap_ssid);

    return ESP_OK;
}

static void wifi_provision_credentials_saved(void)
{
    ESP_LOGI(TAG, "New WiFi credentials saved");

    /*
     * Stop AP provisioning
     */
    wifi_provision_stop();

    /*
     * Notify upper layer
     * (wifi_manager)
     */
    if (s_provision_complete_cb)
    {
        s_provision_complete_cb();
    }
}

/*==========================================================
 *
 *              INITIALIZATION
 *
 *=========================================================*/

esp_err_t wifi_provision_init(void)
{
    s_state =
        WIFI_PROVISION_IDLE;

    return ESP_OK;
}

/*==========================================================
 *
 *              START
 *
 *=========================================================*/

esp_err_t wifi_provision_start(void)
{

    if (s_state == WIFI_PROVISION_RUNNING)
    {
        return ESP_OK;
    }

    esp_err_t err;

    err =
        wifi_provision_start_ap();

    if (err != ESP_OK)
    {
        s_state =
            WIFI_PROVISION_FAILED;

        return err;
    }

    /*
     * Start configuration webpage
     */

    err =
        wifi_http_server_start();

    if (err != ESP_OK)
    {
        s_state =
            WIFI_PROVISION_FAILED;

        return err;
    }

    wifi_http_server_register_save_callback(
        wifi_provision_credentials_saved);

    s_state =
        WIFI_PROVISION_RUNNING;

    ESP_LOGI(TAG,
             "Provisioning started");

    return ESP_OK;
}

/*==========================================================
 *
 *              STOP
 *
 *=========================================================*/

esp_err_t wifi_provision_stop(void)
{

    wifi_http_server_stop();

    esp_wifi_stop();

    if (s_ap_netif)
    {
        esp_netif_destroy(
            s_ap_netif);

        s_ap_netif = NULL;
    }

    s_state =
        WIFI_PROVISION_IDLE;

    ESP_LOGI(TAG,
             "Provisioning stopped");

    return ESP_OK;
}

/*==========================================================
 *
 *              STATUS
 *
 *=========================================================*/

wifi_provision_state_t
wifi_provision_get_state(void)
{
    return s_state;
}

bool wifi_provision_is_running(void)
{
    return (s_state ==
            WIFI_PROVISION_RUNNING);
}

esp_err_t wifi_provision_register_complete_callback(
    wifi_provision_complete_callback_t callback)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_provision_complete_cb = callback;

    return ESP_OK;
}