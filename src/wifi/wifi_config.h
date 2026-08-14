/**
 * @file wifi_config.h
 * @brief Wi-Fi configuration for ESP-IDF
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
#include "esp_event.h"
#include "freertos/event_groups.h"

#define WIFI_MAXIMUM_RETRY 10
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_RECONNECT_DELAY_MS 5000
#define WIFI_MONITOR_PERIOD_MS 5000
#define WIFI_SCAN_PERIOD_MS 30000

#define WIFI_HOSTNAME "solar-inverter"

#define WIFI_DEFAULT_SSID ""
#define WIFI_DEFAULT_PASSWORD ""

#define WIFI_PROVISION_AP_SSID "INVERTER_SETUP"
/* Empty means a unique WPA2 password is generated from the device MAC. */
#define WIFI_PROVISION_AP_PASSWORD ""
#define WIFI_PROVISION_CHANNEL 1
#define WIFI_PROVISION_MAX_CONN 4

#define INVERTER_WIFI_AUTH_MODE WIFI_AUTH_WPA2_PSK

#define WIFI_USE_STATIC_IP false
#define WIFI_STATIC_IP "192.168.1.200"
#define WIFI_STATIC_GATEWAY "192.168.1.1"
#define WIFI_STATIC_NETMASK "255.255.255.0"
#define WIFI_STATIC_DNS "8.8.8.8"

#define WIFI_POWER_SAVE_MODE WIFI_PS_MIN_MODEM

#define WIFI_RSSI_EXCELLENT (-45)
#define WIFI_RSSI_GOOD (-60)
#define WIFI_RSSI_FAIR (-70)
#define WIFI_RSSI_WEAK (-80)

#define WIFI_NVS_NAMESPACE "wifi"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASSWORD "password"
#define WIFI_NVS_KEY_STATIC_IP "static"
#define WIFI_NVS_KEY_DHCP "dhcp"

#define WIFI_HTTP_PORT 80
#define WIFI_HTTP_MAX_URI 8
#define WIFI_HTTP_STACK_SIZE 8192

#define WIFI_MONITOR_TASK_NAME "wifi_monitor"
#define WIFI_MONITOR_STACK_SIZE 6144
#define WIFI_MONITOR_PRIORITY 5

#define WIFI_MANAGER_TASK_NAME "wifi_manager"
#define WIFI_MANAGER_STACK_SIZE 6144
#define WIFI_MANAGER_PRIORITY 6

#define WIFI_MAX_SCAN_RESULTS 20
#define WIFI_SCAN_SHOW_HIDDEN true

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_SCAN_DONE_BIT BIT2

    /* Defined in wifi_manager.c */
    extern EventGroupHandle_t wifi_event_group;

    typedef enum
    {
        WIFI_SIGNAL_NONE = 0,
        WIFI_SIGNAL_WEAK,
        WIFI_SIGNAL_FAIR,
        WIFI_SIGNAL_GOOD,
        WIFI_SIGNAL_EXCELLENT

    } wifi_signal_quality_t;

    typedef struct
    {
        char ssid[33];
        char password[64];

    } wifi_credentials_t;

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CONFIG_H */