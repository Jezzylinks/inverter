/**
 * @file mdns_service.c
 * @brief mDNS/Bonjour Service for device discovery
 */

#include "mdns_service.h"
#include "firmware_version.h"
#include <string.h>
#include "mdns.h"
#include "esp_log.h"
#include "wifi/wifi_config.h"

static const char *TAG = "MDNS_SERVICE";

static bool s_initialized = false;

/*----------------------------------------------------------
 * Initialize mDNS
 *---------------------------------------------------------*/
esp_err_t mdns_service_init(const char *hostname)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    if (hostname == NULL)
    {
        hostname = WIFI_HOSTNAME;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
        mdns_free();
        return err;
    }

    err = mdns_instance_name_set("Solar Inverter");
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "mDNS instance name set failed: %s", esp_err_to_name(err));
    }

    /* Advertise HTTP service (provisioning portal) */
    err = mdns_service_add(NULL, "_http", "_tcp", WIFI_HTTP_PORT, NULL, 0);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "mDNS HTTP service add failed: %s", esp_err_to_name(err));
    }

    /* Advertise inverter-specific service */
    mdns_txt_item_t inverter_txt[] = {
        {"model", "solar-inverter-v1"},
        {"version", INVERTER_FIRMWARE_VERSION},
        {"vendor", "Jezzylinks"},
    };

    err = mdns_service_add(NULL, "_inverter", "_tcp", 80, inverter_txt, 3);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "mDNS inverter service add failed: %s", esp_err_to_name(err));
    }

    s_initialized = true;
    ESP_LOGI(TAG, "mDNS initialized: %s.local", hostname);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Deinitialize mDNS
 *---------------------------------------------------------*/
esp_err_t mdns_service_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_OK;
    }

    mdns_free();
    s_initialized = false;

    ESP_LOGI(TAG, "mDNS deinitialized");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Update service TXT records
 *---------------------------------------------------------*/
esp_err_t mdns_service_update_status(const char *status, int8_t rssi)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    char rssi_str[8];
    snprintf(rssi_str, sizeof(rssi_str), "%d", rssi);

    mdns_txt_item_t txt[] = {
        {"status", status ? status : "unknown"},
        {"rssi", rssi_str},
    };

    esp_err_t err = mdns_service_txt_set("_inverter", "_tcp", txt, 2);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "mDNS TXT update failed: %s", esp_err_to_name(err));
    }

    return err;
}