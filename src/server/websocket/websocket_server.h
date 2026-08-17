/**
 * @file websocket_server.h
 * @brief WebSocket Server Interface
 */

#ifndef WEBSOCKET_SERVER_H
#define WEBSOCKET_SERVER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_err.h"
#include "esp_http_server.h"
#include "wifi/wifi_events.h"

    /**
     * @brief Initialize WebSocket server subsystem
     * @return ESP_OK on success
     */
    esp_err_t websocket_server_init(void);

    /**
     * @brief Deinitialize WebSocket server
     * @return ESP_OK on success
     */
    esp_err_t websocket_server_deinit(void);

    /**
     * @brief Register WebSocket URI on an HTTP server
     * @param server HTTP server handle
     * @return ESP_OK on success
     */
    esp_err_t websocket_server_register(httpd_handle_t server);

    /**
     * @brief Unregister WebSocket URI
     * @param server HTTP server handle
     * @return ESP_OK on success
     */
    esp_err_t websocket_server_unregister(httpd_handle_t server);

    /**
     * @brief Broadcast status to all connected WebSocket clients
     * @param status Current WiFi status
     */
    void websocket_broadcast_status(const wifi_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* WEBSOCKET_SERVER_H */