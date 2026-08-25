/**
 * @file mdns_service.h
 * @brief mDNS/Bonjour Service Interface
 */

#ifndef MDNS_SERVICE_H
#define MDNS_SERVICE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_err.h"

    /**
     * @brief Initialize mDNS service
     * @param hostname Device hostname (e.g., "solar-inverter"). NULL uses WIFI_HOSTNAME.
     * @return ESP_OK on success
     */
    esp_err_t mdns_service_init(const char *hostname);

    /**
     * @brief Deinitialize mDNS service
     * @return ESP_OK on success
     */
    esp_err_t mdns_service_deinit(void);

    /**
     * @brief Update mDNS TXT records with current status
     * @param status Connection status string
     * @param rssi Signal strength in dBm
     * @return ESP_OK on success
     */
    esp_err_t mdns_service_update_status(const char *status, int8_t rssi);

#ifdef __cplusplus
}
#endif

#endif /* MDNS_SERVICE_H */