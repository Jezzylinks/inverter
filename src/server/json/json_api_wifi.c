#include "server/json/json_api_wifi.h"
#include "server/json/json_api_server.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "cJSON.h"

#include "wifi/wifi_manager.h"
#include "wifi/wifi_scan.h"
#include "wifi/wifi_storage.h"
#include "wifi/wifi_events.h"
#include "wifi/wifi_monitor.h"
#include "wifi/wifi_controller.h"
#include "wifi/wifi_config.h"
#include "system/inverter_errors.h"
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

static const char *wifi_auth_name(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA3_PSK: return "wpa3";
    default: return "other";
    }
}

static bool parse_wifi_mode(const char *value, wifi_mode_t *mode)
{
    if (value == NULL || mode == NULL) return false;
    if (strcmp(value, "sta") == 0) *mode = WIFI_MODE_STA;
    else if (strcmp(value, "ap") == 0) *mode = WIFI_MODE_AP;
    else if (strcmp(value, "apsta") == 0) *mode = WIFI_MODE_APSTA;
    else return false;
    return true;
}

static bool parse_wifi_auth(const char *value, wifi_auth_mode_t *authmode)
{
    if (value == NULL || authmode == NULL) return false;
    if (strcmp(value, "open") == 0) *authmode = WIFI_AUTH_OPEN;
    else if (strcmp(value, "wpa2") == 0) *authmode = WIFI_AUTH_WPA2_PSK;
    else if (strcmp(value, "wpa3") == 0) *authmode = WIFI_AUTH_WPA3_PSK;
    else return false;
    return true;
}

static bool parse_ipv4_field(const cJSON *root, const char *name, esp_ip4_addr_t *target)
{
    const cJSON *item = cJSON_GetObjectItem(root, name);
    if (item == NULL) return true;
    return cJSON_IsString(item) && item->valuestring != NULL &&
           esp_netif_str_to_ip4(item->valuestring, target) == ESP_OK;
}

static void add_ipv4_field(cJSON *root, const char *name, const esp_ip4_addr_t *value)
{
    char text[16] = {0};
    snprintf(text, sizeof(text), IPSTR, IP2STR(value));
    cJSON_AddStringToObject(root, name, text);
}

static void add_config_json(cJSON *root, const wifi_manager_config_t *config)
{
    cJSON_AddStringToObject(root, "mode", wifi_mode_name(config->mode));
    cJSON_AddStringToObject(root, "ssid", config->ssid);
    cJSON_AddStringToObject(root, "auth", wifi_auth_name(config->authmode));
    cJSON_AddBoolToObject(root, "dhcp", config->dhcp);
    cJSON_AddBoolToObject(root, "auto_reconnect", config->auto_reconnect);
    cJSON_AddNumberToObject(root, "reconnect_interval_ms", config->reconnect_interval_ms);
    add_ipv4_field(root, "ip", &config->ip_info.ip);
    add_ipv4_field(root, "gateway", &config->ip_info.gw);
    add_ipv4_field(root, "netmask", &config->ip_info.netmask);
    add_ipv4_field(root, "dns", &config->dns);
    cJSON_AddStringToObject(root, "ap_ssid", config->ap_ssid);
    cJSON_AddStringToObject(root, "ap_auth", wifi_auth_name(config->ap_authmode));
    cJSON_AddNumberToObject(root, "ap_channel", config->ap_channel);
    cJSON_AddNumberToObject(root, "ap_max_connection", config->ap_max_connection);
    cJSON_AddStringToObject(root, "hostname", WIFI_HOSTNAME);
    cJSON_AddBoolToObject(root, "requires_restart", true);
}

static bool apply_string_field(const cJSON *root, const char *name, char *target, size_t target_size)
{
    const cJSON *item = cJSON_GetObjectItem(root, name);
    if (item == NULL) return true;
    if (!cJSON_IsString(item) || item->valuestring == NULL || strlen(item->valuestring) >= target_size) return false;
    memset(target, 0, target_size);
    strncpy(target, item->valuestring, target_size - 1U);
    return true;
}

static bool apply_bool_field(const cJSON *root, const char *name, bool *target)
{
    const cJSON *item = cJSON_GetObjectItem(root, name);
    if (item == NULL) return true;
    if (!cJSON_IsBool(item)) return false;
    *target = cJSON_IsTrue(item);
    return true;
}

static bool apply_uint_field(const cJSON *root, const char *name, uint32_t *target, uint32_t min, uint32_t max)
{
    const cJSON *item = cJSON_GetObjectItem(root, name);
    if (item == NULL) return true;
    if (!cJSON_IsNumber(item) || item->valuedouble < min || item->valuedouble > max ||
        (uint32_t)item->valuedouble != item->valuedouble) return false;
    *target = (uint32_t)item->valuedouble;
    return true;
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
    if (root == NULL) return ESP_ERR_NO_MEM;
    add_config_json(root, &config);

    return json_api_send(req, root, 200);
}

