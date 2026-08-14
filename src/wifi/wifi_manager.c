/**
 * @file wifi_manager.c
 * @brief Wi-Fi Manager Implementation
 */

#include "wifi_manager.h"
#include <string.h>
#include <stdlib.h>

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

static const char *TAG = "wifi_manager";

/*----------------------------------------------------------
 * Static Data
 *---------------------------------------------------------*/

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static wifi_manager_config_t s_config;
static bool s_initialized = false;
uint8_t s_retry_limit = WIFI_MAXIMUM_RETRY;
bool s_auto_reconnect = true;
static wifi_credentials_t s_credentials;
static SemaphoreHandle_t s_manager_mutex = NULL;

/*----------------------------------------------------------
 * Forward Declarations
 *---------------------------------------------------------*/
static esp_err_t wifi_manager_configure_apsta(void);
static esp_err_t wifi_manager_set_static_ip(void);
static bool wifi_manager_config_valid(const wifi_manager_config_t *config);
static void wifi_manager_load_network_config(void);

/*----------------------------------------------------------
 * Configure Static IP
 *---------------------------------------------------------*/
static esp_err_t wifi_manager_set_static_ip(void)
{
    if (s_sta_netif == NULL)
    {
        ESP_LOGE(TAG, "STA netif not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_network_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    esp_err_t err = wifi_storage_load_network_config(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to load network config, using DHCP");
        return ESP_OK; /* Default to DHCP if no config stored */
    }

    if (cfg.dhcp)
    {
        err = esp_netif_dhcpc_start(s_sta_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            return err;
        }
        return ESP_OK;
    }

    /* Stop DHCP client */
    err = esp_netif_dhcpc_stop(s_sta_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    {
        ESP_LOGW(TAG, "Failed to stop DHCP client: %s", esp_err_to_name(err));
        return err;
    }

    /* Apply static IP */
    err = esp_netif_set_ip_info(s_sta_netif, &cfg.ip_info);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set static IP");
        return err;
    }

    /* Set DNS server if provided */
    if (cfg.dns.addr != 0)
    {
        esp_netif_dns_info_t dns_info = {0};
        dns_info.ip.u_addr.ip4.addr = cfg.dns.addr;
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    }

    ESP_LOGI(TAG, "Static IP configured: " IPSTR, IP2STR(&cfg.ip_info.ip));
    ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&cfg.ip_info.gw));
    ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&cfg.ip_info.netmask));

    return ESP_OK;
}

/*----------------------------------------------------------
 * Load Network Config from Storage into s_config
 *---------------------------------------------------------*/
static void wifi_manager_load_network_config(void)
{
    wifi_network_config_t net_cfg;
    memset(&net_cfg, 0, sizeof(net_cfg));

    if (wifi_storage_load_network_config(&net_cfg) != ESP_OK)
    {
        ESP_LOGW(TAG, "No network config in storage, using safe defaults");
        wifi_storage_set_default_network_config(&net_cfg);
    }

    s_config.mode = net_cfg.mode;
    s_config.authmode = INVERTER_WIFI_AUTH_MODE;
    s_config.dhcp = net_cfg.dhcp;
    s_config.auto_reconnect = net_cfg.auto_reconnect;
    s_config.reconnect_interval_ms = net_cfg.reconnect_interval_ms;
    s_config.ip_info = net_cfg.ip_info;
    s_config.dns = net_cfg.dns;
    s_config.ap_max_connection = net_cfg.ap_max_connection;
    s_config.ap_authmode = net_cfg.ap_authmode;

    /* AP SSID/password from storage or fall back to provision defaults */
    if (net_cfg.ap_ssid[0] != '\0')
    {
        strncpy(s_config.ap_ssid, net_cfg.ap_ssid, sizeof(s_config.ap_ssid) - 1);
        s_config.ap_ssid[sizeof(s_config.ap_ssid) - 1] = '\0';
        strncpy(s_config.ap_password, net_cfg.ap_password, sizeof(s_config.ap_password) - 1);
        s_config.ap_password[sizeof(s_config.ap_password) - 1] = '\0';
    }
    else
    {
        strncpy(s_config.ap_ssid, WIFI_PROVISION_AP_SSID, sizeof(s_config.ap_ssid) - 1);
        s_config.ap_ssid[sizeof(s_config.ap_ssid) - 1] = '\0';
        strncpy(s_config.ap_password, WIFI_PROVISION_AP_PASSWORD, sizeof(s_config.ap_password) - 1);
        s_config.ap_password[sizeof(s_config.ap_password) - 1] = '\0';
    }

    if (net_cfg.ap_channel != 0)
    {
        s_config.ap_channel = net_cfg.ap_channel;
    }
    else
    {
        s_config.ap_channel = WIFI_PROVISION_CHANNEL;
    }
}

