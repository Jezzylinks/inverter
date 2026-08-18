#include "json_api_wifi.h"
#include "json_api_server.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "cJSON.h"

#include "wifi/wifi_manager.h"
#include "wifi/wifi_scan.h"
#include "wifi/wifi_storage.h"
#include "wifi/wifi_events.h"
#include "wifi/wifi_monitor.h"
#include "wifi/wifi_controller.h"
#include "wifi/wifi_config.h"
#include "inverter_errors.h"
#include "server/network_services.h"

static const char *wifi_mode_name(wifi_mode_t mode)
{
    switch (mode) {
    case WIFI_MODE_STA: return "sta";
    case WIFI_MODE_AP: return "ap";
    case WIFI_MODE_APSTA: return "apsta";
    default: return "unknown";
    }
}

static esp_err_t api_status_handler(httpd_req_t *req)
{
    esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "mode", wifi_mode_name(wifi_manager_get_mode()));
    cJSON_AddBoolToObject(root, "runtime_provisioning_enabled",
                          WIFI_RUNTIME_PROVISIONING_ENABLED != 0);
    wifi_manager_config_t config = {0};
    if (wifi_controller_get_config(&config) == ESP_OK) {
        cJSON_AddStringToObject(root, "ssid", config.ssid);
    }

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

    return json_api_send(req, root, 200);
}

/*----------------------------------------------------------
 * GET /api/v1/scan
 *---------------------------------------------------------*/
static esp_err_t api_scan_handler(httpd_req_t *req)
{
#if !WIFI_RUNTIME_PROVISIONING_ENABLED
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "error", "Runtime Wi-Fi scanning is disabled; use menuconfig credentials");
    return json_api_send(req, error, 403);
#else
    esp_err_t auth_err = json_api_require_pin(req);
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
        return json_api_send(req, error, 503);
    }

    const esp_err_t scan_err = wifi_scan_start_records(records, &ap_count);
    if (scan_err != ESP_OK) {
        free(records);
        cJSON_Delete(networks);
        cJSON_Delete(root);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", esp_err_to_name(scan_err));
        return json_api_send(req, error, 503);
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
    return json_api_send(req, root, 200);
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
    return json_api_send(req, error, 403);
#else
    if (req->method == HTTP_OPTIONS)
    {
        json_api_add_cors_headers(req);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;

    if (req->method != HTTP_POST)
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Method not allowed");
        return json_api_send(req, err, 405);
    }

    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > 512)
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Invalid content length");
        return json_api_send(req, err, 400);
    }

    char *buf = malloc(content_len + 1);
    if (buf == NULL)
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Out of memory");
        return json_api_send(req, err, 500);
    }

    int ret = httpd_req_recv(req, buf, content_len);
    if (ret <= 0)
    {
        free(buf);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "No data received");
        return json_api_send(req, err, 400);
    }
    buf[ret] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);

    if (json == NULL)
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Invalid JSON");
        return json_api_send(req, err, 400);
    }

    cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(json, "password");

    if (ssid_item == NULL || !cJSON_IsString(ssid_item))
    {
        cJSON_Delete(json);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Missing or invalid SSID");
        return json_api_send(req, err, 400);
    }

    const char *ssid = cJSON_GetStringValue(ssid_item);
    const char *password = pass_item && cJSON_IsString(pass_item) ? cJSON_GetStringValue(pass_item) : "";

    if (strlen(ssid) == 0 || strlen(ssid) > 32)
    {
        cJSON_Delete(json);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Invalid SSID length");
        return json_api_send(req, err, 400);
    }

    if (strlen(password) > 64)
    {
        cJSON_Delete(json);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Password too long");
        return json_api_send(req, err, 400);
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
        return json_api_send(req, resp, 500);
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

    return json_api_send(req, resp, accepted ? 202 : 500);
#endif
}

/*----------------------------------------------------------
 * POST /api/v1/disconnect
 *---------------------------------------------------------*/
