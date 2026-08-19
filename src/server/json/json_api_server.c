#include "json_api_server.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "security/security.h"
#include "system_state.h"

#include "json_api_device.h"
#include "json_api_cloud.h"
#include "json_api_mqtt.h"
#include "json_api_ota.h"
#include "json_api_system.h"
#include "json_api_wifi.h"

static const char *TAG = "JSON_API";
static httpd_handle_t s_server = NULL;
extern system_state_t sys_state;

void json_api_add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "http://192.168.4.1");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-Inverter-PIN");
    httpd_resp_set_type(req, "application/json");
}

static const char *api_status_line(int status_code)
{
    switch (status_code) {
    case 200: return "200 OK";
    case 202: return "202 Accepted";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
    }
}

esp_err_t json_api_send(httpd_req_t *req, cJSON *root, int status_code)
{
    json_api_add_cors_headers(req);
    char *json_str = cJSON_Print(root);
    if (json_str == NULL)
    {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"JSON serialization failed\"}");
        return ESP_OK;
    }

    httpd_resp_set_status(req, api_status_line(status_code));
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t json_api_options_handler(httpd_req_t *req)
{
    json_api_add_cors_headers(req);
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t json_api_receive_json(httpd_req_t *req, cJSON **json)
{
    if (req == NULL || json == NULL || req->content_len == 0 || req->content_len > 2048) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *body = calloc((size_t)req->content_len + 1U, 1U);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t received = 0U;
    while (received < (size_t)req->content_len) {
        const int result = httpd_req_recv(req, body + received,
                                           (size_t)req->content_len - received);
        if (result <= 0) {
            free(body);
            return result == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        received += (size_t)result;
    }
    body[received] = '\0';
    *json = cJSON_Parse(body);
    free(body);
    return *json != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t json_api_require_pin(httpd_req_t *req)
{
    if (!req || !sys_state.security.enabled || req->method == HTTP_OPTIONS) {
        return ESP_OK;
    }
    const size_t header_len = httpd_req_get_hdr_value_len(req, "X-Inverter-PIN");
    if (header_len != SECURITY_PIN_LEN) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "PIN required");
        return json_api_send(req, error, 401);
    }
    char pin_text[SECURITY_PIN_LEN + 1U] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Inverter-PIN", pin_text,
                                    sizeof(pin_text)) != ESP_OK) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Invalid PIN header");
        return json_api_send(req, error, 401);
    }
    uint8_t pin[SECURITY_PIN_LEN] = {0};
    for (size_t i = 0; i < SECURITY_PIN_LEN; ++i) {
        if (pin_text[i] < '0' || pin_text[i] > '9') {
            cJSON *error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "Invalid PIN");
            return json_api_send(req, error, 401);
        }
        pin[i] = (uint8_t)(pin_text[i] - '0');
    }
    if (!security_verify_pin(pin)) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Unauthorized");
        return json_api_send(req, error, 401);
    }
    return ESP_OK;
}

static void register_uri_group(const httpd_uri_t *uris, size_t count)
{
    for (size_t i = 0U; i < count; ++i) {
        const esp_err_t err = httpd_register_uri_handler(s_server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register URI %s: %s",
                     uris[i].uri, esp_err_to_name(err));
        }
    }
}

static void unregister_uri_group(const httpd_uri_t *uris, size_t count)
{
    for (size_t i = 0U; i < count; ++i) {
        (void)httpd_unregister_uri_handler(s_server, uris[i].uri, uris[i].method);
    }
}

esp_err_t json_api_server_start(httpd_handle_t server)
{
    if (server == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_server = server;
    size_t device_count = 0U;
    size_t wifi_count = 0U;
    size_t mqtt_count = 0U;
    size_t ota_count = 0U;
    size_t system_count = 0U;
    size_t cloud_count = 0U;
    const httpd_uri_t *device_uris = json_api_device_uris(&device_count);
    const httpd_uri_t *wifi_uris = json_api_wifi_uris(&wifi_count);
    const httpd_uri_t *mqtt_uris = json_api_mqtt_uris(&mqtt_count);
    const httpd_uri_t *ota_uris = json_api_ota_uris(&ota_count);
    const httpd_uri_t *system_uris = json_api_system_uris(&system_count);
    const httpd_uri_t *cloud_uris = json_api_cloud_uris(&cloud_count);
    register_uri_group(device_uris, device_count);
    register_uri_group(wifi_uris, wifi_count);
    register_uri_group(mqtt_uris, mqtt_count);
    register_uri_group(ota_uris, ota_count);
    register_uri_group(system_uris, system_count);
    register_uri_group(cloud_uris, cloud_count);

    ESP_LOGI(TAG, "JSON API server registered");
    return ESP_OK;
}

esp_err_t json_api_server_stop(void)
{
    if (s_server == NULL)
    {
        return ESP_OK;
    }

    size_t device_count = 0U;
    size_t wifi_count = 0U;
    size_t mqtt_count = 0U;
    size_t ota_count = 0U;
    size_t system_count = 0U;
    size_t cloud_count = 0U;
    unregister_uri_group(json_api_device_uris(&device_count), device_count);
    unregister_uri_group(json_api_wifi_uris(&wifi_count), wifi_count);
    unregister_uri_group(json_api_mqtt_uris(&mqtt_count), mqtt_count);
    unregister_uri_group(json_api_ota_uris(&ota_count), ota_count);
    unregister_uri_group(json_api_system_uris(&system_count), system_count);
    unregister_uri_group(json_api_cloud_uris(&cloud_count), cloud_count);
    s_server = NULL;
    return ESP_OK;
}