/*----------------------------------------------------------
 * Wi-Fi Event State Update Callback
 *---------------------------------------------------------*/
static void wifi_manager_event_update(wifi_connection_state_t state)
{
    if (!s_initialized)
    {
        return;
    }

    switch (state)
    {
    case WIFI_STATE_CONNECTING:
        ESP_LOGI(TAG, "WiFi connecting");
        break;
    case WIFI_STATE_CONNECTED:
        ESP_LOGI(TAG, "WiFi connected");
        break;
    case WIFI_STATE_DISCONNECTED:
        ESP_LOGW(TAG, "WiFi disconnected");
        break;
    case WIFI_STATE_FAILED:
        ESP_LOGE(TAG, "WiFi failed");
        break;
    case WIFI_STATE_IDLE:
        ESP_LOGI(TAG, "WiFi idle");
        break;
    default:
        break;
    }
}

/*----------------------------------------------------------
 * Validate Wi-Fi Configuration (pure validation, no modification)
 *---------------------------------------------------------*/
static bool wifi_manager_config_valid(const wifi_manager_config_t *config)
{
    if (!config) {
        return false;
    }
    if (strnlen(config->ssid, sizeof(config->ssid)) >= sizeof(config->ssid) ||
        strnlen(config->password, sizeof(config->password)) >= sizeof(config->password) ||
        strnlen(config->ap_ssid, sizeof(config->ap_ssid)) >= sizeof(config->ap_ssid) ||
        strnlen(config->ap_password, sizeof(config->ap_password)) >= sizeof(config->ap_password)) {
        ESP_LOGE(TAG, "WiFi configuration is not NUL terminated");
        return false;
    }
    if (config->ssid[0] == '\0' && config->ap_ssid[0] == '\0') {
        ESP_LOGW(TAG, "Both STA and AP SSIDs are empty");
        return false;
    }
    if (config->ssid[0] != '\0' && strlen(config->password) > 0U && strlen(config->password) < 8U) {
        ESP_LOGW(TAG, "STA password is shorter than WPA minimum");
        return false;
    }
    if (config->ap_ssid[0] != '\0' && strlen(config->ap_password) < 8U) {
        ESP_LOGW(TAG, "Provisioning AP password is too short");
        return false;
    }
    return true;
}

/*----------------------------------------------------------
 * Initialize Wi-Fi Manager
 *---------------------------------------------------------*/
