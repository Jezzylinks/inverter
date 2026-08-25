/**
 * @file wifi_dns_server.h
 * @brief DNS Server for Captive Portal
 */

#ifndef WIFI_DNS_SERVER_H
#define WIFI_DNS_SERVER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>

#include "esp_err.h"

    /*----------------------------------------------------------
     * Initialization
     *---------------------------------------------------------*/

    esp_err_t wifi_dns_server_init(void);

    esp_err_t wifi_dns_server_deinit(void);

    /*----------------------------------------------------------
     * Control
     *---------------------------------------------------------*/

    esp_err_t wifi_dns_server_start(void);

    esp_err_t wifi_dns_server_stop(void);

    /*----------------------------------------------------------
     * Status
     *---------------------------------------------------------*/

    bool wifi_dns_server_is_running(void);

#ifdef __cplusplus
}

#endif

#endif