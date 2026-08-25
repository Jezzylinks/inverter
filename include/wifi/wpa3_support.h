/**
 * @file wpa3_support.h
 * @brief WPA3/SAE Security Support Interface
 */

#ifndef WPA3_SUPPORT_H
#define WPA3_SUPPORT_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"

    /**
     * @brief WPA3 STA configuration
     */
    typedef struct
    {
        char ssid[33];
        char password[65];
    } wpa3_config_t;

    /**
     * @brief WPA3 AP configuration
     */
    typedef struct
    {
        char ssid[33];
        char password[65];
        uint8_t channel;
        uint8_t max_connection;
    } wpa3_ap_config_t;

    /**
     * @brief Check if WPA3 is supported on this hardware
     * @return true if WPA3-SAE is available
     */
    bool wpa3_is_supported(void);

    /**
     * @brief Configure STA for WPA3-SAE
     * @param config STA configuration
     * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if hardware doesn't support WPA3
     */
    esp_err_t wpa3_configure_sta(const wpa3_config_t *config);

    /**
     * @brief Configure AP for WPA3-SAE Transition Mode
     * @param config AP configuration
     * @return ESP_OK on success
     */
    esp_err_t wpa3_configure_ap_transition(const wpa3_ap_config_t *config);

    /**
     * @brief Get the best available auth mode
     * @return WIFI_AUTH_WPA3_PSK or WIFI_AUTH_WPA2_PSK
     */
    wifi_auth_mode_t wpa3_get_recommended_authmode(void);

    /**
     * @brief Validate password meets WPA3 requirements
     * @param password Password to validate
     * @return true if valid (8-63 chars)
     */
    bool wpa3_validate_password(const char *password);

#ifdef __cplusplus
}
#endif

#endif /* WPA3_SUPPORT_H */