static esp_err_t api_disconnect_handler(httpd_req_t *req)
{
    if (req->method == HTTP_OPTIONS)
    {
        json_api_add_cors_headers(req);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    esp_err_t err = wifi_controller_disconnect();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", err == ESP_OK);
    cJSON_AddStringToObject(resp, "message", err == ESP_OK ? "Disconnected" : esp_err_to_name(err));

    return json_api_send(req, resp, err == ESP_OK ? 200 : 500);
}

/*----------------------------------------------------------
 * POST /api/v1/reset
 *---------------------------------------------------------*/
static esp_err_t api_reset_handler(httpd_req_t *req)
{
#if !WIFI_RUNTIME_PROVISIONING_ENABLED
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "error", "Runtime Wi-Fi credential reset is disabled");
    return json_api_send(req, error, 403);
#else
    if (req->method == HTTP_OPTIONS)
    {
        json_api_add_cors_headers(req);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;

    if (req->method != HTTP_POST)
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Method not allowed");
        return json_api_send(req, err, 405);
    }

    wifi_controller_stop();
    esp_err_t err = wifi_storage_erase_credentials();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", err == ESP_OK);
    cJSON_AddStringToObject(resp, "message", err == ESP_OK ? "Credentials erased" : esp_err_to_name(err));

    return json_api_send(req, resp, err == ESP_OK ? 200 : 500);
#endif
}

/*----------------------------------------------------------
 * GET /api/v1/config
 *---------------------------------------------------------*/
static esp_err_t api_config_handler(httpd_req_t *req)
{
    esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    wifi_manager_config_t config;
    memset(&config, 0, sizeof(config));

    esp_err_t err = wifi_controller_get_config(&config);
    if (err != ESP_OK) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", esp_err_to_name(err));
        return json_api_send(req, error, 500);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "dhcp", config.dhcp);
    cJSON_AddBoolToObject(root, "auto_reconnect", config.auto_reconnect);
    cJSON_AddNumberToObject(root, "reconnect_interval_ms", config.reconnect_interval_ms);
    cJSON_AddNumberToObject(root, "ap_channel", config.ap_channel);
    cJSON_AddNumberToObject(root, "ap_max_connection", config.ap_max_connection);
    cJSON_AddStringToObject(root, "hostname", WIFI_HOSTNAME);

    return json_api_send(req, root, 200);
}

static const httpd_uri_t s_wifi_uris[] = {
    {.uri = "/api/v1/wifi", .method = HTTP_GET, .handler = api_status_handler},
    {.uri = "/api/v1/wifi/scan", .method = HTTP_GET, .handler = api_scan_handler},
    {.uri = "/api/v1/wifi/connect", .method = HTTP_POST, .handler = api_connect_handler},
    {.uri = "/api/v1/wifi/disconnect", .method = HTTP_POST, .handler = api_disconnect_handler},
    {.uri = "/api/v1/wifi/reset", .method = HTTP_POST, .handler = api_reset_handler},
    {.uri = "/api/v1/wifi/config", .method = HTTP_GET, .handler = api_config_handler},
    {.uri = "/api/v1/wifi", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/wifi/scan", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/wifi/connect", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/wifi/disconnect", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/wifi/reset", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/wifi/config", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    /* Compatibility aliases retained for existing local clients. */
    {.uri = "/api/v1/scan", .method = HTTP_GET, .handler = api_scan_handler},
    {.uri = "/api/v1/connect", .method = HTTP_POST, .handler = api_connect_handler},
    {.uri = "/api/v1/disconnect", .method = HTTP_POST, .handler = api_disconnect_handler},
    {.uri = "/api/v1/reset", .method = HTTP_POST, .handler = api_reset_handler},
    {.uri = "/api/v1/config", .method = HTTP_GET, .handler = api_config_handler},
    {.uri = "/api/v1/connect", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/disconnect", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/reset", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
};

const httpd_uri_t *json_api_wifi_uris(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(s_wifi_uris) / sizeof(s_wifi_uris[0]);
    }
    return s_wifi_uris;
}
