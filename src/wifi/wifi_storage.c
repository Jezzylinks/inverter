/**
 * @file wifi_storage.c
 * @brief Wi-Fi NVS Storage
 */

#include "wifi_storage.h"
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wifi_config.h"

/* Use consistent keys from wifi_config.h */
#define WIFI_KEY_MODE "mode"
#define WIFI_KEY_AUTORECONNECT "autorec"
#define WIFI_KEY_RECONNECT_TIME "rectime"
#define WIFI_KEY_DHCP "dhcp"
#define WIFI_KEY_AP_SSID "apssid"
#define WIFI_KEY_AP_PASS "appass"
#define WIFI_KEY_AP_CHANNEL "apchan"
#define WIFI_KEY_DNS "dns"
#define WIFI_KEY_IP_INFO "ip_info"
#define WIFI_KEY_HOSTNAME "hostname"

/* STA credentials keys - MUST match wifi_config.h */
#define WIFI_KEY_SSID WIFI_NVS_KEY_SSID
#define WIFI_KEY_PASSWORD WIFI_NVS_KEY_PASSWORD

static const char *TAG = "WIFI_STORAGE";

/*==========================================================
 *
 * PRIVATE HELPERS
 *
 *=========================================================*/

static esp_err_t wifi_storage_open(nvs_handle_t *handle, nvs_open_mode_t mode)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return nvs_open(WIFI_NVS_NAMESPACE, mode, handle);
}

static esp_err_t wifi_storage_commit_close(nvs_handle_t handle)
{
    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

/*==========================================================
 *
 * INITIALIZATION
 *
 *=========================================================*/

esp_err_t wifi_storage_init(void)
{
    esp_err_t err;

    err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS init failed (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WiFi storage initialized");

    return ESP_OK;
}

esp_err_t wifi_storage_deinit(void)
{
    /* NVS flash deinit is global; only call if no other users */
    /* nvs_flash_deinit(); */
    return ESP_OK;
}

/*==========================================================
 *
 * SAVE CREDENTIALS
 *
 *=========================================================*/

esp_err_t wifi_storage_save_credentials(
    const wifi_credentials_t *credentials)
{
    if (credentials == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(credentials->ssid) == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t err =
        wifi_storage_open(&handle, NVS_READWRITE);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_str(
        handle,
        WIFI_KEY_SSID,
        credentials->ssid);

    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_str(
        handle,
        WIFI_KEY_PASSWORD,
        credentials->password);

    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = wifi_storage_commit_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Credentials saved");
    }

    return err;
}

/*==========================================================
 *
 * LOAD CREDENTIALS
 *
 *=========================================================*/

esp_err_t wifi_storage_load_credentials(
    wifi_credentials_t *credentials)
{
    if (credentials == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(credentials, 0, sizeof(wifi_credentials_t));

    nvs_handle_t handle;

    esp_err_t err =
        wifi_storage_open(&handle, NVS_READONLY);

    if (err != ESP_OK)
    {
        return err;
    }

    size_t ssid_len = sizeof(credentials->ssid);

    err = nvs_get_str(
        handle,
        WIFI_KEY_SSID,
        credentials->ssid,
        &ssid_len);

    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    size_t password_len = sizeof(credentials->password);

    err = nvs_get_str(
        handle,
        WIFI_KEY_PASSWORD,
        credentials->password,
        &password_len);

    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        credentials->password[0] = '\0';
        err = ESP_OK;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG, "Credentials loaded");

    return ESP_OK;
}

/*==========================================================
 *
 * CHECK IF CREDENTIALS EXIST
 *
 *=========================================================*/

bool wifi_storage_has_credentials(void)
{
    wifi_credentials_t credentials;

    esp_err_t err =
        wifi_storage_load_credentials(&credentials);

    if (err != ESP_OK)
    {
        return false;
    }

    return (strlen(credentials.ssid) > 0);
}

/*==========================================================
 *
 * SAVE HOSTNAME
 *
 *=========================================================*/

esp_err_t wifi_storage_save_hostname(const char *hostname)
{
    if (hostname == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t err =
        wifi_storage_open(&handle, NVS_READWRITE);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_str(handle,
                      WIFI_KEY_HOSTNAME,
                      hostname);

    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    return wifi_storage_commit_close(handle);
}

/*==========================================================
 *
 * LOAD HOSTNAME
 *
 *=========================================================*/

esp_err_t wifi_storage_load_hostname(char *hostname,
                                     size_t max_len)
{
    if (hostname == NULL || max_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t err =
        wifi_storage_open(&handle, NVS_READONLY);

    if (err != ESP_OK)
    {
        return err;
    }

    size_t length = max_len;

    err = nvs_get_str(handle,
                      WIFI_KEY_HOSTNAME,
                      hostname,
                      &length);

    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        strncpy(hostname,
                WIFI_HOSTNAME,
                max_len - 1);

        hostname[max_len - 1] = '\0';

        return ESP_OK;
    }

    return err;
}

/*==========================================================
 *
 * ERASE CREDENTIALS
 *
 *=========================================================*/

esp_err_t wifi_storage_erase_credentials(void)
{
    nvs_handle_t handle;

    esp_err_t err =
        wifi_storage_open(&handle, NVS_READWRITE);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_erase_key(handle, WIFI_KEY_SSID);

    if (err != ESP_OK &&
        err != ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(handle);
        return err;
    }

    err = nvs_erase_key(handle, WIFI_KEY_PASSWORD);

    if (err != ESP_OK &&
        err != ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(handle);
        return err;
    }

    err = wifi_storage_commit_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "WiFi credentials erased");
    }

    return err;
}

/*==========================================================
 *
 * FACTORY RESET
 *
 *=========================================================*/

esp_err_t wifi_storage_factory_reset(void)
{
    nvs_handle_t handle;

    esp_err_t err =
        wifi_storage_open(&handle, NVS_READWRITE);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_erase_all(handle);

    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = wifi_storage_commit_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "WiFi storage factory reset");
    }

    return err;
}

