#include "wifi_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_storage.h"
#include "wifi_events.h"
#include "wifi_provision.h"
#include "wifi_config.h"

#define WIFI_RECONNECT_DELAY_MS 3000
static const char *TAG = "wifi_manager";

const wifi_status_t *wifi_events_get_status(void);

/*----------------------------------------------------------
 * Static Data
 *---------------------------------------------------------*/

static esp_netif_t *s_sta_netif = NULL;

static esp_netif_t *s_ap_netif = NULL;

static wifi_manager_config_t s_config;

static bool s_initialized = false;

static uint8_t s_retry_limit = WIFI_MAX_RETRY_COUNT;

static bool s_auto_reconnect = true;
static wifi_credentials_t s_credentials;

/*----------------------------------------------------------
 * Configure Static IP
 *---------------------------------------------------------*/
static esp_err_t wifi_manager_set_static_ip(void)
{
    wifi_network_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));

    esp_err_t err =
        wifi_storage_load_network_config(&cfg);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Failed to load network configuration");

        return err;
    }

    /*
     * DHCP enabled?
     */
    if (cfg.dhcp)
    {
        ESP_LOGI(TAG,
                 "DHCP enabled");

        return ESP_OK;
    }

    /*
     * Stop DHCP client
     */
    ESP_ERROR_CHECK(
        esp_netif_dhcpc_stop(s_sta_netif));

    /*
     * Apply static IP
     */
    ESP_ERROR_CHECK(
        esp_netif_set_ip_info(
            s_sta_netif,
            &cfg.ip_info));

    wifi_dns_set_server(
        s_sta_netif,
        cfg.dns);

    ESP_LOGI(TAG,
             "Static IP configured");

    ESP_LOGI(TAG,
             "IP: " IPSTR,
             IP2STR(&cfg.ip_info.ip));

    ESP_LOGI(TAG,
             "Gateway: " IPSTR,
             IP2STR(&cfg.ip_info.gw));

    ESP_LOGI(TAG,
             "Netmask: " IPSTR,
             IP2STR(&cfg.ip_info.netmask));

    return ESP_OK;
}

/*----------------------------------------------------------
 * Initialize Wi-Fi Manager
 *---------------------------------------------------------*/

static void wifi_manager_provision_complete(void)
{
    ESP_LOGI(TAG,
             "Provisioning completed");

    /*
     * Load the newly saved credentials
     */
    wifi_credentials_t credentials;

    if (wifi_storage_load_credentials(&credentials) != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Failed loading credentials");

        return;
    }

    /*
     * Restart WiFi in STA mode
     */
    wifi_manager_connect();
}

/*----------------------------------------------------------
 * Wi-Fi Event State Update Callback
 *---------------------------------------------------------*/

