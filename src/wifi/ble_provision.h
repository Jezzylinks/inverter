/**
 * @file ble_provision.h
 * @brief Bluetooth LE Provisioning Interface
 */

#ifndef BLE_PROVISION_H
#define BLE_PROVISION_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_err.h"

    /**
     * @brief Callback invoked when BLE provisioning completes
     */
    typedef void (*ble_provision_complete_callback_t)(void);

    /**
     * @brief Initialize BLE provisioning subsystem
     * @return ESP_OK on success
     */
    esp_err_t ble_provision_init(void);

    /**
     * @brief Start BLE advertising and wait for provisioning
     * @param callback Called when credentials are saved and ready
     * @return ESP_OK on success
     */
    esp_err_t ble_provision_start(ble_provision_complete_callback_t callback);

    /**
     * @brief Stop BLE advertising and disconnect client
     * @return ESP_OK on success
     */
    esp_err_t ble_provision_stop(void);

    /**
     * @brief Deinitialize BLE subsystem
     * @return ESP_OK on success
     */
    esp_err_t ble_provision_deinit(void);

    /**
     * @brief Start/restart BLE advertising
     * @return ESP_OK on success
     */
    esp_err_t ble_provision_start_advertising(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_PROVISION_H */