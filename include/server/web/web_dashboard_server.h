#ifndef WEB_DASHBOARD_SERVER_H
#define WEB_DASHBOARD_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

/** Mount the dashboard filesystem and prepare static asset serving. */
esp_err_t web_dashboard_server_init(void);

/** Unmount the dashboard filesystem. */
esp_err_t web_dashboard_server_deinit(void);

/** Register dashboard routes on the shared station-mode HTTP server. */
esp_err_t web_dashboard_server_register(httpd_handle_t server);

/** Unregister dashboard routes from the shared station-mode HTTP server. */
esp_err_t web_dashboard_server_unregister(httpd_handle_t server);

/** Return whether the dashboard filesystem is mounted and routes are ready. */
bool web_dashboard_server_available(void);

#ifdef __cplusplus
}
#endif

#endif
