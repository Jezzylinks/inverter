#ifndef JSON_API_SERVER_H
#define JSON_API_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

/* Common infrastructure shared by endpoint-group modules. */
void json_api_add_cors_headers(httpd_req_t *req);
esp_err_t json_api_send(httpd_req_t *req, cJSON *root, int status_code);
esp_err_t json_api_require_pin(httpd_req_t *req);
esp_err_t json_api_receive_json(httpd_req_t *req, cJSON **json);
esp_err_t json_api_options_handler(httpd_req_t *req);

esp_err_t json_api_server_start(httpd_handle_t server);
esp_err_t json_api_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* JSON_API_SERVER_H */
