/**
 * @file wifi_storage.h
 * @brief Wi-Fi NVS Storage Interface
 */

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
#include "wifi_config.h"

/* Use the same namespace as defined in wifi_config.h */
#define WIFI_NVS_NAMESPACE "wifi"

    /*==========================================================
     * Network Configuration
     *========================================================*/
    typedef struct
    {
        wifi_mode_t mode;
        bool auto_reconnect;
        uint32_t reconnect_interval_ms;
        bool dhcp;
        esp_netif_ip_info_t ip_info;
        esp_ip4_addr_t dns;
        char ap_ssid[33];
        char ap_password[64]; /* Matched to credentials size */
        uint8_t ap_channel;
        uint8_t ap_max_connection;    /* 1–10, default 4 */
        wifi_auth_mode_t ap_authmode; /* WPA2, WPA3, etc. */

    } wifi_network_config_t;

    /*==========================================================
     * Initialization
     *========================================================*/
    /** Populate a safe default AP/STA network configuration. */
    void wifi_storage_set_default_network_config(wifi_network_config_t *config);

    esp_err_t wifi_storage_init(void);

    esp_err_t wifi_storage_deinit(void);

    /*==========================================================
     * Credentials
     *========================================================*/
    esp_err_t wifi_storage_save_credentials(const wifi_credentials_t *credentials);

    esp_err_t wifi_storage_load_credentials(wifi_credentials_t *credentials);

    /**
     * @brief Check if credentials exist in NVS.
     * @param[out] has_creds Set to true if credentials exist, false otherwise.
     * @return true on success, false if NVS read failed.
     */
    bool wifi_storage_has_credentials(void);

    esp_err_t wifi_storage_erase_credentials(void);

    /*==========================================================
     * Network Config
     *========================================================*/
    esp_err_t wifi_storage_save_network_config(const wifi_network_config_t *config);

    esp_err_t wifi_storage_load_network_config(wifi_network_config_t *config);

    esp_err_t wifi_storage_erase_network_config(void);

    /**
     * @brief Erase all Wi-Fi related NVS data (credentials + network config).
     * @return ESP_OK on success.
     */
    esp_err_t wifi_storage_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_STORAGE_H */