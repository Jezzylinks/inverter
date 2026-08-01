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

#define WIFI_MAX_SSID_LEN 32
#define WIFI_MAX_PASSWORD_LEN 64
#define WIFI_MAX_HOSTNAME_LEN 32
#define WIFI_MAX_IP_STRING_LEN 16
#define WIFI_MAX_MAC_STRING_LEN 18

    /**
     * Wi-Fi operating mode.
     */
    typedef enum
    {
        WIFI_MODE_DISABLED = 0,

        WIFI_MODE_STA,

        WIFI_MODE_AP,

        WIFI_MODE_AP_STA

    } wifi_operating_mode_t;

    /**
     * Internal Wi-Fi state machine.
     */
    typedef enum
    {
        WIFI_STATE_OFF = 0,

        WIFI_STATE_INITIALIZING,

        WIFI_STATE_READY,

        WIFI_STATE_CONNECTING,

        WIFI_STATE_CONNECTED,

        WIFI_STATE_GOT_IP,

        WIFI_STATE_DISCONNECTED,

        WIFI_STATE_RECONNECTING,

        WIFI_STATE_AP_STARTED,

        WIFI_STATE_PROVISIONING,

        WIFI_STATE_ERROR

    } wifi_state_t;

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
     * Network credentials.
     */
    typedef struct
    {
        char ssid[WIFI_MAX_SSID_LEN + 1];

        char password[WIFI_MAX_PASSWORD_LEN + 1];

    } wifi_credentials_t;

    /**
     * Static network configuration.
     */
    typedef struct
    {
        bool dhcp;

        char ip[WIFI_MAX_IP_STRING_LEN];

        char gateway[WIFI_MAX_IP_STRING_LEN];

        char subnet[WIFI_MAX_IP_STRING_LEN];

        char dns1[WIFI_MAX_IP_STRING_LEN];

        char dns2[WIFI_MAX_IP_STRING_LEN];

    } wifi_network_config_t;

    /**
     * Persistent Wi-Fi settings.
     */
    typedef struct
    {
        wifi_operating_mode_t mode;

        wifi_credentials_t credentials;

        wifi_network_config_t network;

        char hostname[WIFI_MAX_HOSTNAME_LEN + 1];

        bool auto_connect;

    } wifi_settings_t;

    /**
     * Runtime connection status.
     */
    typedef struct
    {
        wifi_state_t state;

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

    } wifi_status_t;

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

        wifi_state_t state;

    } wifi_result_t;

#ifdef __cplusplus
}
#endif

#endif /* WIFI_TYPES_H */