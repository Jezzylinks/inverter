/**
 * @file wifi_monitor.h
 * @brief Wi-Fi Runtime Monitor
 *
 * Monitors:
 * - RSSI signal strength
 * - IP address status
 * - Connection health
 * - Internet availability
 * - Periodic Wi-Fi status updates
 */

#ifndef WIFI_MONITOR_H
#define WIFI_MONITOR_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_ping.h"

    /*==========================================================
     *
     *              CONFIGURATION
     *
     *=========================================================*/

#define WIFI_MONITOR_TASK_STACK_SIZE 3072
#define WIFI_MONITOR_TASK_PRIORITY 5

#define WIFI_MONITOR_INTERVAL_MS 10000

    /*==========================================================
     *
     *              INTERNET STATUS
     *
     *=========================================================*/

    typedef enum
    {
        WIFI_INTERNET_UNKNOWN = 0,

        WIFI_INTERNET_AVAILABLE,

        WIFI_INTERNET_UNAVAILABLE

    } wifi_internet_status_t;

    typedef void (*wifi_internet_callback_t)(
        wifi_internet_status_t status);

    esp_err_t wifi_monitor_register_internet_callback(
        wifi_internet_callback_t callback);

    /*==========================================================
     *
     *              MONITOR STATUS
     *
     *=========================================================*/

    typedef struct
    {
        bool connected;

        bool got_ip;

        int8_t rssi;

        esp_ip4_addr_t ip;

        wifi_internet_status_t internet;

        uint32_t uptime_seconds;

    } wifi_monitor_status_t;

    /*==========================================================
     *
     *              CALLBACK
     *
     *=========================================================*/

    typedef void (*wifi_monitor_callback_t)(
        const wifi_monitor_status_t *status);

    /*==========================================================
     *
     *              INITIALIZATION
     *
     *=========================================================*/

    /**
     * @brief Initialize Wi-Fi monitor task
     */
    esp_err_t wifi_monitor_init(void);

    /**
     * @brief Stop Wi-Fi monitor
     */
    esp_err_t wifi_monitor_deinit(void);

    /*==========================================================
     *
     *              CONTROL
     *
     *=========================================================*/

    /**
     * @brief Start monitoring
     */
    esp_err_t wifi_monitor_start(void);

    /**
     * @brief Stop monitoring
     */
    esp_err_t wifi_monitor_stop(void);

    /*==========================================================
     *
     *              STATUS
     *
     *=========================================================*/

    bool wifi_monitor_is_online(void);

    int8_t wifi_monitor_get_rssi(void);

    wifi_internet_status_t
    wifi_monitor_get_internet_status(void);

    const wifi_monitor_status_t *
    wifi_monitor_get_status(void);

    /*==========================================================
     *
     *              CALLBACK
     *
     *=========================================================*/

    esp_err_t wifi_monitor_register_callback(
        wifi_monitor_callback_t callback);

    esp_err_t wifi_monitor_unregister_callback(
        wifi_monitor_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MONITOR_H */