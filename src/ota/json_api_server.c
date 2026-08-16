/**
 * @file json_api_server.c
 * @brief JSON REST API for WiFi status and control
 */

#include "json_api_server.h"
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "wifi/wifi_manager.h"
#include "wifi/wifi_scan.h"
#include "wifi/wifi_storage.h"
#include "wifi/wifi_events.h"
#include "wifi/wifi_monitor.h"
#include "wifi/wifi_controller.h"
#include "wifi/wifi_config.h"
#include "security/security.h"
#include "system_state.h"
#include "network_services.h"
#include "inverter_errors.h"

static const char *TAG = "JSON_API";

static httpd_handle_t s_server = NULL;
extern system_state_t sys_state;

/*----------------------------------------------------------
 * CORS Headers
 *---------------------------------------------------------*/
static void add_cors_headers(httpd_req_t *req)
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

static esp_err_t send_json(httpd_req_t *req, cJSON *root, int status_code)
{
    add_cors_headers(req);
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

static esp_err_t api_require_pin(httpd_req_t *req);

static esp_err_t api_options_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t api_receive_json(httpd_req_t *req, cJSON **json)
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
    esp_err_t auth_err = api_require_pin(req);
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
    cJSON_AddBoolToObject(root, "mqtt_configured", status.mqtt_configured);
    cJSON_AddBoolToObject(root, "mqtt_connected", status.mqtt_connected);
    api_add_mqtt_config(root, &config);
    return send_json(req, root, 200);
}

static esp_err_t api_mqtt_config_handler(httpd_req_t *req)
{
    esp_err_t auth_err = api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    cJSON *json = NULL;
    esp_err_t err = api_receive_json(req, &json);
    if (err != ESP_OK) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Invalid JSON body");
        return send_json(req, error, err == ESP_ERR_NO_MEM ? 503 : 400);
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
    return send_json(req, response, err == ESP_OK ? 200 : 400);
}

static esp_err_t api_mqtt_connect_handler(httpd_req_t *req)
{
    esp_err_t auth_err = api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    const esp_err_t err = network_services_mqtt_connect();
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddBoolToObject(response, "pending", err == ESP_OK);
    cJSON_AddStringToObject(response, "message", err == ESP_OK ? "MQTT connection requested" : esp_err_to_name(err));
    return send_json(req, response, err == ESP_OK ? 202 : 400);
}

static esp_err_t api_mqtt_disconnect_handler(httpd_req_t *req)
{
    esp_err_t auth_err = api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    const esp_err_t err = network_services_mqtt_disconnect();
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message", err == ESP_OK ? "MQTT disconnected" : esp_err_to_name(err));
    return send_json(req, response, err == ESP_OK ? 200 : 400);
}

static esp_err_t api_mqtt_publish_handler(httpd_req_t *req)
{
    esp_err_t auth_err = api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    cJSON *json = NULL;
    esp_err_t err = api_receive_json(req, &json);
    if (err != ESP_OK) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Invalid JSON body");
        return send_json(req, error, 400);
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
    return send_json(req, response, message_id >= 0 ? 200 : 400);
}

static esp_err_t api_mqtt_subscribe_handler(httpd_req_t *req)
{
    esp_err_t auth_err = api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    cJSON *json = NULL;
    esp_err_t err = api_receive_json(req, &json);
    if (err != ESP_OK) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Invalid JSON body");
        return send_json(req, error, 400);
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
    return send_json(req, response, message_id >= 0 ? 200 : 400);
}

static esp_err_t api_require_pin(httpd_req_t *req)
{
    if (!req || !sys_state.security.enabled || req->method == HTTP_OPTIONS) {
        return ESP_OK;
    }
    const size_t header_len = httpd_req_get_hdr_value_len(req, "X-Inverter-PIN");
    if (header_len != SECURITY_PIN_LEN) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "PIN required");
        return send_json(req, error, 401);
    }
    char pin_text[SECURITY_PIN_LEN + 1U] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Inverter-PIN", pin_text,
                                    sizeof(pin_text)) != ESP_OK) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Invalid PIN header");
        return send_json(req, error, 401);
    }
    uint8_t pin[SECURITY_PIN_LEN] = {0};
    for (size_t i = 0; i < SECURITY_PIN_LEN; ++i) {
        if (pin_text[i] < '0' || pin_text[i] > '9') {
            cJSON *error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "Invalid PIN");
            return send_json(req, error, 401);
        }
        pin[i] = (uint8_t)(pin_text[i] - '0');
    }
    if (!security_verify_pin(pin)) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Unauthorized");
        return send_json(req, error, 401);
    }
    return ESP_OK;
}

