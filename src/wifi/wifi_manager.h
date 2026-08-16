/**
 * @file wifi_manager.h
 * @brief Wi-Fi Manager Public Interface
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "wifi_events.h"
#include "wifi_config.h"

    /*----------------------------------------------------------
     * Wi-Fi Configuration
     *---------------------------------------------------------*/
    typedef struct
    {
        /* STA configuration */
        char ssid[33];
        char password[64]; /* Matches ESP-IDF wifi_sta_config_t */

        /* Operating mode */
        wifi_mode_t mode;

        /* Network options */
        bool dhcp;
        bool auto_reconnect;
        uint32_t reconnect_interval_ms;

        /* Static IP configuration (used when dhcp == false) */
        esp_netif_ip_info_t ip_info;
        esp_ip4_addr_t dns;

        /* Authentication */
        wifi_auth_mode_t authmode;

        /* AP provisioning settings */
        char ap_ssid[33];
        char ap_password[64]; /* Matches ESP-IDF */
        uint8_t ap_channel;
        uint8_t ap_max_connection; /* 1–10, default 4 */
        wifi_auth_mode_t ap_authmode;

    } wifi_manager_config_t;

    /*----------------------------------------------------------
     * Initialization
     *---------------------------------------------------------*/
    esp_err_t wifi_manager_init(void);

    esp_err_t wifi_manager_deinit(void);

    /*----------------------------------------------------------
     * Connection Control
     *---------------------------------------------------------*/
    esp_err_t wifi_manager_start(void);

    esp_err_t wifi_manager_stop(void);

    esp_err_t wifi_manager_connect(void);

    esp_err_t wifi_manager_disconnect(void);

    esp_err_t wifi_manager_reconnect(void);

    /*----------------------------------------------------------
     * Configuration
     *---------------------------------------------------------*/
    esp_err_t wifi_manager_set_config(const wifi_manager_config_t *config);

    esp_err_t wifi_manager_get_config(wifi_manager_config_t *config);

    /*----------------------------------------------------------
     * Status
     *---------------------------------------------------------*/
    bool wifi_manager_is_connected(void);

    wifi_connection_state_t wifi_manager_get_state(void);

    const wifi_status_t *wifi_manager_get_status(void);

    int8_t wifi_manager_get_rssi(void);

    /*----------------------------------------------------------
     * Information
     *---------------------------------------------------------*/
    wifi_mode_t wifi_manager_get_mode(void);

    typedef struct
    {
        uint8_t mac[6];
        uint16_t aid;
    } wifi_ap_client_info_t;

    esp_err_t wifi_manager_get_ap_clients(wifi_ap_client_info_t clients[],
                                           size_t capacity,
                                           size_t *count);

    esp_err_t wifi_manager_disconnect_ap_client(const uint8_t mac[6]);

    esp_err_t wifi_manager_get_mac(uint8_t mac[6]);

    esp_err_t wifi_manager_get_ip(esp_netif_ip_info_t *ip);

    esp_err_t wifi_manager_get_ap_info(wifi_ap_record_t *ap);

    /*----------------------------------------------------------
     * Retry Control
     *---------------------------------------------------------*/
    void wifi_manager_set_retry_limit(uint8_t retry);

    uint8_t wifi_manager_get_retry_limit(void);

    /*----------------------------------------------------------
     * Auto Reconnect
     *---------------------------------------------------------*/
    void wifi_manager_enable_auto_reconnect(bool enable);

    bool wifi_manager_auto_reconnect_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */