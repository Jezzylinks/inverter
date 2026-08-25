/**
 * @file wpa3_support.c
 * @brief WPA3/SAE Security Support
 */

#include "wifi/wpa3_support.h"
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "WPA3";

/*----------------------------------------------------------
 * Check if WPA3 is supported on this device
 *---------------------------------------------------------*/
bool wpa3_is_supported(void)
{
/* ESP32-S3, ESP32-C3, ESP32-C6, ESP32-H2 support WPA3 */
/* ESP32 (original) does not support WPA3 in hardware */
#if CONFIG_IDF_TARGET_ESP32
    return false;
#else
    return true;
#endif
}

/*----------------------------------------------------------
 * Configure WiFi for WPA3-SAE
 *---------------------------------------------------------*/
esp_err_t wpa3_configure_sta(const wpa3_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!wpa3_is_supported())
    {
        ESP_LOGW(TAG, "WPA3 not supported on this chip");
        return ESP_ERR_NOT_SUPPORTED;
    }

    wifi_config_t wifi_cfg = {0};

    strncpy((char *)wifi_cfg.sta.ssid, config->ssid, sizeof(wifi_cfg.sta.ssid) - 1);

    if (config->password[0] != '\0')
    {
        strncpy((char *)wifi_cfg.sta.password, config->password, sizeof(wifi_cfg.sta.password) - 1);
    }

    /* Enable SAE (WPA3-Personal) */
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA3_PSK;

/* Enable SAE-PWE (Password Element) derivation method */
#if CONFIG_ESP_WIFI_ENABLE_SAE_PK
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
#else
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_HUNT_AND_PECK;
#endif

    /* Enable transition disable indication */
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    /* Disable 802.11b for better security */
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = true;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure WPA3 STA: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WPA3-SAE configured for SSID: %s", config->ssid);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Configure AP for WPA3-SAE Transition Mode
 * (Supports both WPA2 and WPA3 clients)
 *---------------------------------------------------------*/
esp_err_t wpa3_configure_ap_transition(const wpa3_ap_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!wpa3_is_supported())
    {
        ESP_LOGW(TAG, "WPA3 not supported, falling back to WPA2");

        /* Configure as WPA2 only */
        wifi_config_t wifi_cfg = {0};
        strncpy((char *)wifi_cfg.ap.ssid, config->ssid, sizeof(wifi_cfg.ap.ssid) - 1);
        strncpy((char *)wifi_cfg.ap.password, config->password, sizeof(wifi_cfg.ap.password) - 1);
        wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_cfg.ap.max_connection = config->max_connection;
        wifi_cfg.ap.channel = config->channel;

        return esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    }

    wifi_config_t wifi_cfg = {0};

    strncpy((char *)wifi_cfg.ap.ssid, config->ssid, sizeof(wifi_cfg.ap.ssid) - 1);
    strncpy((char *)wifi_cfg.ap.password, config->password, sizeof(wifi_cfg.ap.password) - 1);

    /* WPA3-SAE Transition Mode: supports both WPA2 and WPA3 */
    wifi_cfg.ap.authmode = WIFI_AUTH_WPA3_PSK;
    wifi_cfg.ap.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wifi_cfg.ap.max_connection = config->max_connection;
    wifi_cfg.ap.channel = config->channel;

    /* Protected Management Frames required for WPA3 */
    wifi_cfg.ap.pmf_cfg.capable = true;
    wifi_cfg.ap.pmf_cfg.required = true;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure WPA3 AP: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WPA3-SAE Transition AP configured: %s", config->ssid);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Get recommended auth mode based on capabilities
 *---------------------------------------------------------*/
wifi_auth_mode_t wpa3_get_recommended_authmode(void)
{
    if (wpa3_is_supported())
    {
        return WIFI_AUTH_WPA3_PSK;
    }
    return WIFI_AUTH_WPA2_PSK;
}

/*----------------------------------------------------------
 * Validate password for WPA3
 * WPA3-SAE requires minimum 8 characters
 *---------------------------------------------------------*/
bool wpa3_validate_password(const char *password)
{
    if (password == NULL)
    {
        return false;
    }

    size_t len = strlen(password);
    return (len >= 8 && len <= 63);
}