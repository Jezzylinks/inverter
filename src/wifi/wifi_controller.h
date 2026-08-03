/**
 * @file wifi_controller.h
 * @brief High Level Wi-Fi Controller
 */

#ifndef WIFI_CONTROLLER_H
#define WIFI_CONTROLLER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "wifi_manager.h"
#include "wifi_http_server.h"

    /*==========================================================
     *
     *              Controller State
     *
     *=========================================================*/

    typedef enum
    {
        WIFI_CONTROLLER_IDLE = 0,

        WIFI_CONTROLLER_STARTING,

        WIFI_CONTROLLER_CONNECTING,

        WIFI_CONTROLLER_CONNECTED,

        WIFI_CONTROLLER_PROVISIONING,

        WIFI_CONTROLLER_ERROR

    } wifi_controller_state_t;

    /*==========================================================
     *
     *              Initialization
     *
     *=========================================================*/

    /**
     * @brief Initialize Wi-Fi controller
     */
    esp_err_t wifi_controller_init(void);

    /**
     * @brief Start Wi-Fi system
     *
     * Decides:
     * - connect STA
     * - start provisioning AP
     */
    esp_err_t wifi_controller_start(void);

    /**
     * @brief Stop Wi-Fi system
     */
    esp_err_t wifi_controller_stop(void);

    /*==========================================================
     *
     *              Connection Control
     *
     *=========================================================*/

    /**
     * @brief Force Wi-Fi reconnect
     */
    esp_err_t wifi_controller_reconnect(void);

    /**
     * @brief Start provisioning manually
     */
    esp_err_t wifi_controller_start_provisioning(void);

    /**
     * @brief Exit provisioning
     */
    esp_err_t wifi_controller_stop_provisioning(void);

    /*==========================================================
     *
     *              Status
     *
     *=========================================================*/

    wifi_controller_state_t
    wifi_controller_get_state(void);

    bool wifi_controller_is_connected(void);

    const wifi_status_t *
    wifi_controller_get_status(void);

    /*==========================================================
     *
     *              Configuration
     *
     *=========================================================*/

    esp_err_t wifi_controller_set_config(
        const wifi_manager_config_t *config);

    esp_err_t wifi_controller_get_config(
        wifi_manager_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CONTROLLER_H */