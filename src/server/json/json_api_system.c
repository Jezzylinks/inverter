#include "server/json/json_api_system.h"
#include "server/json/json_api_server.h"

#include "esp_http_server.h"
#include "cJSON.h"

#include "server/network_services.h"

static void api_add_mqtt_config(cJSON *root, const network_mqtt_config_t *config)
{
    cJSON *mqtt = cJSON_CreateObject();
    cJSON_AddBoolToObject(mqtt, "enabled", config->enabled);
    cJSON_AddStringToObject(mqtt, "broker", config->broker_url);
    cJSON_AddStringToObject(mqtt, "client_id", config->client_id);
    cJSON_AddStringToObject(mqtt, "username", config->username);
    cJSON_AddStringToObject(mqtt, "publish_topic", config->publish_topic);
    cJSON_AddStringToObject(mqtt, "subscribe_topic", config->subscribe_topic);
    cJSON_AddNumberToObject(mqtt, "keepalive_sec", config->keepalive_sec);
    cJSON_AddNumberToObject(mqtt, "qos", config->qos);
    cJSON_AddBoolToObject(mqtt, "retain", config->retain);
    network_services_status_t status;
    network_services_get_status(&status);
    cJSON_AddBoolToObject(mqtt, "connected", status.mqtt_connected);
    cJSON_AddItemToObject(root, "mqtt", mqtt);
}

static esp_err_t api_services_handler(httpd_req_t *req)
{
    esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    network_services_status_t status;
    network_mqtt_config_t config;
    network_services_get_status(&status);
    network_services_get_mqtt_config(&config);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "http", status.http_running);
    cJSON_AddBoolToObject(root, "dashboard", status.dashboard_running);
    cJSON_AddBoolToObject(root, "websocket", status.websocket_running);
    cJSON_AddBoolToObject(root, "mdns", status.mdns_running);
    cJSON_AddBoolToObject(root, "ntp", status.ntp_running);
    cJSON_AddBoolToObject(root, "ntp_time_set", status.ntp_time_set);
    cJSON_AddBoolToObject(root, "mqtt_configured", status.mqtt_configured);
    cJSON_AddBoolToObject(root, "mqtt_connected", status.mqtt_connected);
    api_add_mqtt_config(root, &config);
    return json_api_send(req, root, 200);
}

static const httpd_uri_t s_system_uris[] = {
    {.uri = "/api/v1/services", .method = HTTP_GET, .handler = api_services_handler},
};

const httpd_uri_t *json_api_system_uris(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(s_system_uris) / sizeof(s_system_uris[0]);
    }
    return s_system_uris;
}