/*----------------------------------------------------------
 * GET /api/v1/status
 *---------------------------------------------------------*/
static esp_err_t api_status_handler(httpd_req_t *req)
{
    esp_err_t auth_err = api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    cJSON *root = cJSON_CreateObject();

    wifi_status_t status_copy = {0};
    const bool have_status = wifi_events_get_status_copy(&status_copy) == ESP_OK;
    if (have_status)
    {
        const wifi_status_t *status = &status_copy;
        const char *state_str = "unknown";
        switch (status->state)
        {
        case WIFI_STATE_CONNECTED:
            state_str = "connected";
            break;
        case WIFI_STATE_CONNECTING:
            state_str = "connecting";
            break;
        case WIFI_STATE_DISCONNECTED:
            state_str = "disconnected";
            break;
        case WIFI_STATE_FAILED:
            state_str = "failed";
            break;
        case WIFI_STATE_RECONNECTING:
            state_str = "reconnecting";
            break;
        case WIFI_STATE_IDLE:
            state_str = "idle";
            break;
        default:
            break;
        }

        cJSON_AddStringToObject(root, "state", state_str);
        cJSON_AddBoolToObject(root, "connected", status->connected);
        cJSON_AddBoolToObject(root, "got_ip", status->got_ip);
        cJSON_AddBoolToObject(root, "internet_available", status->internet_available);
        cJSON_AddNumberToObject(root, "retry_count", status->retry_count);
        cJSON_AddNumberToObject(root, "rssi", status->rssi);

        char ip_str[16], gw_str[16], mask_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&status->ip));
        snprintf(gw_str, sizeof(gw_str), IPSTR, IP2STR(&status->gateway));
        snprintf(mask_str, sizeof(mask_str), IPSTR, IP2STR(&status->netmask));

        cJSON_AddStringToObject(root, "ip", ip_str);
        cJSON_AddStringToObject(root, "gateway", gw_str);
        cJSON_AddStringToObject(root, "netmask", mask_str);
    }
    else
    {
        cJSON_AddStringToObject(root, "state", "unknown");
    }

    wifi_internet_status_t internet = wifi_monitor_get_internet_status();
    const char *internet_str = "unknown";
    switch (internet)
    {
    case WIFI_INTERNET_AVAILABLE:
        internet_str = "available";
        break;
    case WIFI_INTERNET_UNAVAILABLE:
        internet_str = "unavailable";
        break;
    case WIFI_INTERNET_UNKNOWN:
        internet_str = "unknown";
        break;
    }
    cJSON_AddStringToObject(root, "internet", internet_str);

    int8_t rssi = wifi_monitor_get_rssi();
    cJSON_AddNumberToObject(root, "monitor_rssi", rssi);

    network_services_status_t services;
    network_services_get_status(&services);
    cJSON_AddBoolToObject(root, "http_service", services.http_running);
    cJSON_AddBoolToObject(root, "dashboard_service", services.dashboard_running);
    cJSON_AddBoolToObject(root, "websocket_service", services.websocket_running);
    cJSON_AddBoolToObject(root, "mdns_service", services.mdns_running);
    cJSON_AddBoolToObject(root, "mqtt_configured", services.mqtt_configured);
    cJSON_AddBoolToObject(root, "mqtt_connected", services.mqtt_connected);
    const inverter_start_error_code_t start_error = inverter_get_last_start_error_code();
    cJSON_AddNumberToObject(root, "start_error_code", (unsigned)start_error);
    char start_error_text[16];
    snprintf(start_error_text, sizeof(start_error_text), "E%03X",
             (unsigned)start_error & 0x0FFFU);
    cJSON_AddStringToObject(root, "start_error", start_error_text);
    cJSON_AddStringToObject(root, "start_error_reason",
                            inverter_get_last_start_error_reason());

    return send_json(req, root, 200);
}

/*----------------------------------------------------------
 * GET /api/v1/scan
 *---------------------------------------------------------*/
static esp_err_t api_scan_handler(httpd_req_t *req)
{
#if !WIFI_RUNTIME_PROVISIONING_ENABLED
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "error", "Runtime Wi-Fi scanning is disabled; use menuconfig credentials");
    return send_json(req, error, 403);
