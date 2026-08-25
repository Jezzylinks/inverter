#include "cloud/cloud_reporting.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "system/firmware_version.h"

#define CLOUD_NVS_NAMESPACE "cloud_rpt"
#define CLOUD_NVS_KEY "config"
#define CLOUD_MIN_PERIOD_SEC 30U
#define CLOUD_DEFAULT_PERIOD_SEC 60U
#define CLOUD_HTTP_TIMEOUT_MS 12000

static const char *TAG = "CLOUD_RPT";

typedef struct {
    inverter_state_t inverter_state;
    uint32_t fault_flags;
    float battery_voltage;
    float battery_soc;
    float solar_power_kw;
    float load_power_kw;
    float output_voltage;
    float output_current;
    float output_frequency;
    uint8_t load_percentage;
    int rssi;
    bool telemetry_valid;
} cloud_snapshot_t;

typedef struct {
    char response[256];
    size_t response_len;
} cloud_http_response_t;

static cloud_reporting_config_t s_config;
static cloud_reporting_status_t s_status;
static SemaphoreHandle_t s_mutex;
static int64_t s_next_publish_us;

static void safe_copy(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0U) return;
    (void)snprintf(dst, dst_len, "%s", src != NULL ? src : "");
}

static bool is_https_endpoint(const char *endpoint)
{
    return endpoint != NULL && strncmp(endpoint, "https://", 8U) == 0 && strlen(endpoint) > 8U;
}

static void set_error_locked(const char *message)
{
    safe_copy(s_status.last_error, sizeof(s_status.last_error), message);
}

static esp_err_t save_config_locked(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLOUD_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, CLOUD_NVS_KEY, &s_config, sizeof(s_config));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void refresh_status_locked(void)
{
    s_status.enabled = s_config.enabled;
    s_status.configured = is_https_endpoint(s_config.endpoint) && s_config.hardware_id[0] != '\0';
    s_status.enrolled = s_config.device_token[0] != '\0';
}

static const char *state_name(inverter_state_t state)
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

static esp_err_t response_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data != NULL && event->data != NULL) {
        cloud_http_response_t *response = event->user_data;
        const size_t remaining = sizeof(response->response) - response->response_len - 1U;
        const size_t copy_len = event->data_len < (int)remaining ? (size_t)event->data_len : remaining;
        if (copy_len > 0U) {
            memcpy(response->response + response->response_len, event->data, copy_len);
            response->response_len += copy_len;
            response->response[response->response_len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t post_json(const char *url, const char *header_name, const char *header_value,
                           const char *body, cloud_http_response_t *response)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = CLOUD_HTTP_TIMEOUT_MS,
        .event_handler = response_event,
        .user_data = response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK && header_name != NULL && header_value != NULL) err = esp_http_client_set_header(client, header_name, header_value);
    if (err == ESP_OK) err = esp_http_client_set_post_field(client, body, (int)strlen(body));
    if (err == ESP_OK) err = esp_http_client_perform(client);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    return (err == ESP_OK && status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

static esp_err_t enroll_if_needed(void)
{
    if (s_config.device_token[0] != '\0') return ESP_OK;
    if (s_config.enrollment_code[0] == '\0') return ESP_ERR_INVALID_STATE;
    char url[CLOUD_REPORTING_ENDPOINT_MAX + 40U];
    (void)snprintf(url, sizeof(url), "%s/api/device/v1/enroll", s_config.endpoint);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "hardwareId", s_config.hardware_id);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) return ESP_ERR_NO_MEM;
    cloud_http_response_t response = {0};
    const esp_err_t err = post_json(url, "X-Inverter-Enrollment", s_config.enrollment_code, body, &response);
    cJSON_free(body);
    if (err != ESP_OK) return err;
    cJSON *reply = cJSON_Parse(response.response);
    cJSON *token = reply != NULL ? cJSON_GetObjectItemCaseSensitive(reply, "deviceToken") : NULL;
    if (token == NULL || !cJSON_IsString(token) || token->valuestring == NULL || token->valuestring[0] == '\0') {
        cJSON_Delete(reply);
        return ESP_FAIL;
    }
    safe_copy(s_config.device_token, sizeof(s_config.device_token), token->valuestring);
    s_config.enrollment_code[0] = '\0';
    cJSON_Delete(reply);
    return save_config_locked();
}

static void publish_task(void *argument)
{
    cloud_snapshot_t *snapshot = argument;
    esp_err_t err = ESP_FAIL;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        err = enroll_if_needed();
        cloud_reporting_config_t config = s_config;
        xSemaphoreGive(s_mutex);
        if (err == ESP_OK) {
            char url[CLOUD_REPORTING_ENDPOINT_MAX + 48U];
            (void)snprintf(url, sizeof(url), "%s/api/device/v1/telemetry", config.endpoint);
            cJSON *root = cJSON_CreateObject();
            if (root != NULL) {
                cJSON_AddStringToObject(root, "hardwareId", config.hardware_id);
                cJSON_AddStringToObject(root, "firmwareVersion", INVERTER_FIRMWARE_VERSION);
                cJSON_AddStringToObject(root, "inverterState", state_name(snapshot->inverter_state));
                cJSON_AddNumberToObject(root, "faultFlags", snapshot->fault_flags);
                cJSON_AddNumberToObject(root, "batteryVoltage", snapshot->battery_voltage);
                cJSON_AddNumberToObject(root, "batterySoc", snapshot->battery_soc);
                cJSON_AddNumberToObject(root, "solarPowerKw", snapshot->solar_power_kw);
                cJSON_AddNumberToObject(root, "loadPowerKw", snapshot->load_power_kw);
                cJSON_AddNumberToObject(root, "outputVoltage", snapshot->output_voltage);
                cJSON_AddNumberToObject(root, "outputCurrent", snapshot->output_current);
                cJSON_AddNumberToObject(root, "outputFrequency", snapshot->output_frequency);
                cJSON_AddNumberToObject(root, "loadPercentage", snapshot->load_percentage);
                cJSON_AddNumberToObject(root, "rssi", snapshot->rssi);
                cJSON_AddBoolToObject(root, "telemetryValid", snapshot->telemetry_valid);
                char *body = cJSON_PrintUnformatted(root);
                cJSON_Delete(root);
                if (body != NULL) {
                    cloud_http_response_t response = {0};
                    err = post_json(url, "X-Inverter-Device-Token", config.device_token, body, &response);
                    cJSON_free(body);
                } else {
                    err = ESP_ERR_NO_MEM;
                }
            } else {
                err = ESP_ERR_NO_MEM;
            }
        }
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_status.publish_in_progress = false;
            if (err == ESP_OK) {
                s_status.last_success_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
                set_error_locked("");
            } else {
                set_error_locked("HTTPS report failed");
            }
            refresh_status_locked();
            xSemaphoreGive(s_mutex);
        }
    }
    vPortFree(snapshot);
    vTaskDelete(NULL);
}