esp_err_t wifi_manager_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    s_manager_mutex = xSemaphoreCreateMutex();
    if (s_manager_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err;

    memset(&s_config, 0, sizeof(s_config));
    memset(&s_credentials, 0, sizeof(s_credentials));

    /* Load network config first (sets defaults if none stored) */
    wifi_manager_load_network_config();

    if (wifi_storage_has_credentials())
    {
        err = wifi_storage_load_credentials(&s_credentials);
        if (err == ESP_OK)
        {
            strncpy(s_config.ssid, s_credentials.ssid, sizeof(s_config.ssid) - 1);
            s_config.ssid[sizeof(s_config.ssid) - 1] = '\0';
            strncpy(s_config.password, s_credentials.password, sizeof(s_config.password) - 1);
            s_config.password[sizeof(s_config.password) - 1] = '\0';
        }
        else
        {
            ESP_LOGW(TAG, "Failed to load stored credentials: %s", esp_err_to_name(err));
        }
    }
    else
    {
        ESP_LOGW(TAG, "No WiFi credentials stored");
    }

    err = esp_netif_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Netif init failed: %s", esp_err_to_name(err));
        goto rollback_mutex;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Event loop creation failed: %s", esp_err_to_name(err));
        goto rollback_mutex;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL)
    {
        ESP_LOGE(TAG, "STA netif creation failed");
        err = ESP_FAIL;
        goto rollback_netif;
    }

    esp_netif_set_hostname(s_sta_netif, WIFI_HOSTNAME);

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL)
    {
        ESP_LOGE(TAG, "AP netif creation failed");
        err = ESP_FAIL;
        goto rollback_sta;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    err = esp_wifi_init(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        goto rollback_ap;
    }

    /* Apply power save mode from config */
    esp_wifi_set_ps(WIFI_POWER_SAVE_MODE);

    err = wifi_events_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Events init failed: %s", esp_err_to_name(err));
        goto rollback_wifi;
    }

    wifi_events_set_retry_policy(s_config.auto_reconnect, s_retry_limit);

    err = wifi_events_register_event_callback(wifi_manager_event_update);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Events callback registration failed: %s", esp_err_to_name(err));
        goto rollback_events;
    }

    s_initialized = true;

    ESP_LOGI(TAG, "Wi-Fi manager initialized");

    return ESP_OK;

rollback_events:
    wifi_events_deinit();
rollback_wifi:
    esp_wifi_deinit();
rollback_ap:
    esp_netif_destroy(s_ap_netif);
    s_ap_netif = NULL;
rollback_sta:
    esp_netif_destroy(s_sta_netif);
    s_sta_netif = NULL;
rollback_netif:
    /* Can't undo esp_netif_init or event_loop */
rollback_mutex:
    if (s_manager_mutex)
    {
        vSemaphoreDelete(s_manager_mutex);
        s_manager_mutex = NULL;
    }
    return err;
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

    esp_err_t err;
    esp_err_t first_err = ESP_OK;

    wifi_events_unregister_event_callback(wifi_manager_event_update);

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT)
    {
        ESP_LOGW(TAG, "WiFi stop failed: %s", esp_err_to_name(err));
        if (first_err == ESP_OK)
            first_err = err;
    }

    err = esp_wifi_deinit();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "WiFi deinit failed: %s", esp_err_to_name(err));
        if (first_err == ESP_OK)
            first_err = err;
    }

    wifi_events_deinit();

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
    memset(&s_credentials, 0, sizeof(s_credentials));

    s_retry_limit = WIFI_MAXIMUM_RETRY;
    s_auto_reconnect = true;

    if (s_manager_mutex)
    {
        vSemaphoreDelete(s_manager_mutex);
        s_manager_mutex = NULL;
    }

    s_initialized = false;

    ESP_LOGI(TAG, "Wi-Fi manager deinitialized");

    return first_err;
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

    if (!wifi_manager_config_valid(&s_config))
    {
        ESP_LOGE(TAG, "Invalid WiFi configuration");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = wifi_manager_configure_apsta();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure APSTA: %s", esp_err_to_name(err));
        return err;
    }

    err = wifi_manager_set_static_ip();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Static IP config failed: %s", esp_err_to_name(err));
    }

    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "WiFi started (mode: %s)",
             (s_config.mode == WIFI_MODE_STA) ? "STA" : (s_config.mode == WIFI_MODE_AP)  ? "AP"
                                                    : (s_config.mode == WIFI_MODE_APSTA) ? "AP+STA"
                                                                                         : "UNKNOWN");

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

    esp_err_t err = esp_wifi_stop();
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "WiFi stopped");
    }
    else
    {
        ESP_LOGW(TAG, "WiFi stop failed: %s", esp_err_to_name(err));
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

    /* Always reload latest credentials */
    esp_err_t err = wifi_storage_load_credentials(&s_credentials);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "No WiFi credentials available");
        return err;
    }

    if (s_credentials.ssid[0] == '\0')
    {
        ESP_LOGE(TAG, "SSID is empty");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));

    strncpy((char *)wifi_config.sta.ssid, s_credentials.ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, s_credentials.password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = s_config.authmode;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Set mode STA failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Set STA config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* WiFi is already started by wifi_manager_start() or previous connect */
    /* Only start if not already running */
    wifi_mode_t current_mode;
    if (esp_wifi_get_mode(&current_mode) != ESP_OK || current_mode == WIFI_MODE_NULL)
    {
        err = esp_wifi_start();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    err = wifi_manager_set_static_ip();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Static IP config failed: %s", esp_err_to_name(err));
    }

    err = esp_wifi_connect();
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Connecting to %s", s_credentials.ssid);
    }
    else
    {
        ESP_LOGE(TAG, "Connect failed: %s", esp_err_to_name(err));
    }

    return err;
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

    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "WiFi disconnected");
    }
    else if (err != ESP_ERR_WIFI_NOT_CONNECT)
    {
        ESP_LOGW(TAG, "Disconnect failed: %s", esp_err_to_name(err));
    }

    return err;
}