#else
    esp_err_t auth_err = api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();

    uint16_t ap_count = WIFI_MAX_SCAN_RESULTS;
    wifi_ap_record_t *records = calloc(ap_count, sizeof(*records));
    if (records == NULL) {
        cJSON_Delete(networks);
        cJSON_Delete(root);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Out of memory");
        return send_json(req, error, 503);
    }

    const esp_err_t scan_err = wifi_scan_start_records(records, &ap_count);
    if (scan_err != ESP_OK) {
        free(records);
        cJSON_Delete(networks);
        cJSON_Delete(root);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", esp_err_to_name(scan_err));
        return send_json(req, error, 503);
    }

    for (uint16_t i = 0U; i < ap_count; ++i)
    {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (char *)records[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(ap, "channel", records[i].primary);
        cJSON_AddStringToObject(ap, "auth",
                                records[i].authmode == WIFI_AUTH_OPEN ? "open" : records[i].authmode == WIFI_AUTH_WPA2_PSK ? "wpa2"
                                                                             : records[i].authmode == WIFI_AUTH_WPA3_PSK ? "wpa3" : "other");
        cJSON_AddItemToArray(networks, ap);
    }
    free(records);

    cJSON_AddItemToObject(root, "networks", networks);
    cJSON_AddNumberToObject(root, "count", ap_count);
    return send_json(req, root, 200);
#endif
}

/*----------------------------------------------------------
 * POST /api/v1/connect
 * Body: {"ssid":"...","password":"..."}
 *---------------------------------------------------------*/
static esp_err_t api_connect_handler(httpd_req_t *req)
{
#if !WIFI_RUNTIME_PROVISIONING_ENABLED
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "error", "Runtime Wi-Fi credentials are disabled; use menuconfig");
    return send_json(req, error, 403);
#else
    if (req->method == HTTP_OPTIONS)
    {
        add_cors_headers(req);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    esp_err_t auth_err = api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;

    if (req->method != HTTP_POST)
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Method not allowed");
        return send_json(req, err, 405);
    }

    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > 512)
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Invalid content length");
        return send_json(req, err, 400);
    }

    char *buf = malloc(content_len + 1);
    if (buf == NULL)
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Out of memory");
        return send_json(req, err, 500);
    }

    int ret = httpd_req_recv(req, buf, content_len);
    if (ret <= 0)
    {
        free(buf);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "No data received");
        return send_json(req, err, 400);
    }
    buf[ret] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);

    if (json == NULL)
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Invalid JSON");
        return send_json(req, err, 400);
    }

    cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(json, "password");

    if (ssid_item == NULL || !cJSON_IsString(ssid_item))
    {
        cJSON_Delete(json);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Missing or invalid SSID");
        return send_json(req, err, 400);
    }

    const char *ssid = cJSON_GetStringValue(ssid_item);
    const char *password = pass_item && cJSON_IsString(pass_item) ? cJSON_GetStringValue(pass_item) : "";

    if (strlen(ssid) == 0 || strlen(ssid) > 32)
    {
        cJSON_Delete(json);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Invalid SSID length");
        return send_json(req, err, 400);
    }

    if (strlen(password) > 64)
    {
        cJSON_Delete(json);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Password too long");
        return send_json(req, err, 400);
    }

    wifi_credentials_t credentials;
    memset(&credentials, 0, sizeof(credentials));
    strncpy(credentials.ssid, ssid, sizeof(credentials.ssid) - 1);
    strncpy(credentials.password, password, sizeof(credentials.password) - 1);

    esp_err_t err = wifi_storage_save_credentials(&credentials);
    if (err != ESP_OK)
    {
        cJSON_Delete(json);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "success", false);
        cJSON_AddStringToObject(resp, "error", "Failed to save credentials");
        return send_json(req, resp, 500);
    }

    cJSON_Delete(json);

    err = wifi_controller_start();
    if (err != ESP_OK) {
        err = wifi_controller_reconnect();
    }

    const bool accepted = err == ESP_OK || err == ESP_ERR_WIFI_CONN;
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", accepted);
    cJSON_AddBoolToObject(resp, "pending", accepted);
    cJSON_AddStringToObject(resp, "state", accepted ? "connecting" : "failed");
    cJSON_AddStringToObject(resp, "message",
                            accepted ? "Connection request accepted; poll /api/v1/status"
                                     : esp_err_to_name(err));

    return send_json(req, resp, accepted ? 202 : 500);
#endif
}

/*----------------------------------------------------------
 * POST /api/v1/disconnect
 *---------------------------------------------------------*/