/* POST /api/v1/wifi/config persists supported non-credential settings.
 * Station credentials remain on the existing /wifi/connect workflow and are
 * deliberately never included in configuration responses. */
static esp_err_t api_config_update_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    if (req->content_len == 0 || req->content_len > 768) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Invalid configuration payload length");
        return json_api_send(req, error, 400);
    }

    char *body = calloc(1U, req->content_len + 1U);
    if (body == NULL) return ESP_ERR_NO_MEM;
    const int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        free(body);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Could not read configuration payload");
        return json_api_send(req, error, 400);
    }
    cJSON *input = cJSON_Parse(body);
    free(body);
    if (input == NULL || !cJSON_IsObject(input)) {
        cJSON_Delete(input);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Configuration payload must be a JSON object");
        return json_api_send(req, error, 400);
    }

    wifi_manager_config_t config = {0};
    esp_err_t err = wifi_controller_get_config(&config);
    bool valid = err == ESP_OK;
    const cJSON *mode = cJSON_GetObjectItem(input, "mode");
    const cJSON *ap_auth = cJSON_GetObjectItem(input, "ap_auth");
    uint32_t ap_channel = config.ap_channel;
    uint32_t ap_max_connection = config.ap_max_connection;
    valid = valid && (mode == NULL || (cJSON_IsString(mode) && parse_wifi_mode(mode->valuestring, &config.mode)));
    valid = valid && (ap_auth == NULL || (cJSON_IsString(ap_auth) && parse_wifi_auth(ap_auth->valuestring, &config.ap_authmode)));
    valid = valid && apply_bool_field(input, "dhcp", &config.dhcp);
    valid = valid && apply_bool_field(input, "auto_reconnect", &config.auto_reconnect);
    valid = valid && apply_uint_field(input, "reconnect_interval_ms", &config.reconnect_interval_ms, 250U, 60000U);
    valid = valid && apply_uint_field(input, "ap_channel", &ap_channel, 1U, 13U);
    valid = valid && apply_uint_field(input, "ap_max_connection", &ap_max_connection, 1U, 10U);
    config.ap_channel = (uint8_t)ap_channel;
    config.ap_max_connection = (uint8_t)ap_max_connection;
    valid = valid && apply_string_field(input, "ap_ssid", config.ap_ssid, sizeof(config.ap_ssid));
    valid = valid && apply_string_field(input, "ap_password", config.ap_password, sizeof(config.ap_password));
    valid = valid && parse_ipv4_field(input, "ip", &config.ip_info.ip);
    valid = valid && parse_ipv4_field(input, "gateway", &config.ip_info.gw);
    valid = valid && parse_ipv4_field(input, "netmask", &config.ip_info.netmask);
    valid = valid && parse_ipv4_field(input, "dns", &config.dns);
    cJSON_Delete(input);
    if (!valid) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Invalid or unsupported Wi-Fi configuration value");
        return json_api_send(req, error, 400);
    }

    err = wifi_controller_set_config(&config);
    if (err == ESP_OK) {
        wifi_network_config_t stored = {0};
        stored.mode = config.mode;
        stored.auto_reconnect = config.auto_reconnect;
        stored.reconnect_interval_ms = config.reconnect_interval_ms;
        stored.dhcp = config.dhcp;
        stored.ip_info = config.ip_info;
        stored.dns = config.dns;
        stored.ap_channel = config.ap_channel;
        stored.ap_max_connection = config.ap_max_connection;
        stored.ap_authmode = config.ap_authmode;
        strncpy(stored.ap_ssid, config.ap_ssid, sizeof(stored.ap_ssid) - 1U);
        strncpy(stored.ap_password, config.ap_password, sizeof(stored.ap_password) - 1U);
        err = wifi_storage_save_network_config(&stored);
    }
    if (err != ESP_OK) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", esp_err_to_name(err));
        return json_api_send(req, error, 500);
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", true);
    cJSON_AddStringToObject(result, "status", "requires_restart");
    cJSON_AddStringToObject(result, "message", "Wi-Fi settings saved. Restart or reconnect the inverter to apply mode and IP changes.");
    add_config_json(result, &config);
    return json_api_send(req, result, 202);
}

static const httpd_uri_t s_wifi_uris[] = {
    {.uri = "/api/v1/wifi", .method = HTTP_GET, .handler = api_status_handler},
    {.uri = "/api/v1/wifi/scan", .method = HTTP_GET, .handler = api_scan_handler},
    {.uri = "/api/v1/wifi/connect", .method = HTTP_POST, .handler = api_connect_handler},
    {.uri = "/api/v1/wifi/disconnect", .method = HTTP_POST, .handler = api_disconnect_handler},
    {.uri = "/api/v1/wifi/reset", .method = HTTP_POST, .handler = api_reset_handler},
    {.uri = "/api/v1/wifi/config", .method = HTTP_GET, .handler = api_config_handler},
    {.uri = "/api/v1/wifi/config", .method = HTTP_POST, .handler = api_config_update_handler},
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