/*----------------------------------------------------------
 * Reconnect
 *---------------------------------------------------------*/
esp_err_t wifi_manager_reconnect(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = wifi_manager_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT)
    {
        ESP_LOGW(TAG, "Disconnect before reconnect failed: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_DELAY_MS));

    return wifi_manager_connect();
}

/*----------------------------------------------------------
 * Set Wi-Fi Configuration (with local sanitization)
 *---------------------------------------------------------*/
esp_err_t wifi_manager_set_config(const wifi_manager_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* Local copy to safely sanitize */
    wifi_manager_config_t temp;
    memcpy(&temp, config, sizeof(temp));

    /* Force null termination on local copy */
    temp.ssid[sizeof(temp.ssid) - 1] = '\0';
    temp.password[sizeof(temp.password) - 1] = '\0';
    temp.ap_ssid[sizeof(temp.ap_ssid) - 1] = '\0';
    temp.ap_password[sizeof(temp.ap_password) - 1] = '\0';

    if (!wifi_manager_config_valid(&temp))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_manager_mutex)
    {
        xSemaphoreTake(s_manager_mutex, portMAX_DELAY);
    }

    memcpy(&s_config, &temp, sizeof(s_config));
    wifi_events_set_retry_policy(s_config.auto_reconnect, s_retry_limit);

    if (s_manager_mutex)
    {
        xSemaphoreGive(s_manager_mutex);
    }

    ESP_LOGI(TAG, "WiFi configuration updated");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Get Wi-Fi Configuration
 *---------------------------------------------------------*/
esp_err_t wifi_manager_get_config(wifi_manager_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_manager_mutex)
    {
        xSemaphoreTake(s_manager_mutex, portMAX_DELAY);
    }

    memcpy(config, &s_config, sizeof(wifi_manager_config_t));

    if (s_manager_mutex)
    {
        xSemaphoreGive(s_manager_mutex);
    }

    return ESP_OK;
}

/*----------------------------------------------------------
 * Connection Status
 *---------------------------------------------------------*/
bool wifi_manager_is_connected(void)
{
    const wifi_status_t *status = wifi_events_get_status();

    if (status == NULL)
    {
        return false;
    }

    return status->connected;
}

/*----------------------------------------------------------
 * Get Wi-Fi State
 *---------------------------------------------------------*/
