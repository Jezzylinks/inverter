#ifndef JSON_API_CLOUD_H
#define JSON_API_CLOUD_H

#include <stddef.h>
#include "esp_http_server.h"

const httpd_uri_t *json_api_cloud_uris(size_t *count);

#endif