esp_err_t cloud_reporting_init(void)
{
    if (s_mutex != NULL) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    memset(&s_config, 0, sizeof(s_config));
    s_config.period_sec = CLOUD_DEFAULT_PERIOD_SEC;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLOUD_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = sizeof(s_config);
        err = nvs_get_blob(handle, CLOUD_NVS_KEY, &s_config, &size);
        nvs_close(handle);
        if (err != ESP_OK || size != sizeof(s_config)) {
            memset(&s_config, 0, sizeof(s_config));
            s_config.period_sec = CLOUD_DEFAULT_PERIOD_SEC;
        }
    }
    if (s_config.period_sec < CLOUD_MIN_PERIOD_SEC) s_config.period_sec = CLOUD_DEFAULT_PERIOD_SEC;
    refresh_status_locked();
    ESP_LOGI(TAG, "Cloud reporting initialized: enabled=%d configured=%d", s_status.enabled, s_status.configured);
    return ESP_OK;
}

esp_err_t cloud_reporting_get_config(cloud_reporting_config_t *out)
{
    if (out == NULL || s_mutex == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(250)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *out = s_config;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t cloud_reporting_set_config(const cloud_reporting_config_t *config)
{
    if (config == NULL || s_mutex == NULL || config->period_sec < CLOUD_MIN_PERIOD_SEC ||
        (config->enabled && (!is_https_endpoint(config->endpoint) || config->hardware_id[0] == '\0'))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const bool endpoint_changed = strcmp(s_config.endpoint, config->endpoint) != 0 ||
                                  strcmp(s_config.hardware_id, config->hardware_id) != 0;
    s_config.enabled = config->enabled;
    s_config.period_sec = config->period_sec;
    safe_copy(s_config.endpoint, sizeof(s_config.endpoint), config->endpoint);
    safe_copy(s_config.hardware_id, sizeof(s_config.hardware_id), config->hardware_id);
    if (config->enrollment_code[0] != '\0') {
        safe_copy(s_config.enrollment_code, sizeof(s_config.enrollment_code), config->enrollment_code);
        s_config.device_token[0] = '\0';
    } else if (endpoint_changed) {
        s_config.device_token[0] = '\0';
    }
    const esp_err_t err = save_config_locked();
    refresh_status_locked();
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t cloud_reporting_get_status(cloud_reporting_status_t *out)
{
    if (out == NULL || s_mutex == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(250)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *out = s_status;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void cloud_reporting_publish(const system_state_t *state, float solar_power_kw, float load_power_kw, int rssi)
{
    if (state == NULL || s_mutex == NULL) return;
    const int64_t now = esp_timer_get_time();
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return;
    if (!s_config.enabled || !s_status.configured || s_status.publish_in_progress || now < s_next_publish_us) {
        xSemaphoreGive(s_mutex);
        return;
    }
    cloud_snapshot_t *snapshot = pvPortMalloc(sizeof(*snapshot));
    if (snapshot == NULL) {
        set_error_locked("No memory for report");
        xSemaphoreGive(s_mutex);
        return;
    }
    *snapshot = (cloud_snapshot_t) {
        .inverter_state = state->inverter.inverter_state,
        .fault_flags = state->fault_flags,
        .battery_voltage = state->inverter.battery.voltage,
        .battery_soc = state->inverter.battery.battery_soc,
        .solar_power_kw = solar_power_kw,
        .load_power_kw = load_power_kw,
        .output_voltage = state->inverter.output_voltage,
        .output_current = state->inverter.output_current,
        .output_frequency = state->inverter.output_frequency,
        .load_percentage = state->inverter.load_percentage,
        .rssi = rssi,
        .telemetry_valid = state->inverter.adc_data_valid,
    };
    s_status.publish_in_progress = true;
    s_next_publish_us = now + ((int64_t)s_config.period_sec * 1000000LL);
    xSemaphoreGive(s_mutex);
    if (xTaskCreate(publish_task, "cloud_report", 6144, snapshot, 3, NULL) != pdPASS) {
        vPortFree(snapshot);
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
            s_status.publish_in_progress = false;
            set_error_locked("Report task start failed");
            xSemaphoreGive(s_mutex);
        }
    }
}