wifi_connection_state_t wifi_manager_get_state(void)
{
    const wifi_status_t *status = wifi_events_get_status();

    if (status == NULL)
    {
        return WIFI_STATE_IDLE;
    }

    return status->state;
}

/*----------------------------------------------------------
 * Get Wi-Fi Status
 *---------------------------------------------------------*/
const wifi_status_t *wifi_manager_get_status(void)
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
esp_err_t wifi_manager_get_mac(uint8_t mac[6])
{
    if (mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_wifi_get_mac(WIFI_IF_STA, mac);
}

/*----------------------------------------------------------
 * Get IP Address
 *---------------------------------------------------------*/
esp_err_t wifi_manager_get_ip(esp_netif_ip_info_t *ip)
{
    if (ip == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_sta_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_netif_get_ip_info(s_sta_netif, ip);
}

/*----------------------------------------------------------
 * Get AP Information
 *---------------------------------------------------------*/
esp_err_t wifi_manager_get_ap_info(wifi_ap_record_t *ap)
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
void wifi_manager_set_retry_limit(uint8_t retry)
{
    s_retry_limit = retry;
    wifi_events_set_retry_policy(s_auto_reconnect, s_retry_limit);
}

uint8_t wifi_manager_get_retry_limit(void)
{
    return s_retry_limit;
}

/*----------------------------------------------------------
 * Auto Reconnect
 *---------------------------------------------------------*/
void wifi_manager_enable_auto_reconnect(bool enable)
{
    s_auto_reconnect = enable;

    if (s_manager_mutex)
    {
        xSemaphoreTake(s_manager_mutex, portMAX_DELAY);
    }
    s_config.auto_reconnect = enable;
    if (s_manager_mutex)
    {
        xSemaphoreGive(s_manager_mutex);
    }
    wifi_events_set_retry_policy(enable, s_retry_limit);
}

bool wifi_manager_auto_reconnect_enabled(void)
{
    return s_auto_reconnect;
}

/*----------------------------------------------------------
 * Internal: Configure AP+STA
 *---------------------------------------------------------*/
static esp_err_t wifi_manager_configure_apsta(void)
{
    wifi_config_t sta_config = {0};
    wifi_config_t ap_config = {0};

    if (s_config.ssid[0] != '\0')
    {
        strncpy((char *)sta_config.sta.ssid, s_config.ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, s_config.password, sizeof(sta_config.sta.password) - 1);
        sta_config.sta.threshold.authmode = s_config.authmode;
    }

    if (s_config.ap_ssid[0] != '\0')
    {
        strncpy((char *)ap_config.ap.ssid, s_config.ap_ssid, sizeof(ap_config.ap.ssid) - 1);
        strncpy((char *)ap_config.ap.password, s_config.ap_password, sizeof(ap_config.ap.password) - 1);
        ap_config.ap.channel = s_config.ap_channel;
        ap_config.ap.max_connection = s_config.ap_max_connection;
        ap_config.ap.authmode = s_config.ap_authmode;
    }

    /* Determine mode based on what's configured */
    wifi_mode_t mode = s_config.mode;
    if (mode == WIFI_MODE_NULL)
    {
        bool has_sta = (sta_config.sta.ssid[0] != '\0');
        bool has_ap = (ap_config.ap.ssid[0] != '\0');
        if (has_sta && has_ap)
        {
            mode = WIFI_MODE_APSTA;
        }
        else if (has_sta)
        {
            mode = WIFI_MODE_STA;
        }
        else if (has_ap)
        {
            mode = WIFI_MODE_AP;
        }
        else
        {
            ESP_LOGE(TAG, "No STA or AP configuration provided");
            return ESP_ERR_INVALID_ARG;
        }
    }

    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Set WiFi mode failed: %s", esp_err_to_name(err));
        return err;
    }

    if (sta_config.sta.ssid[0] != '\0')
    {
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Set STA config failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (ap_config.ap.ssid[0] != '\0')
    {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Set AP config failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}