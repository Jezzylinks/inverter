/**
 * @file json_api_server.h
 * @brief JSON REST API Server Interface
 */

#ifndef JSON_API_SERVER_H
#define JSON_API_SERVER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_err.h"
#include "esp_http_server.h"

    /**
     * Routes registered on the shared station-mode HTTP server:
     * GET  /api/v1/status, /api/v1/scan, /api/v1/config, /api/v1/services
     * POST /api/v1/connect, /api/v1/disconnect, /api/v1/reset
     * POST /api/v1/mqtt/config, /api/v1/mqtt/connect,
     *      /api/v1/mqtt/disconnect, /api/v1/mqtt/publish,
     *      /api/v1/mqtt/subscribe
     *
     * Mutating routes require X-Inverter-PIN when security is enabled.
     * Register JSON API URI handlers on an existing HTTP server.
     * @param server Existing HTTP server handle
     * @return ESP_OK on success
     */
    esp_err_t json_api_server_start(httpd_handle_t server);

    /**
     * @brief Unregister JSON API URI handlers
     * @return ESP_OK on success
     */
    esp_err_t json_api_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* JSON_API_SERVER_H */