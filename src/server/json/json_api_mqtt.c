#include "server/json/json_api_mqtt.h"
#include "server/json/json_api_server.h"

#include <string.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "server/network_services.h"

static esp_err_t api_mqtt_config_handler(httpd_req_t *req)
{
    esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    cJSON *json = NULL;
    esp_err_t err = json_api_receive_json(req, &json);
    if (err != ESP_OK) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Invalid JSON body");
        return json_api_send(req, error, err == ESP_ERR_NO_MEM ? 503 : 400);
    }

    network_mqtt_config_t config;
    (void)network_services_get_mqtt_config(&config);
    cJSON *item = cJSON_GetObjectItem(json, "enabled");
    if (item != NULL && cJSON_IsBool(item)) config.enabled = cJSON_IsTrue(item);
    item = cJSON_GetObjectItem(json, "broker");
    if (item != NULL && cJSON_IsString(item)) strncpy(config.broker_url, item->valuestring, sizeof(config.broker_url) - 1U);
    item = cJSON_GetObjectItem(json, "client_id");
    if (item != NULL && cJSON_IsString(item)) strncpy(config.client_id, item->valuestring, sizeof(config.client_id) - 1U);
    item = cJSON_GetObjectItem(json, "username");
    if (item != NULL && cJSON_IsString(item)) strncpy(config.username, item->valuestring, sizeof(config.username) - 1U);
    item = cJSON_GetObjectItem(json, "password");
    if (item != NULL && cJSON_IsString(item)) strncpy(config.password, item->valuestring, sizeof(config.password) - 1U);
    item = cJSON_GetObjectItem(json, "publish_topic");
    if (item != NULL && cJSON_IsString(item)) strncpy(config.publish_topic, item->valuestring, sizeof(config.publish_topic) - 1U);
    item = cJSON_GetObjectItem(json, "subscribe_topic");
    if (item != NULL && cJSON_IsString(item)) strncpy(config.subscribe_topic, item->valuestring, sizeof(config.subscribe_topic) - 1U);
    item = cJSON_GetObjectItem(json, "keepalive_sec");
    if (item != NULL && cJSON_IsNumber(item)) config.keepalive_sec = item->valueint;
    item = cJSON_GetObjectItem(json, "qos");
    if (item != NULL && cJSON_IsNumber(item)) config.qos = item->valueint;
    item = cJSON_GetObjectItem(json, "retain");
    if (item != NULL && cJSON_IsBool(item)) config.retain = cJSON_IsTrue(item);
    cJSON_Delete(json);

    err = network_services_set_mqtt_config(&config);
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(response, "message", "MQTT configuration saved");
    } else {
        cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
    return json_api_send(req, response, err == ESP_OK ? 200 : 400);
}

static esp_err_t api_mqtt_connect_handler(httpd_req_t *req)
{
    esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    const esp_err_t err = network_services_mqtt_connect();
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddBoolToObject(response, "pending", err == ESP_OK);
    cJSON_AddStringToObject(response, "message", err == ESP_OK ? "MQTT connection requested" : esp_err_to_name(err));
    return json_api_send(req, response, err == ESP_OK ? 202 : 400);
}

static esp_err_t api_mqtt_disconnect_handler(httpd_req_t *req)
{
    esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    const esp_err_t err = network_services_mqtt_disconnect();
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message", err == ESP_OK ? "MQTT disconnected" : esp_err_to_name(err));
    return json_api_send(req, response, err == ESP_OK ? 200 : 400);
}

static esp_err_t api_mqtt_publish_handler(httpd_req_t *req)
{
    esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    cJSON *json = NULL;
    esp_err_t err = json_api_receive_json(req, &json);
    if (err != ESP_OK) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Invalid JSON body");
        return json_api_send(req, error, 400);
    }
    cJSON *topic = cJSON_GetObjectItem(json, "topic");
    cJSON *data = cJSON_GetObjectItem(json, "data");
    cJSON *qos = cJSON_GetObjectItem(json, "qos");
    cJSON *retain = cJSON_GetObjectItem(json, "retain");
    const int message_id = network_services_mqtt_publish(
        topic && cJSON_IsString(topic) ? topic->valuestring : NULL,
        data && cJSON_IsString(data) ? data->valuestring : NULL,
        qos && cJSON_IsNumber(qos) ? qos->valueint : 0,
        retain && cJSON_IsBool(retain) ? cJSON_IsTrue(retain) : false);
    cJSON_Delete(json);
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", message_id >= 0);
    cJSON_AddNumberToObject(response, "message_id", message_id);
    return json_api_send(req, response, message_id >= 0 ? 200 : 400);
}

static esp_err_t api_mqtt_subscribe_handler(httpd_req_t *req)
{
    esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    cJSON *json = NULL;
    esp_err_t err = json_api_receive_json(req, &json);
    if (err != ESP_OK) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Invalid JSON body");
        return json_api_send(req, error, 400);
    }
    cJSON *topic = cJSON_GetObjectItem(json, "topic");
    cJSON *qos = cJSON_GetObjectItem(json, "qos");
    const int message_id = network_services_mqtt_subscribe(
        topic && cJSON_IsString(topic) ? topic->valuestring : NULL,
        qos && cJSON_IsNumber(qos) ? qos->valueint : 0);
    cJSON_Delete(json);
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", message_id >= 0);
    cJSON_AddNumberToObject(response, "message_id", message_id);
    return json_api_send(req, response, message_id >= 0 ? 200 : 400);
}

static const httpd_uri_t s_mqtt_uris[] = {
    {.uri = "/api/v1/mqtt/config", .method = HTTP_POST, .handler = api_mqtt_config_handler},
    {.uri = "/api/v1/mqtt/connect", .method = HTTP_POST, .handler = api_mqtt_connect_handler},
    {.uri = "/api/v1/mqtt/disconnect", .method = HTTP_POST, .handler = api_mqtt_disconnect_handler},
    {.uri = "/api/v1/mqtt/publish", .method = HTTP_POST, .handler = api_mqtt_publish_handler},
    {.uri = "/api/v1/mqtt/subscribe", .method = HTTP_POST, .handler = api_mqtt_subscribe_handler},
    {.uri = "/api/v1/mqtt/config", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/mqtt/connect", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/mqtt/disconnect", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/mqtt/publish", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/mqtt/subscribe", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
};

const httpd_uri_t *json_api_mqtt_uris(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(s_mqtt_uris) / sizeof(s_mqtt_uris[0]);
    }
    return s_mqtt_uris;
}
