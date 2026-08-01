#ifndef WIFI_STORAGE_H
#define WIFI_STORAGE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#define WIFI_NVS_NAMESPACE "wifi"

    /*==========================================================
     *
     * STA Credentials
     *
     *=========================================================*/

    typedef struct
    {
        char ssid[33];

        char password[65];

    } wifi_credentials_t;

    /*==========================================================
     *
     * Network Configuration
     *
     *=========================================================*/

    typedef struct
    {
        wifi_mode_t mode;

        bool auto_reconnect;

        uint32_t reconnect_interval_ms;

        bool dhcp;

        esp_netif_ip_info_t ip_info;

        esp_ip4_addr_t dns;

        char ap_ssid[33];

        char ap_password[65];

        uint8_t ap_channel;

    } wifi_network_config_t;

    /*==========================================================
     *
     * Initialization
     *
     *=========================================================*/

    esp_err_t wifi_storage_init(void);

    /*==========================================================
     *
     * Credentials
     *
     *=========================================================*/

    esp_err_t wifi_storage_save_credentials(
        const wifi_credentials_t *credentials);

    esp_err_t wifi_storage_load_credentials(
        wifi_credentials_t *credentials);

    bool wifi_storage_has_credentials(void);

    esp_err_t wifi_storage_erase_credentials(void);

    /*==========================================================
     *
     * Network Config
     *
     *=========================================================*/

    esp_err_t wifi_storage_save_network_config(
        const wifi_network_config_t *config);

    esp_err_t wifi_storage_load_network_config(
        wifi_network_config_t *config);

    esp_err_t wifi_storage_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif