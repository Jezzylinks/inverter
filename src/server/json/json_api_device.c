#include "json_api_device.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "firmware_version.h"
#include "hardware_config.h"
#include "inverter_power_status.h"
#include "inverter_errors.h"
#include "lcd_config.h"
#include "server/network_services.h"
#include "server/ntp/ntp_client.h"
#include "system_state.h"
#include "wifi/wifi_config.h"
#include "wifi/wifi_events.h"
#include "wifi/wifi_monitor.h"
#include "json_api_server.h"

extern system_state_t sys_state;
extern SemaphoreHandle_t sys_state_mutex;
extern void inverter_power_on(void);
extern void shutdown_inverter(void);

static const char *inverter_state_name(inverter_state_t state)
{
    switch (state) {
    case INVERTER_OFF: return "off";
    case INVERTER_STARTING: return "starting";
    case INVERTER_ON: return "on";
    case INVERTER_STANDBY: return "standby";
    case INVERTER_FAULT: return "fault";
    case INVERTER_DIAGNOSTIC: return "diagnostic";
    case INVERTER_FACTORY_RESET: return "factory_reset";
    default: return "unknown";
    }
}

static void snapshot_system_state(system_state_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    if (sys_state_mutex != NULL) {
        xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
        memcpy(snapshot, &sys_state, sizeof(*snapshot));
        xSemaphoreGive(sys_state_mutex);
    } else {
        memcpy(snapshot, &sys_state, sizeof(*snapshot));
    }
}

static void add_float_or_null(cJSON *object, const char *name,
                              float value, bool valid)
{
    if (valid && isfinite(value)) {
        cJSON_AddNumberToObject(object, name, (double)value);
    } else {
        cJSON_AddNullToObject(object, name);
    }
}

static bool get_wifi_snapshot(wifi_status_t *status)
{
    memset(status, 0, sizeof(*status));
    return wifi_events_get_status_copy(status) == ESP_OK;
}

static const char *wifi_status_state_name(wifi_connection_state_t state)
{
    switch (state) {
    case WIFI_STATE_CONNECTED: return "connected";
    case WIFI_STATE_CONNECTING: return "connecting";
    case WIFI_STATE_DISCONNECTED: return "disconnected";
    case WIFI_STATE_FAILED: return "failed";
    case WIFI_STATE_RECONNECTING: return "reconnecting";
    case WIFI_STATE_IDLE: return "idle";
    case WIFI_STATE_PROVISIONING: return "provisioning";
    case WIFI_STATE_AP_ACTIVE: return "ap_active";
    default: return "unknown";
    }
}

static void add_wifi_summary(cJSON *root, const wifi_status_t *status,
                             bool have_status)
{
    const wifi_monitor_status_t *monitor = wifi_monitor_get_status();
    cJSON *wifi = cJSON_CreateObject();
    if (wifi == NULL) {
        return;
    }
    cJSON_AddBoolToObject(wifi, "available", have_status);
    cJSON_AddBoolToObject(wifi, "connected", have_status && status->connected);
    cJSON_AddBoolToObject(wifi, "got_ip", have_status && status->got_ip);
    cJSON_AddBoolToObject(wifi, "internet_available",
                          have_status && status->internet_available);
    cJSON_AddNumberToObject(wifi, "rssi",
                            have_status ? status->rssi : -127);
    if (have_status) {
        char ip[16] = {0};
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&status->ip));
        cJSON_AddStringToObject(wifi, "ip", ip);
    } else {
        cJSON_AddNullToObject(wifi, "ip");
    }
    if (monitor != NULL) {
        cJSON_AddBoolToObject(wifi, "monitor_online",
                              monitor->connected && monitor->got_ip);
    }
    cJSON_AddItemToObject(root, "wifi", wifi);
}

static void add_power_control_status(cJSON *root, const system_state_t *state)
{
    inverter_power_status_t status = {0};
    inverter_power_status_from_snapshot(state, &status);
    cJSON *power = cJSON_CreateObject();
    if (power == NULL) return;
    cJSON_AddBoolToObject(power, "relay_commanded", status.relay_commanded);
    cJSON_AddBoolToObject(power, "physical_feedback_supported", status.physical_feedback_supported);
    cJSON_AddBoolToObject(power, "physical_feedback_active", status.physical_feedback_active);
    cJSON_AddBoolToObject(power, "physical_feedback_mocked", status.physical_feedback_mocked);
    cJSON_AddBoolToObject(power, "interlocks_ready", status.interlocks_ready);
    cJSON_AddStringToObject(power, "interlock_reason", status.interlock_reason);
    cJSON_AddItemToObject(root, "power_control", power);
}

static esp_err_t api_status_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    system_state_t state;
    snapshot_system_state(&state);
    wifi_status_t wifi = {0};
    const bool have_wifi = get_wifi_snapshot(&wifi);
    network_services_status_t services = {0};
    network_services_get_status(&services);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "device", WIFI_HOSTNAME);
    cJSON_AddBoolToObject(root, "system_ready", state.system_ready);
    cJSON_AddStringToObject(root, "inverter_state",
                            inverter_state_name(state.inverter.inverter_state));
    cJSON_AddBoolToObject(root, "inverter_active", state.inverter.inverter_active);
    cJSON_AddBoolToObject(root, "output_enabled", state.output_enabled);
    cJSON_AddNumberToObject(root, "operating_mode", state.inverter.operating_mode);
    cJSON_AddNumberToObject(root, "fault_flags", (double)state.error.error_flags);
    cJSON_AddStringToObject(root, "fault_message", state.error.last_error_msg);
    cJSON_AddBoolToObject(root, "wifi_service", services.http_running || services.dashboard_running);
    cJSON_AddNumberToObject(root, "uptime_seconds",
                            (double)(esp_timer_get_time() / 1000000ULL));
    add_wifi_summary(root, &wifi, have_wifi);
    add_power_control_status(root, &state);
    /* Compatibility fields for clients that previously consumed the Wi-Fi
     * status endpoint at /api/v1/status. The canonical data is now nested
     * under wifi and is also available at /api/v1/wifi. */
    if (have_wifi) {
        cJSON_AddStringToObject(root, "state", wifi_status_state_name(wifi.state));
        cJSON_AddBoolToObject(root, "connected", wifi.connected);
        cJSON_AddBoolToObject(root, "got_ip", wifi.got_ip);
        cJSON_AddBoolToObject(root, "internet_available", wifi.internet_available);
        cJSON_AddStringToObject(root, "internet",
                                wifi.internet_available ? "available" : "unavailable");
        cJSON_AddNumberToObject(root, "rssi", wifi.rssi);
        cJSON_AddNumberToObject(root, "monitor_rssi", wifi_monitor_get_rssi());
        char legacy_ip[16] = {0};
        char legacy_gateway[16] = {0};
        char legacy_netmask[16] = {0};
        snprintf(legacy_ip, sizeof(legacy_ip), IPSTR, IP2STR(&wifi.ip));
        snprintf(legacy_gateway, sizeof(legacy_gateway), IPSTR, IP2STR(&wifi.gateway));
        snprintf(legacy_netmask, sizeof(legacy_netmask), IPSTR, IP2STR(&wifi.netmask));
        cJSON_AddStringToObject(root, "ip", legacy_ip);
        cJSON_AddStringToObject(root, "gateway", legacy_gateway);
        cJSON_AddStringToObject(root, "netmask", legacy_netmask);
    }
    cJSON_AddBoolToObject(root, "http_service", services.http_running);
    cJSON_AddBoolToObject(root, "dashboard_service", services.dashboard_running);
    cJSON_AddBoolToObject(root, "websocket_service", services.websocket_running);
    cJSON_AddBoolToObject(root, "mdns_service", services.mdns_running);
    cJSON_AddBoolToObject(root, "mqtt_configured", services.mqtt_configured);
    cJSON_AddBoolToObject(root, "mqtt_connected", services.mqtt_connected);
    const inverter_start_error_code_t start_error = inverter_get_last_start_error_code();
    cJSON_AddNumberToObject(root, "start_error_code", (unsigned)start_error);
    char start_error_text[16] = {0};
    snprintf(start_error_text, sizeof(start_error_text), "E%03X",
             (unsigned)start_error & 0x0FFFU);
    cJSON_AddStringToObject(root, "start_error", start_error_text);
    cJSON_AddStringToObject(root, "start_error_reason",
                            inverter_get_last_start_error_reason());
    return json_api_send(req, root, 200);
}

static esp_err_t api_system_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    char time_text[32] = "unset";
    if (ntp_client_get_time_string(time_text, sizeof(time_text)) != ESP_OK) {
        strncpy(time_text, "unset", sizeof(time_text) - 1U);
    }
    network_services_status_t services = {0};
    network_services_get_status(&services);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "firmware_version", INVERTER_FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "hardware", "ESP32");
    cJSON_AddStringToObject(root, "device", WIFI_HOSTNAME);
    cJSON_AddNumberToObject(root, "lcd_columns", lcd_geometry_cols());
    cJSON_AddNumberToObject(root, "lcd_rows", lcd_geometry_rows());
    cJSON_AddNumberToObject(root, "uptime_seconds",
                            (double)(esp_timer_get_time() / 1000000ULL));
    cJSON_AddNumberToObject(root, "free_heap_bytes", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "minimum_free_heap_bytes",
                            esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(root, "system_time", time_text);
    cJSON_AddBoolToObject(root, "ntp_running", services.ntp_running);
    cJSON_AddBoolToObject(root, "ntp_time_set", services.ntp_time_set);
    cJSON_AddNumberToObject(root, "reset_reason", (int)esp_reset_reason());
    return json_api_send(req, root, 200);
}