/*==========================================================
 *
 * SAVE NETWORK CONFIGURATION
 *
 *=========================================================*/

esp_err_t wifi_storage_save_network_config(
    const wifi_network_config_t *config)
{

    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t err =
        wifi_storage_open(
            &handle,
            NVS_READWRITE);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_u8(
        handle,
        WIFI_KEY_MODE,
        config->mode);
    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_u8(
        handle,
        WIFI_KEY_AUTORECONNECT,
        config->auto_reconnect);
    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_u32(
        handle,
        WIFI_KEY_RECONNECT_TIME,
        config->reconnect_interval_ms);
    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_u8(
        handle,
        WIFI_KEY_DHCP,
        config->dhcp);
    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_blob(
        handle,
        WIFI_KEY_IP_INFO,
        &config->ip_info,
        sizeof(esp_netif_ip_info_t));
    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_blob(handle,
                       WIFI_KEY_DNS,
                       &config->dns,
                       sizeof(config->dns));
    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_str(
        handle,
        WIFI_KEY_AP_SSID,
        config->ap_ssid);
    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_str(
        handle,
        WIFI_KEY_AP_PASS,
        config->ap_password);
    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_u8(
        handle,
        WIFI_KEY_AP_CHANNEL,
        config->ap_channel);
    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_u8(
        handle,
        "ap_maxconn",
        config->ap_max_connection);
    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_u8(
        handle,
        "ap_auth",
        config->ap_authmode);
    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    return wifi_storage_commit_close(handle);
}

/*==========================================================
 *
 * LOAD NETWORK CONFIGURATION
 *
 *=========================================================*/