static void wifi_manager_event_update(
    wifi_connection_state_t state)
{
    if (!s_initialized)
    {
        return;
    }

    switch (state)
    {
    case WIFI_STATE_CONNECTING:

        ESP_LOGI(TAG,
                 "WiFi connecting");

        break;

    case WIFI_STATE_CONNECTED:

        ESP_LOGI(TAG,
                 "WiFi connected");

        break;

    case WIFI_STATE_DISCONNECTED:

        ESP_LOGW(TAG,
                 "WiFi disconnected");

        break;

    case WIFI_STATE_FAILED:

        ESP_LOGE(TAG,
                 "WiFi failed");

        break;

    case WIFI_STATE_IDLE:

        ESP_LOGI(TAG,
                 "WiFi idle");

        break;

    default:
        break;
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    wifi_credentials_t credentials;

    memset(&credentials, 0, sizeof(credentials));

    memset(&s_credentials, 0, sizeof(s_credentials));

    wifi_provision_register_complete_callback(
        wifi_manager_provision_complete);

    if (wifi_storage_has_credentials())
    {
        ESP_ERROR_CHECK(
            wifi_storage_load_credentials(&s_credentials));

        strncpy(s_config.ssid,
                s_credentials.ssid,
                sizeof(s_config.ssid) - 1);

        strncpy(s_config.password,
                s_credentials.password,
                sizeof(s_config.password) - 1);
    }
    else
    {
        ESP_LOGW(TAG, "No WiFi credentials stored");
    }

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif =
        esp_netif_create_default_wifi_sta();

    if (s_sta_netif == NULL)
    {
        ESP_LOGE(TAG,
                 "STA netif failed");

        return ESP_FAIL;
    }

    esp_netif_set_hostname(
        s_sta_netif,
        WIFI_HOSTNAME);

    s_ap_netif =
        esp_netif_create_default_wifi_ap();

    if (s_ap_netif == NULL)
    {
        ESP_LOGE(TAG,
                 "AP netif failed");

        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(wifi_events_init());
    ESP_ERROR_CHECK(wifi_events_register_callback(wifi_manager_event_update));

    s_initialized = true;

    ESP_LOGI(TAG, "Wi-Fi manager initialized");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Deinitialize
 *---------------------------------------------------------*/

esp_err_t wifi_manager_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_OK;
    }

    esp_wifi_stop();

    esp_wifi_deinit();

    wifi_events_deinit();

    /*----------------------------------------------------------
     * Destroy WiFi Interfaces
     *---------------------------------------------------------*/

    if (s_sta_netif != NULL)
    {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }

    if (s_ap_netif != NULL)
    {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }

    memset(&s_config, 0, sizeof(s_config));

    s_initialized = false;

    ESP_LOGI(TAG, "Wi-Fi manager deinitialized");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Start Wi-Fi
 *---------------------------------------------------------*/

esp_err_t wifi_manager_start(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_ERROR_CHECK(
        wifi_manager_configure_apsta());

    ESP_ERROR_CHECK(
        wifi_manager_set_static_ip());

    ESP_ERROR_CHECK(
        esp_wifi_start());

    ESP_LOGI(TAG,
             "WiFi AP+STA started");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Stop Wi-Fi
 *---------------------------------------------------------*/

esp_err_t wifi_manager_stop(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err =
        esp_wifi_stop();

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "WiFi stopped");
    }

    return err;
}

/*----------------------------------------------------------
 * Connect
 *---------------------------------------------------------*/

esp_err_t wifi_manager_connect(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Always reload latest credentials
     */
    esp_err_t err =
        wifi_storage_load_credentials(
            &s_credentials);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "No WiFi credentials");

        return err;
    }

    wifi_config_t wifi_config;

    memset(&wifi_config,
           0,
           sizeof(wifi_config));

    strncpy((char *)wifi_config.sta.ssid,
            s_credentials.ssid,
            sizeof(wifi_config.sta.ssid));

    strncpy((char *)wifi_config.sta.password,
            s_credentials.password,
            sizeof(wifi_config.sta.password));

    wifi_config.sta.threshold.authmode =
        WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_STA));

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config));

    ESP_ERROR_CHECK(
        esp_wifi_start());

    wifi_manager_set_static_ip();

    err =
        esp_wifi_connect();

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "Connecting to %s",
                 s_credentials.ssid);
    }

    return err;
}

bool wifi_manager_is_connected(void)
{
    const wifi_status_t *status =
        wifi_events_get_status();

    return status->connected;
}

/*----------------------------------------------------------
 * Disconnect
 *---------------------------------------------------------*/

esp_err_t wifi_manager_disconnect(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err =
        esp_wifi_disconnect();

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "WiFi disconnected");
    }

    return err;
}

/*----------------------------------------------------------
 * Reconnect
 *---------------------------------------------------------*/

esp_err_t wifi_manager_reconnect(void)
{
    esp_err_t err;

    err =
        wifi_manager_disconnect();

    if (err != ESP_OK &&
        err != ESP_ERR_WIFI_NOT_CONNECT)
    {
        return err;
    }

    vTaskDelay(
        pdMS_TO_TICKS(
            WIFI_RECONNECT_DELAY_MS));

    return wifi_manager_connect();
}