static esp_err_t api_inverter_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    system_state_t state;
    snapshot_system_state(&state);
    const bool valid = state.adc_data_valid;
    const float power_kw = valid && state.inverter.output_voltage > 0.0f &&
                                   state.inverter.output_current > 0.0f
                               ? state.inverter.output_voltage *
                                     state.inverter.output_current / 1000.0f
                               : 0.0f;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "state",
                            inverter_state_name(state.inverter.inverter_state));
    cJSON_AddNumberToObject(root, "operating_mode", state.inverter.operating_mode);
    add_float_or_null(root, "voltage", state.inverter.output_voltage, valid);
    add_float_or_null(root, "current", state.inverter.output_current, valid);
    add_float_or_null(root, "power_kw", power_kw, valid);
    add_float_or_null(root, "frequency", state.inverter.output_frequency, valid);
    add_float_or_null(root, "temperature", state.inverter.temperature, valid);
    cJSON_AddNumberToObject(root, "load_percentage", state.inverter.load_percentage);
    cJSON_AddBoolToObject(root, "active", state.inverter.inverter_active);
    cJSON_AddBoolToObject(root, "telemetry_valid", valid);
    add_power_control_status(root, &state);
    return json_api_send(req, root, 200);
}

/* POST /api/v1/inverter/control
 * Body: {"action":"on"} or {"action":"off"}. The handler intentionally
 * delegates to the existing inverter state machine so all current protection
 * checks, startup sequencing, ramp-down, relay handling and fault reporting
 * remain authoritative on the ESP32. */
static esp_err_t api_inverter_control_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    if (req->content_len == 0 || req->content_len > 96) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Invalid inverter control payload");
        return json_api_send(req, error, 400);
    }

    char body[97] = {0};
    const int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Could not read inverter control payload");
        return json_api_send(req, error, 400);
    }
    cJSON *input = cJSON_Parse(body);
    const cJSON *action = input != NULL ? cJSON_GetObjectItem(input, "action") : NULL;
    const bool turn_on = action != NULL && cJSON_IsString(action) &&
                         action->valuestring != NULL && strcmp(action->valuestring, "on") == 0;
    const bool turn_off = action != NULL && cJSON_IsString(action) &&
                          action->valuestring != NULL && strcmp(action->valuestring, "off") == 0;
    cJSON_Delete(input);
    if (!turn_on && !turn_off) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Action must be 'on' or 'off'");
        return json_api_send(req, error, 400);
    }

    system_state_t before;
    snapshot_system_state(&before);
    inverter_power_status_t power_status = {0};
    inverter_power_status_from_snapshot(&before, &power_status);
    if (!power_status.physical_feedback_supported || !power_status.interlocks_ready) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "error", !power_status.physical_feedback_supported
                                             ? "Physical output feedback is not configured; remote power control is disabled"
                                             : power_status.interlock_reason);
        cJSON_AddBoolToObject(error, "physical_feedback_supported", power_status.physical_feedback_supported);
        cJSON_AddBoolToObject(error, "interlocks_ready", power_status.interlocks_ready);
        cJSON_AddStringToObject(error, "interlock_reason", power_status.interlock_reason);
        return json_api_send(req, error, 423);
    }
    if (turn_on && before.inverter.inverter_state == INVERTER_STARTING) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Inverter startup is already in progress");
        return json_api_send(req, error, 409);
    }

    if (turn_on && before.inverter.inverter_state == INVERTER_OFF) {
        inverter_power_on();
    } else if (turn_off && before.inverter.inverter_state != INVERTER_OFF) {
        shutdown_inverter();
    }

    system_state_t after;
    snapshot_system_state(&after);
    const bool reached_target = turn_on ? after.inverter.inverter_state == INVERTER_ON
                                        : after.inverter.inverter_state == INVERTER_OFF;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", reached_target);
    cJSON_AddStringToObject(result, "action", turn_on ? "on" : "off");
    cJSON_AddStringToObject(result, "state", inverter_state_name(after.inverter.inverter_state));
    cJSON_AddBoolToObject(result, "active", after.inverter.inverter_active);
    cJSON_AddBoolToObject(result, "output_enabled", after.output_enabled);
    add_power_control_status(result, &after);
    if (reached_target) {
        cJSON_AddStringToObject(result, "message", turn_on ? "Inverter is on" : "Inverter is off");
        return json_api_send(req, result, 200);
    }
    cJSON_AddStringToObject(result, "error", turn_on ? inverter_get_last_start_error_reason()
                                                       : "The inverter did not reach the requested off state");
    return json_api_send(req, result, 409);
}