esp_err_t wifi_storage_load_network_config(
    wifi_network_config_t *config)
{

    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config,
           0,
           sizeof(*config));

    nvs_handle_t handle;

    esp_err_t err =
        wifi_storage_open(
            &handle,
            NVS_READONLY);

    if (err != ESP_OK)
    {
        return err;
    }

    uint8_t value;

    if (nvs_get_u8(
            handle,
            WIFI_KEY_MODE,
            &value) == ESP_OK)
    {
        config->mode =
            value;
    }

    if (nvs_get_u8(
            handle,
            WIFI_KEY_AUTORECONNECT,
            &value) == ESP_OK)
    {
        config->auto_reconnect =
            value;
    }

    nvs_get_u32(
        handle,
        WIFI_KEY_RECONNECT_TIME,
        &config->reconnect_interval_ms);

    if (nvs_get_u8(
            handle,
            WIFI_KEY_DHCP,
            &value) == ESP_OK)
    {
        config->dhcp =
            value;
    }

    size_t ip_info_size =
        sizeof(esp_netif_ip_info_t);

    err = nvs_get_blob(
        handle,
        WIFI_KEY_IP_INFO,
        &config->ip_info,
        &ip_info_size);

    if (err != ESP_OK &&
        err != ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(handle);
        return err;
    }

    /* Load DNS configuration */
    size_t dns_size = sizeof(config->dns);
    err = nvs_get_blob(handle, WIFI_KEY_DNS, &config->dns, &dns_size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(handle);
        return err;
    }

    size_t size = sizeof(config->ap_ssid);

    nvs_get_str(
        handle,
        WIFI_KEY_AP_SSID,
        config->ap_ssid,
        &size);

    size = sizeof(config->ap_password);

    nvs_get_str(
        handle,
        WIFI_KEY_AP_PASS,
        config->ap_password,
        &size);

    if (nvs_get_u8(handle, WIFI_KEY_AP_CHANNEL, &value) == ESP_OK)
    {
        config->ap_channel = value;
    }

    if (nvs_get_u8(handle, "ap_maxconn", &value) == ESP_OK)
    {
        config->ap_max_connection = value;
    }

    if (nvs_get_u8(handle, "ap_auth", &value) == ESP_OK)
    {
        config->ap_authmode = value;
    }

    nvs_close(handle);

    return ESP_OK;
}

/*==========================================================
 *
 * RESET NETWORK CONFIGURATION
 *
 *=========================================================*/

static void erase_key_if_exists(nvs_handle_t handle, const char *key)
{
    esp_err_t err = nvs_erase_key(handle, key);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG,
                 "Failed to erase key '%s' (%s)",
                 key,
                 esp_err_to_name(err));
    }
}

esp_err_t wifi_storage_reset_network_config(void)
{
    nvs_handle_t handle;

    esp_err_t err =
        wifi_storage_open(&handle, NVS_READWRITE);

    if (err != ESP_OK)
    {
        return err;
    }

    /*-----------------------------------------------------
     * Remove network configuration keys
     *-----------------------------------------------------*/

    erase_key_if_exists(
        handle,
        WIFI_KEY_MODE);

    erase_key_if_exists(
        handle,
        WIFI_KEY_AUTORECONNECT);

    erase_key_if_exists(
        handle,
        WIFI_KEY_RECONNECT_TIME);

    erase_key_if_exists(
        handle,
        WIFI_KEY_DHCP);

    erase_key_if_exists(
        handle,
        WIFI_KEY_IP_INFO);

    erase_key_if_exists(
        handle,
        WIFI_KEY_DNS);

    erase_key_if_exists(
        handle,
        WIFI_KEY_AP_SSID);

    erase_key_if_exists(
        handle,
        WIFI_KEY_AP_PASS);

    erase_key_if_exists(
        handle,
        WIFI_KEY_AP_CHANNEL);

    erase_key_if_exists(handle, "ap_maxconn");
    erase_key_if_exists(handle, "ap_auth");

    err =
        wifi_storage_commit_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "Network configuration reset");
    }
    else
    {
        ESP_LOGE(TAG,
                 "Failed to reset network configuration (%s)",
                 esp_err_to_name(err));
    }

    return err;
}

/*==========================================================
 *
 * ERASE NETWORK CONFIG
 *
 *=========================================================*/

esp_err_t wifi_storage_erase_network_config(void)
{
    return wifi_storage_reset_network_config();
}