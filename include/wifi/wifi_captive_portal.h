/**
 * @file wifi_captive_portal.h
 * @brief WiFi Captive Portal
 */

#ifndef WIFI_CAPTIVE_PORTAL_H
#define WIFI_CAPTIVE_PORTAL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>

#include "esp_err.h"

    /*----------------------------------------------------------
     * Initialization
     *---------------------------------------------------------*/

    esp_err_t wifi_captive_portal_init(void);

    esp_err_t wifi_captive_portal_deinit(void);

    /*----------------------------------------------------------
     * Control
     *---------------------------------------------------------*/

    esp_err_t wifi_captive_portal_start(void);

    esp_err_t wifi_captive_portal_stop(void);

    /*----------------------------------------------------------
     * Status
     *---------------------------------------------------------*/

    bool wifi_captive_portal_is_running(void);

#ifdef __cplusplus
}
#endif

#endif