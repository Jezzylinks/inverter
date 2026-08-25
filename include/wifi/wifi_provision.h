/**
 * @file wifi_provision.h
 * @brief Wi-Fi Provisioning Manager
 */

#ifndef WIFI_PROVISION_H
#define WIFI_PROVISION_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>

#include "esp_err.h"

    /*==========================================================
     *
     *              CONFIGURATION
     *
     *=========================================================*/

#define WIFI_PROVISION_TIMEOUT_MS (10 * 60 * 1000)

    /*==========================================================
     *
     *              STATE
     *
     *=========================================================*/

    typedef enum
    {
        WIFI_PROVISION_IDLE = 0,

        WIFI_PROVISION_RUNNING,

        WIFI_PROVISION_COMPLETE,

        WIFI_PROVISION_FAILED

    } wifi_provision_state_t;

    /*==========================================================
     *
     *              CONTROL
     *
     *=========================================================*/

    /**
     * @brief Initialize provisioning module
     */
    esp_err_t wifi_provision_init(void);
    esp_err_t wifi_provision_deinit(void);

    /**
     * @brief Start AP provisioning mode
     */
    esp_err_t wifi_provision_start(void);

    /**
     * @brief Stop provisioning mode
     */
    esp_err_t wifi_provision_stop(void);

    /**
     * @brief Check provisioning state
     */
    wifi_provision_state_t
    wifi_provision_get_state(void);

    /**
     * @brief Check if provisioning active
     */
    bool wifi_provision_is_running(void);

    typedef void (*wifi_provision_complete_callback_t)(void);

    esp_err_t wifi_provision_register_complete_callback(
        wifi_provision_complete_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif