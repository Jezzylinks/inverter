#include "server/json/json_api_ota.h"

#include "cJSON.h"
#include "esp_http_server.h"

#include "app/app_services.h"
#include "ota/ota_service.h"
#include "server/json/json_api_server.h"

static const char *ota_state_name(app_ota_state_t state)
{
    switch (state) {
    case APP_OTA_IDLE: return "idle";
    case APP_OTA_CHECKING: return "checking";
    case APP_OTA_AVAILABLE: return "available";
    case APP_OTA_CONFIRMING: return "confirming";
    case APP_OTA_PREPARING: return "preparing";
    case APP_OTA_DOWNLOADING: return "downloading";
    case APP_OTA_VERIFYING: return "verifying";
    case APP_OTA_COMPLETE: return "complete";
    case APP_OTA_ERROR: return "error";
    case APP_OTA_CANCELLED: return "cancelled";
    default: return "unknown";
    }
}

static esp_err_t api_ota_status_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    app_ota_status_t status = {0};
    app_services_get_ota_status(&status);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "state", ota_state_name(status.state));
    cJSON_AddStringToObject(root, "installed_version", status.installed_version);
    cJSON_AddStringToObject(root, "available_version", status.available_version);
    cJSON_AddStringToObject(root, "error", status.error_detail);
    cJSON_AddNumberToObject(root, "progress_percent", status.progress_percent);
    cJSON_AddBoolToObject(root, "auto_check_enabled", status.auto_check_enabled);
    cJSON_AddBoolToObject(root, "update_available", status.update_available);
    cJSON_AddBoolToObject(root, "confirmation_pending", status.confirmation_pending);
    cJSON_AddBoolToObject(root, "cancel_confirmation_pending",
                          status.cancel_confirmation_pending);
    cJSON_AddBoolToObject(root, "in_progress", ota_service_in_progress());
    return json_api_send(req, root, 200);
}

static esp_err_t api_ota_action_response(httpd_req_t *req, esp_err_t err,
                                         const char *success_message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "success", err == ESP_OK);
    cJSON_AddStringToObject(root, "message",
                            err == ESP_OK ? success_message : esp_err_to_name(err));
    return json_api_send(req, root,
                         err == ESP_OK ? 202 :
                         (err == ESP_ERR_INVALID_STATE ? 409 : 400));
}

static esp_err_t api_ota_check_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    return api_ota_action_response(req, app_services_check_for_update(true),
                                    "OTA manifest check started");
}

static esp_err_t api_ota_confirm_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    return api_ota_action_response(req, app_services_confirm_update(),
                                    "OTA update started");
}

static esp_err_t api_ota_start_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    esp_err_t err = app_services_request_update_confirmation();
    if (err == ESP_OK) {
        err = app_services_confirm_update();
    }
    return api_ota_action_response(req, err, "OTA update started");
}

static esp_err_t api_ota_cancel_handler(httpd_req_t *req)
{
    const esp_err_t auth_err = json_api_require_pin(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    return api_ota_action_response(req, app_services_cancel_update(),
                                    "OTA operation cancelled");
}

static const httpd_uri_t s_ota_uris[] = {
    {.uri = "/api/v1/ota", .method = HTTP_GET, .handler = api_ota_status_handler},
    {.uri = "/api/v1/ota/check", .method = HTTP_POST, .handler = api_ota_check_handler},
    {.uri = "/api/v1/ota/confirm", .method = HTTP_POST, .handler = api_ota_confirm_handler},
    {.uri = "/api/v1/ota/start", .method = HTTP_POST, .handler = api_ota_start_handler},
    {.uri = "/api/v1/ota/cancel", .method = HTTP_POST, .handler = api_ota_cancel_handler},
    {.uri = "/api/v1/ota", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/ota/check", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/ota/confirm", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/ota/start", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
    {.uri = "/api/v1/ota/cancel", .method = HTTP_OPTIONS, .handler = json_api_options_handler},
};

const httpd_uri_t *json_api_ota_uris(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(s_ota_uris) / sizeof(s_ota_uris[0]);
    }
    return s_ota_uris;
}

