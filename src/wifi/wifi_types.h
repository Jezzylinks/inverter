/**
 * @file wifi_types.h
 * @brief Common Wi-Fi types for the inverter Wi-Fi subsystem.
 *
 * ESP-IDF Version:
 *      v5.2.1
 */

#ifndef WIFI_TYPES_H
#define WIFI_TYPES_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "wifi_config.h"

#define WIFI_MAX_SSID_LEN 32
#define WIFI_MAX_PASSWORD_LEN 64
#define WIFI_MAX_HOSTNAME_LEN 32
#define WIFI_MAX_IP_STRING_LEN 16
#define WIFI_MAX_MAC_STRING_LEN 18

    /**
     * Application-level desired operating mode.
     *
     * These names deliberately differ from ESP-IDF's WIFI_MODE_* enumerators.
     */
    typedef enum
    {
        INVERTER_WIFI_MODE_DISABLED = 0,
        INVERTER_WIFI_MODE_STA,
        INVERTER_WIFI_MODE_AP,
        INVERTER_WIFI_MODE_AP_STA

    } inverter_wifi_operating_mode_t;

    /**
     * Diagnostic state model for consumers that do not use wifi_events.h.
     *
     * The runtime connection state is wifi_connection_state_t from wifi_events.h.
     */
    typedef enum
    {
        INVERTER_WIFI_STATE_OFF = 0,
        INVERTER_WIFI_STATE_INITIALIZING,
        INVERTER_WIFI_STATE_READY,
        INVERTER_WIFI_STATE_CONNECTING,
        INVERTER_WIFI_STATE_CONNECTED,
        INVERTER_WIFI_STATE_GOT_IP,
        INVERTER_WIFI_STATE_DISCONNECTED,
        INVERTER_WIFI_STATE_RECONNECTING,
        INVERTER_WIFI_STATE_AP_STARTED,
        INVERTER_WIFI_STATE_PROVISIONING,
        INVERTER_WIFI_STATE_ERROR

    } inverter_wifi_state_t;

    /**
     * Connection security.
     */
    typedef enum
    {
        WIFI_SECURITY_OPEN = 0,

        WIFI_SECURITY_WEP,

        WIFI_SECURITY_WPA,

        WIFI_SECURITY_WPA2,

        WIFI_SECURITY_WPA_WPA2,

        WIFI_SECURITY_WPA3,

        WIFI_SECURITY_UNKNOWN

    } wifi_security_t;

    /**
     * Disconnect reason.
     */
    typedef enum
    {
        WIFI_DISCONNECT_NONE = 0,

        WIFI_DISCONNECT_AUTH_FAIL,

        WIFI_DISCONNECT_AP_NOT_FOUND,

        WIFI_DISCONNECT_TIMEOUT,

        WIFI_DISCONNECT_NO_IP,

        WIFI_DISCONNECT_LOST_SIGNAL,

        WIFI_DISCONNECT_USER,

        WIFI_DISCONNECT_UNKNOWN

    } wifi_disconnect_reason_t;

    /**
     * Static network configuration used by the optional generic settings model.
     *
     * Persistent driver-facing settings use wifi_network_config_t from
     * wifi_storage.h. Credentials are defined once in wifi_config.h.
     */
    typedef struct
    {
        bool dhcp;

        char ip[WIFI_MAX_IP_STRING_LEN];

        char gateway[WIFI_MAX_IP_STRING_LEN];

        char subnet[WIFI_MAX_IP_STRING_LEN];

        char dns1[WIFI_MAX_IP_STRING_LEN];

        char dns2[WIFI_MAX_IP_STRING_LEN];

    } wifi_static_network_config_t;

    /**
     * Persistent Wi-Fi settings.
     */
    typedef struct
    {
        inverter_wifi_operating_mode_t mode;

        wifi_credentials_t credentials;

        wifi_static_network_config_t network;

        char hostname[WIFI_MAX_HOSTNAME_LEN + 1];

        bool auto_connect;

    } wifi_settings_t;

    /**
     * Runtime connection status.
     */
    typedef struct
    {
        inverter_wifi_state_t state;

        bool initialized;

        bool connected;

        bool got_ip;

        int8_t rssi;

        uint8_t channel;

        wifi_security_t security;

        wifi_disconnect_reason_t disconnect_reason;

        char ip[WIFI_MAX_IP_STRING_LEN];

        char mac[WIFI_MAX_MAC_STRING_LEN];

        uint32_t reconnect_count;

    } wifi_diagnostic_status_t;

    /**
     * Access point information.
     */
    typedef struct
    {
        char ssid[WIFI_MAX_SSID_LEN + 1];

        int8_t rssi;

        uint8_t channel;

        wifi_security_t security;

    } wifi_scan_result_t;

    /**
     * Generic Wi-Fi result.
     */
    typedef struct
    {
        esp_err_t result;

        inverter_wifi_state_t state;

    } wifi_result_t;

#ifdef __cplusplus
}
#endif

#endif /* WIFI_TYPES_H */