static esp_err_t api_disconnect_handler(httpd_req_t *req)
{
    if (req->method == HTTP_OPTIONS)
    {
        add_cors_headers(req);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    esp_err_t auth_err = api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    esp_err_t err = wifi_controller_disconnect();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", err == ESP_OK);
    cJSON_AddStringToObject(resp, "message", err == ESP_OK ? "Disconnected" : esp_err_to_name(err));

    return send_json(req, resp, err == ESP_OK ? 200 : 500);
}

/*----------------------------------------------------------
 * POST /api/v1/reset
 *---------------------------------------------------------*/
static esp_err_t api_reset_handler(httpd_req_t *req)
{
#if !WIFI_RUNTIME_PROVISIONING_ENABLED
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "error", "Runtime Wi-Fi credential reset is disabled");
    return send_json(req, error, 403);
#else
    if (req->method == HTTP_OPTIONS)
    {
        add_cors_headers(req);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    esp_err_t auth_err = api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;

    if (req->method != HTTP_POST)
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Method not allowed");
        return send_json(req, err, 405);
    }

    wifi_controller_stop();
    esp_err_t err = wifi_storage_erase_credentials();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", err == ESP_OK);
    cJSON_AddStringToObject(resp, "message", err == ESP_OK ? "Credentials erased" : esp_err_to_name(err));

    return send_json(req, resp, err == ESP_OK ? 200 : 500);
#endif
}

/*----------------------------------------------------------
 * GET /api/v1/config
 *---------------------------------------------------------*/
static esp_err_t api_config_handler(httpd_req_t *req)
{
    esp_err_t auth_err = api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    wifi_manager_config_t config;
    memset(&config, 0, sizeof(config));

    esp_err_t err = wifi_controller_get_config(&config);
    if (err != ESP_OK) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", esp_err_to_name(err));
        return send_json(req, error, 500);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "dhcp", config.dhcp);
    cJSON_AddBoolToObject(root, "auto_reconnect", config.auto_reconnect);
    cJSON_AddNumberToObject(root, "reconnect_interval_ms", config.reconnect_interval_ms);
    cJSON_AddNumberToObject(root, "ap_channel", config.ap_channel);
    cJSON_AddNumberToObject(root, "ap_max_connection", config.ap_max_connection);
    cJSON_AddStringToObject(root, "hostname", WIFI_HOSTNAME);

    return send_json(req, root, 200);
}

/*----------------------------------------------------------
 * URI Table
 *---------------------------------------------------------*/
static const httpd_uri_t uri_status = {
    .uri = "/api/v1/status",
    .method = HTTP_GET,
    .handler = api_status_handler,
};

static const httpd_uri_t uri_scan = {
    .uri = "/api/v1/scan",
    .method = HTTP_GET,
    .handler = api_scan_handler,
};

static const httpd_uri_t uri_connect = {
    .uri = "/api/v1/connect",
    .method = HTTP_POST,
    .handler = api_connect_handler,
};

static const httpd_uri_t uri_disconnect = {
    .uri = "/api/v1/disconnect",
    .method = HTTP_POST,
    .handler = api_disconnect_handler,
};

static const httpd_uri_t uri_reset = {
    .uri = "/api/v1/reset",
    .method = HTTP_POST,
    .handler = api_reset_handler,
};

static const httpd_uri_t uri_config = {
    .uri = "/api/v1/config",
    .method = HTTP_GET,
    .handler = api_config_handler,
};

static const httpd_uri_t uri_services = {
    .uri = "/api/v1/services",
    .method = HTTP_GET,
    .handler = api_services_handler,
};

static const httpd_uri_t uri_mqtt_config = {
    .uri = "/api/v1/mqtt/config",
    .method = HTTP_POST,
    .handler = api_mqtt_config_handler,
};

static const httpd_uri_t uri_mqtt_connect = {
    .uri = "/api/v1/mqtt/connect",
    .method = HTTP_POST,
    .handler = api_mqtt_connect_handler,
};

static const httpd_uri_t uri_mqtt_disconnect = {
    .uri = "/api/v1/mqtt/disconnect",
    .method = HTTP_POST,
    .handler = api_mqtt_disconnect_handler,
};

static const httpd_uri_t uri_mqtt_publish = {
    .uri = "/api/v1/mqtt/publish",
    .method = HTTP_POST,
    .handler = api_mqtt_publish_handler,
};

static const httpd_uri_t uri_mqtt_subscribe = {
    .uri = "/api/v1/mqtt/subscribe",
    .method = HTTP_POST,
    .handler = api_mqtt_subscribe_handler,
};

