#include "server/json/json_api_system.h"
#include "server/json/json_api_server.h"

#include "esp_http_server.h"
#include "cJSON.h"

#include "server/network_services.h"
#include "events/fault_log.h"
#include "diagnostics/system_diagnostics.h"
#include <math.h>

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



#define API_FAULT_HISTORY_LIMIT 10U

static esp_err_t api_fault_history_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    fault_log_entry_t entries[API_FAULT_HISTORY_LIMIT] = {0};
    const uint32_t count = fault_log_get_entries(entries, API_FAULT_HISTORY_LIMIT);
    cJSON *root = cJSON_CreateObject();
    cJSON *events = cJSON_CreateArray();
    if (root == NULL || events == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(events);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddNumberToObject(root, "capacity", FAULT_LOG_CAPACITY);
    for (uint32_t i = 0U; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            cJSON_Delete(events);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddNumberToObject(item, "uptime_ms", entries[i].timestamp_ms);
        cJSON_AddStringToObject(item, "severity",
                                fault_log_severity_name(entries[i].severity));
        cJSON_AddStringToObject(item, "source",
                                fault_log_source_name(entries[i].source));
        cJSON_AddStringToObject(item, "message", entries[i].message);
        if (isfinite(entries[i].value)) {
            cJSON_AddNumberToObject(item, "value", entries[i].value);
        } else {
            cJSON_AddNullToObject(item, "value");
        }
        cJSON_AddItemToArray(events, item);
    }
    cJSON_AddItemToObject(root, "events", events);

    system_diagnostics_snapshot_t snapshot = {0};
    const bool diagnostics_available = system_diagnostics_get_snapshot(&snapshot);
    cJSON_AddBoolToObject(root, "diagnostics_available", diagnostics_available);
    if (diagnostics_available) {
        cJSON *diagnostics = cJSON_CreateObject();
        if (diagnostics == NULL) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddNumberToObject(diagnostics, "boot_count", snapshot.boot_count);
        cJSON_AddStringToObject(diagnostics, "last_reset_reason",
                                system_diagnostics_reset_reason_name(snapshot.last_reset_reason));
        cJSON_AddNumberToObject(diagnostics, "last_fault_flags", snapshot.last_fault_flags);
        cJSON_AddNumberToObject(diagnostics, "last_fault_timestamp_ms",
                                snapshot.last_fault_timestamp_ms);
        cJSON_AddNumberToObject(diagnostics, "last_uptime_seconds",
                                snapshot.last_uptime_seconds);
        if (isfinite(snapshot.last_battery_voltage) &&
            snapshot.last_battery_voltage > 0.0f) {
            cJSON_AddNumberToObject(diagnostics, "last_battery_voltage",
                                    snapshot.last_battery_voltage);
        } else {
            cJSON_AddNullToObject(diagnostics, "last_battery_voltage");
        }
        cJSON_AddItemToObject(root, "diagnostics", diagnostics);
    }

    return json_api_send(req, root, 200);
}

static const httpd_uri_t s_system_uris[] = {
    {.uri = "/api/v1/services", .method = HTTP_GET, .handler = api_services_handler},
    {.uri = "/api/v1/faults", .method = HTTP_GET, .handler = api_fault_history_handler},
    {.uri = "/api/v1/faults", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
};

const httpd_uri_t *json_api_system_uris(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(s_system_uris) / sizeof(s_system_uris[0]);
    }
    return s_system_uris;
}