/*----------------------------------------------------------
 * Set Wi-Fi Configuration
 *---------------------------------------------------------*/

esp_err_t wifi_manager_set_config(
    const wifi_manager_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config,
           config,
           sizeof(wifi_manager_config_t));

    ESP_LOGI(TAG,
             "WiFi configuration updated");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Get Wi-Fi Configuration
 *---------------------------------------------------------*/

esp_err_t wifi_manager_get_config(
    wifi_manager_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(config,
           &s_config,
           sizeof(wifi_manager_config_t));

    return ESP_OK;
}

/*----------------------------------------------------------
 * Connection Status
 *---------------------------------------------------------*/

bool wifi_manager_is_connected(void)
{
    const wifi_status_t *status =
        wifi_events_get_status();

    if (status == NULL)
    {
        return false;
    }

    return status->connected;
}

/*----------------------------------------------------------
 * Get Wi-Fi State
 *---------------------------------------------------------*/

wifi_connection_state_t
wifi_manager_get_state(void)
{
    const wifi_status_t *status =
        wifi_events_get_status();

    return status->state;
}

/*----------------------------------------------------------
 * Get Wi-Fi Status
 *---------------------------------------------------------*/

const wifi_status_t *
wifi_manager_get_status(void)
{
    return wifi_events_get_status();
}

/*----------------------------------------------------------
 * Get RSSI
 *---------------------------------------------------------*/

int8_t wifi_manager_get_rssi(void)
{
    wifi_ap_record_t ap_info;

    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        return ap_info.rssi;
    }

    return -127;
}

/*----------------------------------------------------------
 * Get MAC Address
 *---------------------------------------------------------*/

esp_err_t wifi_manager_get_mac(
    uint8_t mac[6])
{
    if (mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_wifi_get_mac(
        WIFI_IF_STA,
        mac);
}

/*----------------------------------------------------------
 * Get IP Address
 *---------------------------------------------------------*/

esp_err_t wifi_manager_get_ip(
    esp_netif_ip_info_t *ip)
{
    if (ip == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_sta_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_netif_get_ip_info(
        s_sta_netif,
        ip);
}

/*----------------------------------------------------------
 * Get AP Information
 *---------------------------------------------------------*/

esp_err_t wifi_manager_get_ap_info(
    wifi_ap_record_t *ap)
{
    if (ap == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_wifi_sta_get_ap_info(ap);
}

/*----------------------------------------------------------
 * Retry Limit
 *---------------------------------------------------------*/

void wifi_manager_set_retry_limit(
    uint8_t retry)
{
    s_retry_limit = retry;
}

uint8_t wifi_manager_get_retry_limit(void)
{
    return s_retry_limit;
}

/*----------------------------------------------------------
 * Auto Reconnect
 *---------------------------------------------------------*/

void wifi_manager_enable_auto_reconnect(
    bool enable)
{
    s_auto_reconnect = enable;

    s_config.auto_reconnect = enable;
}

bool wifi_manager_auto_reconnect_enabled(void)
{
    return s_auto_reconnect;
}

static esp_err_t wifi_manager_configure_apsta(void)
{
    wifi_config_t sta_config = {0};

    wifi_config_t ap_config = {0};

    strncpy(
        (char *)sta_config.sta.ssid,
        s_config.ssid,
        sizeof(sta_config.sta.ssid));

    strncpy(
        (char *)sta_config.sta.password,
        s_config.password,
        sizeof(sta_config.sta.password));

    strncpy(
        (char *)ap_config.ap.ssid,
        s_config.ap_ssid,
        sizeof(ap_config.ap.ssid));

    strncpy(
        (char *)ap_config.ap.password,
        s_config.ap_password,
        sizeof(ap_config.ap.password));

    ap_config.ap.channel =
        s_config.ap_channel;

    ap_config.ap.max_connection =
        4;

    ap_config.ap.authmode =
        WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_APSTA));

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &sta_config));

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_AP,
            &ap_config));

    return ESP_OK;
}