#ifndef WIFI_EVENTS_H
#define WIFI_EVENTS_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

    /*=========================================================
     * Wi-Fi Connection State
     *========================================================*/
    typedef enum
    {
        WIFI_STATE_UNINITIALIZED = 0,
        WIFI_STATE_IDLE,
        WIFI_STATE_CONNECTING,
        WIFI_STATE_CONNECTED,
        WIFI_STATE_DISCONNECTED,
        WIFI_STATE_RECONNECTING,
        WIFI_STATE_FAILED,
        WIFI_STATE_PROVISIONING

    } wifi_connection_state_t;

    /*=========================================================
     * Wi-Fi Runtime Status
     *========================================================*/
    typedef struct
    {
        wifi_connection_state_t state;

        bool connected;

        bool got_ip;

        bool internet_available;

        uint8_t retry_count;

        int8_t rssi;

        esp_ip4_addr_t ip;

        esp_ip4_addr_t gateway;

        esp_ip4_addr_t netmask;

    } wifi_status_t;

    /*=========================================================
     * Callback
     *========================================================*/
    typedef void (*wifi_status_callback_t)(const wifi_status_t *status);
    typedef void (*wifi_event_callback_t)(
        wifi_connection_state_t state);

    /*=========================================================
     * Initialization
     *========================================================*/

    esp_err_t wifi_events_init(void);

    esp_err_t wifi_events_deinit(void);

    /*=========================================================
     * Status
     *========================================================*/

    const wifi_status_t *wifi_events_get_status(void);

    wifi_connection_state_t wifi_events_get_state(void);

    bool wifi_events_is_connected(void);

    bool wifi_events_has_ip(void);

    /*=========================================================
     * Registration
     *========================================================*/

    esp_err_t wifi_events_register_callback(
        wifi_status_callback_t callback);

    esp_err_t wifi_events_unregister_callback(
        wifi_status_callback_t callback);

    /*=========================================================
     * Internal Event Handler
     *========================================================*
     * Used by wifi_controller.c
     * Do NOT call directly from application code.
     */

    static void wifi_event_handler(
        void *arg,
        esp_event_base_t event_base,
        int32_t event_id,
        void *event_data)

#ifdef __cplusplus
}
#endif

#endif /* WIFI_EVENTS_H */