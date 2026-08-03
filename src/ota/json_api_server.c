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

#include "wifi_manager.h"
#include "wifi_scan.h"
#include "wifi_storage.h"
#include "wifi_events.h"
#include "wifi_monitor.h"
#include "wifi_controller.h"
#include "wifi_config.h"

static const char *TAG = "JSON_API";

static httpd_handle_t s_server = NULL;

/*----------------------------------------------------------
 * CORS Headers
 *---------------------------------------------------------*/
static void add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_type(req, "application/json");
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

    char status_str[32];
    snprintf(status_str, sizeof(status_str), "%d", status_code);
    httpd_resp_set_status(req, status_code == 200 ? "200 OK" : status_str);
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/*----------------------------------------------------------
 * GET /api/v1/status
 *---------------------------------------------------------*/
static esp_err_t api_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    const wifi_status_t *status = wifi_manager_get_status();
    if (status)
    {
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

    return send_json(req, root, 200);
}

/*----------------------------------------------------------
 * GET /api/v1/scan
 *---------------------------------------------------------*/
static esp_err_t api_scan_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count > 0)
    {
        if (ap_count > WIFI_MAX_SCAN_RESULTS)
            ap_count = WIFI_MAX_SCAN_RESULTS;

        wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (records)
        {
            esp_wifi_scan_get_ap_records(&ap_count, records);

            for (int i = 0; i < ap_count; i++)
            {
                cJSON *ap = cJSON_CreateObject();
                cJSON_AddStringToObject(ap, "ssid", (char *)records[i].ssid);
                cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);
                cJSON_AddNumberToObject(ap, "channel", records[i].primary);
                cJSON_AddStringToObject(ap, "auth",
                                        records[i].authmode == WIFI_AUTH_OPEN ? "open" : records[i].authmode == WIFI_AUTH_WPA2_PSK ? "wpa2"
                                                                                     : records[i].authmode == WIFI_AUTH_WPA3_PSK   ? "wpa3"
                                                                                                                                   : "other");
                cJSON_AddItemToArray(networks, ap);
            }

            free(records);
        }
    }

    cJSON_AddItemToObject(root, "networks", networks);
    cJSON_AddNumberToObject(root, "count", ap_count);

    return send_json(req, root, 200);
}

/*----------------------------------------------------------
 * POST /api/v1/connect
 * Body: {"ssid":"...","password":"..."}
 *---------------------------------------------------------*/
static esp_err_t api_connect_handler(httpd_req_t *req)
{
    if (req->method == HTTP_OPTIONS)
    {
        add_cors_headers(req);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

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

    err = wifi_manager_connect();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", err == ESP_OK);
    cJSON_AddStringToObject(resp, "message", err == ESP_OK ? "Connecting..." : esp_err_to_name(err));

    return send_json(req, resp, err == ESP_OK ? 200 : 500);
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

    esp_err_t err = wifi_manager_disconnect();

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
    if (req->method == HTTP_OPTIONS)
    {
        add_cors_headers(req);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    if (req->method != HTTP_POST)
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Method not allowed");
        return send_json(req, err, 405);
    }

    esp_err_t err = wifi_storage_erase_credentials();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", err == ESP_OK);
    cJSON_AddStringToObject(resp, "message", err == ESP_OK ? "Credentials erased" : esp_err_to_name(err));

    return send_json(req, resp, err == ESP_OK ? 200 : 500);
}

/*----------------------------------------------------------
 * GET /api/v1/config
 *---------------------------------------------------------*/
static esp_err_t api_config_handler(httpd_req_t *req)
{
    wifi_manager_config_t config;
    memset(&config, 0, sizeof(config));

    esp_err_t err = wifi_manager_get_config(&config);

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
        &uri_status, &uri_scan, &uri_connect,
        &uri_disconnect, &uri_reset, &uri_config,
        NULL};

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

    s_server = NULL;

    return ESP_OK;
}