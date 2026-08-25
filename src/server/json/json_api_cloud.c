#include "server/json/json_api_cloud.h"
#include "server/json/json_api_server.h"

#include <string.h>

#include "cJSON.h"
#include "cloud/cloud_reporting.h"

static void copy_json_string(cJSON *json, const char *name, char *destination, size_t length)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (item != NULL && cJSON_IsString(item) && item->valuestring != NULL) {
        (void)snprintf(destination, length, "%s", item->valuestring);
    }
}

static esp_err_t api_cloud_status_handler(httpd_req_t *req)
{
    esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    cloud_reporting_config_t config;
    cloud_reporting_status_t status;
    const esp_err_t config_err = cloud_reporting_get_config(&config);
    const esp_err_t status_err = cloud_reporting_get_status(&status);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", config_err == ESP_OK && config.enabled);
    cJSON_AddBoolToObject(root, "configured", status_err == ESP_OK && status.configured);
    cJSON_AddBoolToObject(root, "enrolled", status_err == ESP_OK && status.enrolled);
    cJSON_AddBoolToObject(root, "publishing", status_err == ESP_OK && status.publish_in_progress);
    cJSON_AddStringToObject(root, "endpoint", config_err == ESP_OK ? config.endpoint : "");
    cJSON_AddStringToObject(root, "hardware_id", config_err == ESP_OK ? config.hardware_id : "");
    cJSON_AddNumberToObject(root, "period_sec", config_err == ESP_OK ? config.period_sec : 0);
    cJSON_AddNumberToObject(root, "last_success_ms", status_err == ESP_OK ? status.last_success_ms : 0);
    cJSON_AddStringToObject(root, "last_error", status_err == ESP_OK ? status.last_error : "Unavailable");
    return json_api_send(req, root, (config_err == ESP_OK && status_err == ESP_OK) ? 200 : 503);
}

static esp_err_t api_cloud_config_handler(httpd_req_t *req)
{
    esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) return auth_err;
    cJSON *json = NULL;
    esp_err_t err = json_api_receive_json(req, &json);
    if (err != ESP_OK) {
        cJSON *response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "error", "Invalid JSON body");
        return json_api_send(req, response, 400);
    }
    cloud_reporting_config_t config;
    (void)cloud_reporting_get_config(&config);
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    cJSON *period = cJSON_GetObjectItemCaseSensitive(json, "period_sec");
    if (enabled != NULL && cJSON_IsBool(enabled)) config.enabled = cJSON_IsTrue(enabled);
    if (period != NULL && cJSON_IsNumber(period)) config.period_sec = (uint32_t)period->valuedouble;
    copy_json_string(json, "endpoint", config.endpoint, sizeof(config.endpoint));
    copy_json_string(json, "hardware_id", config.hardware_id, sizeof(config.hardware_id));
    copy_json_string(json, "enrollment_code", config.enrollment_code, sizeof(config.enrollment_code));
    cJSON_Delete(json);
    err = cloud_reporting_set_config(&config);
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err == ESP_OK) cJSON_AddStringToObject(response, "message", "Cloud reporting configuration saved");
    else cJSON_AddStringToObject(response, "error", "Configuration rejected; require HTTPS endpoint, hardware ID, and at least a 30-second period");
    return json_api_send(req, response, err == ESP_OK ? 200 : 400);
}

static const httpd_uri_t s_cloud_uris[] = {
    {.uri = "/api/v1/cloud", .method = HTTP_GET, .handler = api_cloud_status_handler},
    {.uri = "/api/v1/cloud/config", .method = HTTP_POST, .handler = api_cloud_config_handler},
    {.uri = "/api/v1/cloud", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/cloud/config", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
};

const httpd_uri_t *json_api_cloud_uris(size_t *count)
{
    if (count != NULL) *count = sizeof(s_cloud_uris) / sizeof(s_cloud_uris[0]);
    return s_cloud_uris;
}
