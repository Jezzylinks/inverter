/**
 * @file wifi_captive_portal.c
 * @brief WiFi Captive Portal
 */

#include "wifi_captive_portal.h"
#include "esp_log.h"
#include "wifi_http_server.h"
#include "wifi_dns_server.h"

static const char *TAG = "WIFI_CAPTIVE";

static bool s_running = false;

/*----------------------------------------------------------
 * Initialize
 *---------------------------------------------------------*/

esp_err_t wifi_captive_portal_init(void)
{
    s_running = false;

    ESP_LOGI(TAG,
             "Captive portal initialized");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Deinitialize
 *---------------------------------------------------------*/

esp_err_t wifi_captive_portal_deinit(void)
{
    if (s_running)
    {
        wifi_captive_portal_stop();
    }

    ESP_LOGI(TAG,
             "Captive portal deinitialized");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Start
 *---------------------------------------------------------*/

esp_err_t wifi_captive_portal_start(void)
{
    if (s_running)
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG,
             "Starting captive portal");

    ESP_ERROR_CHECK(
        wifi_http_server_start());

    ESP_ERROR_CHECK(
        wifi_dns_server_start());

    /*
     * DNS server redirection
     * will be added here.
     */

    s_running = true;

    return ESP_OK;
}

/*----------------------------------------------------------
 * Stop
 *---------------------------------------------------------*/

esp_err_t wifi_captive_portal_stop(void)
{
    if (!s_running)
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG,
             "Stopping captive portal");

    wifi_http_server_stop();
    wifi_dns_server_stop();

    s_running = false;

    return ESP_OK;
}

/*----------------------------------------------------------
 * Status
 *---------------------------------------------------------*/

bool wifi_captive_portal_is_running(void)
{
    return s_running;
}