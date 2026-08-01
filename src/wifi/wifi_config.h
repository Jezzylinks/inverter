/**
 * @file wifi_config.h
 * @brief Wi-Fi configuration for ESP-IDF
 *
 * Project : IoT Pure Sine Wave Inverter
 * Author  : Johnson Ogbu
 */

#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>

#include "esp_wifi.h"
#include "esp_err.h"

    /*==========================================================
     *
     *                GENERAL CONFIGURATION
     *
     *=========================================================*/

#define WIFI_MAXIMUM_RETRY 10

#define WIFI_CONNECT_TIMEOUT_MS 15000

#define WIFI_RECONNECT_DELAY_MS 5000

#define WIFI_MONITOR_PERIOD_MS 5000

#define WIFI_SCAN_PERIOD_MS 30000

#define WIFI_HOSTNAME "solar-inverter"

    /*==========================================================
     *
     *               DEFAULT ACCESS POINT
     *
     * Used only if NVS is empty.
     *
     *=========================================================*/

#define WIFI_DEFAULT_SSID ""

#define WIFI_DEFAULT_PASSWORD ""

    /*==========================================================
     *
     *                  DEVICE HOSTNAME
     *
     *=========================================================*/

#define WIFI_HOSTNAME "ESP32-INVERTER"

    /*==========================================================
     *
     *                  PROVISIONING
     *
     *=========================================================*/

#define WIFI_PROVISION_AP_SSID "INVERTER_SETUP"

#define WIFI_PROVISION_AP_PASSWORD "12345678"

#define WIFI_PROVISION_CHANNEL 1

#define WIFI_PROVISION_MAX_CONN 4

    /*==========================================================
     *
     *                 AUTHENTICATION
     *
     *=========================================================*/

#define WIFI_AUTH_MODE WIFI_AUTH_WPA2_PSK

    /*==========================================================
     *
     *                 STATIC IP SUPPORT
     *
     *=========================================================*/

#define WIFI_USE_STATIC_IP false

#define WIFI_STATIC_IP "192.168.1.200"

#define WIFI_STATIC_GATEWAY "192.168.1.1"

#define WIFI_STATIC_NETMASK "255.255.255.0"

#define WIFI_STATIC_DNS "8.8.8.8"

    /*==========================================================
     *
     *                 POWER SAVE MODE
     *
     *=========================================================*/

#define WIFI_POWER_SAVE_MODE WIFI_PS_MIN_MODEM

    /*==========================================================
     *
     *                  RSSI LIMITS
     *
     *=========================================================*/

#define WIFI_RSSI_EXCELLENT (-45)

#define WIFI_RSSI_GOOD (-60)

#define WIFI_RSSI_FAIR (-70)

#define WIFI_RSSI_WEAK (-80)

    /*==========================================================
     *
     *                 NVS STORAGE
     *
     *=========================================================*/

#define WIFI_NVS_NAMESPACE "wifi"

#define WIFI_NVS_KEY_SSID "ssid"

#define WIFI_NVS_KEY_PASSWORD "password"

#define WIFI_NVS_KEY_STATIC_IP "static"

#define WIFI_NVS_KEY_DHCP "dhcp"

    /*==========================================================
     *
     *                 HTTP SERVER
     *
     *=========================================================*/

#define WIFI_HTTP_PORT 80

#define WIFI_HTTP_MAX_URI 8

#define WIFI_HTTP_STACK_SIZE 4096

    /*==========================================================
     *
     *                  MONITOR TASK
     *
     *=========================================================*/

#define WIFI_MONITOR_TASK_NAME "wifi_monitor"

#define WIFI_MONITOR_STACK_SIZE 4096

#define WIFI_MONITOR_PRIORITY 5

    /*==========================================================
     *
     *                RECONNECT TASK
     *
     *=========================================================*/

#define WIFI_MANAGER_TASK_NAME "wifi_manager"

#define WIFI_MANAGER_STACK_SIZE 4096

#define WIFI_MANAGER_PRIORITY 6

    /*==========================================================
     *
     *                  SCAN LIMITS
     *
     *=========================================================*/

#define WIFI_MAX_SCAN_RESULTS 20

#define WIFI_SCAN_SHOW_HIDDEN true

    /*==========================================================
     *
     *                  EVENT GROUP
     *
     *=========================================================*/

#define WIFI_CONNECTED_BIT BIT0

#define WIFI_FAIL_BIT BIT1

#define WIFI_SCAN_DONE_BIT BIT2

    /*==========================================================
     *
     *                  WIFI STATES
     *
     *=========================================================*/

    typedef enum
    {
        WIFI_STATE_DISABLED = 0,

        WIFI_STATE_IDLE,

        WIFI_STATE_CONNECTING,

        WIFI_STATE_CONNECTED,

        WIFI_STATE_DISCONNECTED,

        WIFI_STATE_SCANNING,

        WIFI_STATE_PROVISIONING,

        WIFI_STATE_FAILED

    } wifi_state_t;

    /*==========================================================
     *
     *                RSSI QUALITY
     *
     *=========================================================*/

    typedef enum
    {
        WIFI_SIGNAL_NONE = 0,

        WIFI_SIGNAL_WEAK,

        WIFI_SIGNAL_FAIR,

        WIFI_SIGNAL_GOOD,

        WIFI_SIGNAL_EXCELLENT

    } wifi_signal_quality_t;

    /*==========================================================
     *
     *               STORED CREDENTIALS
     *
     *=========================================================*/

    typedef struct
    {
        char ssid[33];

        char password[65];

    } wifi_credentials_t;

    /*==========================================================
     *
     *                CONNECTION STATUS
     *
     *=========================================================*/

    typedef struct
    {
        bool connected;

        wifi_state_t state;

        wifi_signal_quality_t quality;

        int8_t rssi;

        uint32_t reconnect_count;

        uint32_t disconnect_count;

    } wifi_status_t;

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CONFIG_H */