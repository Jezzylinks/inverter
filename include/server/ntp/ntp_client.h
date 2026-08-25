/**
 * @file ntp_client.h
 * @brief NTP Client Interface
 */

#ifndef NTP_CLIENT_H
#define NTP_CLIENT_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <time.h>
#include <sys/time.h>
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "stdbool.h"

#define NTP_DEFAULT_SERVER "pool.ntp.org"
#define NTP_SYNC_INTERVAL_MS 3600000  /* 1 hour */
#define NTP_MIN_VALID_TIME 1700000000 /* Dec 2023 */

    /**
     * @brief Initialize NTP client
     * @param server NTP server hostname. NULL uses default (pool.ntp.org).
     * @return ESP_OK on success
     */
    esp_err_t ntp_client_init(const char *server);

    /**
     * @brief Deinitialize NTP client
     * @return ESP_OK on success
     */
    esp_err_t ntp_client_deinit(void);

    /**
     * @brief Force an immediate NTP sync
     * @return ESP_OK on success
     */
    esp_err_t ntp_client_sync_now(void);

    /**
     * @brief Check if system time has been set by NTP
     * @return true if time is valid
     */
    bool ntp_client_time_is_set(void);

    /**
     * @brief Get current time as formatted string
     * @param buffer Output buffer
     * @param len Buffer size
     * @return ESP_OK on success
     */
    esp_err_t ntp_client_get_time_string(char *buffer, size_t len);

    /**
     * @brief Callback type for NTP sync events
     */
    typedef void (*ntp_sync_callback_t)(struct timeval *tv);

    /**
     * @brief Register callback for NTP sync events
     * @param callback Function to call on sync
     * @return ESP_OK on success
     */
    esp_err_t ntp_client_register_callback(ntp_sync_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* NTP_CLIENT_H */