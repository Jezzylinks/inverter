/**
 * @file wifi_http_server.h
 * @brief Wi-Fi Configuration HTTP Server
 */

#ifndef WIFI_HTTP_SERVER_H
#define WIFI_HTTP_SERVER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_err.h"
#include <stdbool.h>

    /*==========================================================
     *
     *              CONFIGURATION
     *
     *=========================================================*/

#define WIFI_HTTP_SERVER_PORT 80

    /*==========================================================
     *
     *              SERVER CONTROL
     *
     *=========================================================*/

    /**
     * @brief Start HTTP configuration server
     *
     * Used during Wi-Fi provisioning mode
     */
    esp_err_t wifi_http_server_start(void);

    /**
     * @brief Stop HTTP configuration server
     */
    esp_err_t wifi_http_server_stop(void);

    /**
     * @brief Check server status
     */
    bool wifi_http_server_running(void);

    typedef void (*wifi_http_save_callback_t)(void);

    esp_err_t wifi_http_server_register_save_callback(
        wifi_http_save_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_HTTP_SERVER_H */