#define API_OPTIONS_URI(name, path) \
    static const httpd_uri_t name = { \
        .uri = path, .method = HTTP_OPTIONS, .handler = api_options_handler \
    }

API_OPTIONS_URI(uri_connect_options, "/api/v1/connect");
API_OPTIONS_URI(uri_disconnect_options, "/api/v1/disconnect");
API_OPTIONS_URI(uri_reset_options, "/api/v1/reset");
API_OPTIONS_URI(uri_mqtt_config_options, "/api/v1/mqtt/config");
API_OPTIONS_URI(uri_mqtt_connect_options, "/api/v1/mqtt/connect");
API_OPTIONS_URI(uri_mqtt_disconnect_options, "/api/v1/mqtt/disconnect");
API_OPTIONS_URI(uri_mqtt_publish_options, "/api/v1/mqtt/publish");
API_OPTIONS_URI(uri_mqtt_subscribe_options, "/api/v1/mqtt/subscribe");

/*----------------------------------------------------------
 * Start / Stop
 *---------------------------------------------------------*/
esp_err_t json_api_server_start(httpd_handle_t server)
{
    if (server == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_server = server;

    const httpd_uri_t *uris[] = {
        &uri_status, &uri_scan, &uri_connect, &uri_disconnect, &uri_reset,
        &uri_config, &uri_services, &uri_mqtt_config, &uri_mqtt_connect,
        &uri_mqtt_disconnect, &uri_mqtt_publish, &uri_mqtt_subscribe,
        &uri_connect_options, &uri_disconnect_options, &uri_reset_options,
        &uri_mqtt_config_options, &uri_mqtt_connect_options,
        &uri_mqtt_disconnect_options, &uri_mqtt_publish_options,
        &uri_mqtt_subscribe_options, NULL};

    for (const httpd_uri_t **uri = uris; *uri != NULL; uri++)
    {
        esp_err_t err = httpd_register_uri_handler(s_server, *uri);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to register URI %s: %s",
                     (*uri)->uri, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "JSON API server registered");

    return ESP_OK;
}

esp_err_t json_api_server_stop(void)
{
    if (s_server == NULL)
    {
        return ESP_OK;
    }

    httpd_unregister_uri_handler(s_server, "/api/v1/status", HTTP_GET);
    httpd_unregister_uri_handler(s_server, "/api/v1/scan", HTTP_GET);
    httpd_unregister_uri_handler(s_server, "/api/v1/connect", HTTP_POST);
    httpd_unregister_uri_handler(s_server, "/api/v1/disconnect", HTTP_POST);
    httpd_unregister_uri_handler(s_server, "/api/v1/reset", HTTP_POST);
    httpd_unregister_uri_handler(s_server, "/api/v1/config", HTTP_GET);
    httpd_unregister_uri_handler(s_server, "/api/v1/services", HTTP_GET);
    httpd_unregister_uri_handler(s_server, "/api/v1/mqtt/config", HTTP_POST);
    httpd_unregister_uri_handler(s_server, "/api/v1/mqtt/connect", HTTP_POST);
    httpd_unregister_uri_handler(s_server, "/api/v1/mqtt/disconnect", HTTP_POST);
    httpd_unregister_uri_handler(s_server, "/api/v1/mqtt/publish", HTTP_POST);
    httpd_unregister_uri_handler(s_server, "/api/v1/mqtt/subscribe", HTTP_POST);
    httpd_unregister_uri_handler(s_server, "/api/v1/connect", HTTP_OPTIONS);
    httpd_unregister_uri_handler(s_server, "/api/v1/disconnect", HTTP_OPTIONS);
    httpd_unregister_uri_handler(s_server, "/api/v1/reset", HTTP_OPTIONS);
    httpd_unregister_uri_handler(s_server, "/api/v1/mqtt/config", HTTP_OPTIONS);
    httpd_unregister_uri_handler(s_server, "/api/v1/mqtt/connect", HTTP_OPTIONS);
    httpd_unregister_uri_handler(s_server, "/api/v1/mqtt/disconnect", HTTP_OPTIONS);
    httpd_unregister_uri_handler(s_server, "/api/v1/mqtt/publish", HTTP_OPTIONS);
    httpd_unregister_uri_handler(s_server, "/api/v1/mqtt/subscribe", HTTP_OPTIONS);

    s_server = NULL;

    return ESP_OK;
}