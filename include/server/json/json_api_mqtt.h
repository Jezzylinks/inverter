#ifndef JSON_API_MQTT_H
#define JSON_API_MQTT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "esp_http_server.h"

const httpd_uri_t *json_api_mqtt_uris(size_t *count);

#ifdef __cplusplus
}
#endif

#endif /* JSON_API_MQTT_H */
