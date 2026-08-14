/**
 * @file wifi_http_server.h
 * @brief Hardened HTTP server for local Wi-Fi provisioning.
 */
#ifndef WIFI_HTTP_SERVER_H
#define WIFI_HTTP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define WIFI_HTTP_SERVER_PORT 80U
#define WIFI_HTTP_MAX_FORM_BYTES 256U

/**
 * Called after credentials have been committed and the HTTP response has been
 * sent. The callback runs from a dedicated task, never from the HTTP server
 * task, so it may stop provisioning or change Wi-Fi mode safely.
 */
typedef void (*wifi_http_save_callback_t)(void);

/** Start the local provisioning server. Idempotent on success. */
esp_err_t wifi_http_server_start(void);

/** Stop the local provisioning server and cancel any pending save callback. */
esp_err_t wifi_http_server_stop(void);

/** True only while the HTTP server handle is valid. */
bool wifi_http_server_running(void);

/** Register the single provisioning-complete callback; NULL clears it. */
esp_err_t wifi_http_server_register_save_callback(
    wifi_http_save_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_HTTP_SERVER_H */