static esp_err_t api_battery_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    system_state_t state;
    snapshot_system_state(&state);
    const bool valid = state.adc_data_valid;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    add_float_or_null(root, "voltage", state.inverter.battery.voltage, valid);
    add_float_or_null(root, "temperature",
                      state.inverter.battery.battery_temperature, valid);
    add_float_or_null(root, "state_of_charge",
                      state.inverter.battery.battery_soc, valid);
    cJSON_AddBoolToObject(root, "charging", state.battery_charging);
    cJSON_AddBoolToObject(root, "low", state.inverter.battery.is_low || state.low_battery);
    cJSON_AddBoolToObject(root, "critical", state.inverter.battery.is_critical);
    cJSON_AddBoolToObject(root, "telemetry_valid", valid);
    cJSON_AddStringToObject(root, "current_status",
                            state.battery_charging ? "charging" : "discharging_or_idle");
    cJSON_AddNullToObject(root, "current");
    cJSON_AddNullToObject(root, "power_kw");
    return json_api_send(req, root, 200);
}

static esp_err_t api_solar_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    system_state_t state;
    snapshot_system_state(&state);
    const bool valid = state.adc_data_valid;
    const float power_kw = valid && state.dc_input_voltage > 0.0f &&
                                   state.dc_input_current > 0.0f
                               ? state.dc_input_voltage * state.dc_input_current / 1000.0f
                               : 0.0f;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    add_float_or_null(root, "voltage", state.dc_input_voltage, valid);
    add_float_or_null(root, "current", state.dc_input_current, valid);
    add_float_or_null(root, "power_kw", power_kw, valid);
    cJSON_AddBoolToObject(root, "active", valid && power_kw > 0.0f);
    cJSON_AddBoolToObject(root, "telemetry_valid", valid);
    cJSON_AddNullToObject(root, "energy_kwh");
    return json_api_send(req, root, 200);
}

static esp_err_t api_load_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    system_state_t state;
    snapshot_system_state(&state);
    const bool valid = state.adc_data_valid;
    const float power_kw = valid && state.inverter.output_voltage > 0.0f &&
                                   state.inverter.output_current > 0.0f
                               ? state.inverter.output_voltage *
                                     state.inverter.output_current / 1000.0f
                               : 0.0f;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    add_float_or_null(root, "voltage", state.inverter.output_voltage, valid);
    add_float_or_null(root, "current", state.inverter.output_current, valid);
    add_float_or_null(root, "power_kw", power_kw, valid);
    cJSON_AddNumberToObject(root, "percentage", state.inverter.load_percentage);
    cJSON_AddBoolToObject(root, "connected", state.load_connected);
    cJSON_AddBoolToObject(root, "active", state.output_enabled);
    cJSON_AddBoolToObject(root, "telemetry_valid", valid);
    return json_api_send(req, root, 200);
}

static esp_err_t api_grid_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    system_state_t state;
    snapshot_system_state(&state);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "available", false);
    cJSON_AddStringToObject(root, "state", "unavailable");
    add_float_or_null(root, "voltage", state.ac_input_voltage,
                      state.ac_input_voltage > 0.0f);
    cJSON_AddNullToObject(root, "current");
    cJSON_AddNullToObject(root, "power_kw");
    cJSON_AddNullToObject(root, "frequency");
    cJSON_AddStringToObject(root, "reason",
                            "No dedicated grid meter is present in the hardware map");
    return json_api_send(req, root, 200);
}

static const httpd_uri_t s_device_uris[] = {
    {.uri = "/api/v1/status", .method = HTTP_GET, .handler = api_status_handler},
    {.uri = "/api/v1/system", .method = HTTP_GET, .handler = api_system_handler},
    {.uri = "/api/v1/inverter", .method = HTTP_GET, .handler = api_inverter_handler},
    {.uri = "/api/v1/inverter/control", .method = HTTP_POST, .handler = api_inverter_control_handler},
    {.uri = "/api/v1/battery", .method = HTTP_GET, .handler = api_battery_handler},
    {.uri = "/api/v1/solar", .method = HTTP_GET, .handler = api_solar_handler},
    {.uri = "/api/v1/load", .method = HTTP_GET, .handler = api_load_handler},
    {.uri = "/api/v1/grid", .method = HTTP_GET, .handler = api_grid_handler},
    {.uri = "/api/v1/status", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/system", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/inverter", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/inverter/control", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/battery", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/solar", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/load", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/grid", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
};

const httpd_uri_t *json_api_device_uris(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(s_device_uris) / sizeof(s_device_uris[0]);
    }
    return s_device_uris;
}
