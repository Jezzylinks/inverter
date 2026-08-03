/**
 * @file reconnect_backoff.h
 * @brief Exponential Backoff Reconnect Interface
 */

#ifndef RECONNECT_BACKOFF_H
#define RECONNECT_BACKOFF_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

    /**
     * @brief Callback type for reconnect trigger
     * @param attempt Current reconnection attempt number
     */
    typedef void (*reconnect_callback_t)(uint32_t attempt);

    /**
     * @brief Initialize exponential backoff system
     * @return ESP_OK on success
     */
    esp_err_t reconnect_backoff_init(void);

    /**
     * @brief Deinitialize exponential backoff
     * @return ESP_OK on success
     */
    esp_err_t reconnect_backoff_deinit(void);

    /**
     * @brief Start exponential backoff timer
     * @param callback Function to call when timer expires
     * @return ESP_OK on success
     */
    esp_err_t reconnect_backoff_start(reconnect_callback_t callback);

    /**
     * @brief Stop backoff timer
     * @return ESP_OK on success
     */
    esp_err_t reconnect_backoff_stop(void);

    /**
     * @brief Reset attempt counter (call after successful connection)
     * @return ESP_OK on success
     */
    esp_err_t reconnect_backoff_reset(void);

    /**
     * @brief Get current attempt count
     * @return Number of reconnection attempts since last reset
     */
    uint32_t reconnect_backoff_get_attempts(void);

    /**
     * @brief Check if backoff timer is currently active
     * @return true if waiting to reconnect
     */
    bool reconnect_backoff_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* RECONNECT_BACKOFF_H */