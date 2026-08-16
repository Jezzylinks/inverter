/**
 * @file websocket_server.c
 * @brief WebSocket server for real-time status updates
 */

#include "websocket_server.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "wifi/wifi_events.h"
#include "wifi/wifi_monitor.h"
#include "security/security.h"
#include "system_state.h"

static const char *TAG = "WS_SERVER";

#define WS_MAX_CLIENTS 4

/*----------------------------------------------------------
 * Client tracking
 *---------------------------------------------------------*/
typedef struct
{
    int fd;
    bool active;
    bool authenticated;
} ws_client_t;

static ws_client_t s_clients[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_mutex = NULL;
static httpd_handle_t s_server = NULL;
static bool s_initialized = false;
extern system_state_t sys_state;

/*----------------------------------------------------------
 * Find or allocate client slot
 *---------------------------------------------------------*/
static int ws_find_client(int fd)
{
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
    {
        if (s_clients[i].active && s_clients[i].fd == fd)
        {
            return i;
        }
    }
    return -1;
}

static int ws_alloc_client(httpd_handle_t server, int fd)
{
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
    {
        if (!s_clients[i].active)
        {
            s_clients[i].fd = fd;
            s_clients[i].active = true;
            s_clients[i].authenticated = !sys_state.security.enabled;
            return i;
        }
    }

    return -1;
}

static void ws_remove_client(int fd)
{
    int idx = ws_find_client(fd);
    if (idx >= 0)
    {
        s_clients[idx].active = false;
        s_clients[idx].fd = -1;
        s_clients[idx].authenticated = false;
    }
}

/*----------------------------------------------------------
 * Broadcast status to all connected clients
 *---------------------------------------------------------*/
void websocket_broadcast_status(const wifi_status_t *status)
{
    if (!s_initialized || status == NULL || s_server == NULL)
    {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "status");

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
    cJSON_AddBoolToObject(root, "internet", status->internet_available);
    cJSON_AddNumberToObject(root, "rssi", status->rssi);
    cJSON_AddNumberToObject(root, "retry_count", status->retry_count);

    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&status->ip));
    cJSON_AddStringToObject(root, "ip", ip_str);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL)
    {
        return;
    }

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
    {
        if (s_clients[i].active && s_clients[i].authenticated)
        {
            httpd_ws_frame_t ws_pkt = {
                .final = true,
                .fragmented = false,
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)json_str,
                .len = strlen(json_str),
            };

            esp_err_t err = httpd_ws_send_frame_async(
                s_server,
                s_clients[i].fd,
                &ws_pkt);

            if (err != ESP_OK)
            {
                ESP_LOGW(TAG,
                         "WS send failed for fd %d: %s",
                         s_clients[i].fd,
                         esp_err_to_name(err));

                s_clients[i].active = false;
            }
        }
    }

    if (s_mutex)
    {
        xSemaphoreGive(s_mutex);
    }

    free(json_str);
}

/*----------------------------------------------------------
 * Status callback from wifi_events
 *---------------------------------------------------------*/
static void ws_status_callback(const wifi_status_t *status)
{
    websocket_broadcast_status(status);
}

/*----------------------------------------------------------
 * WebSocket handler
 *---------------------------------------------------------*/
static bool ws_verify_pin_text(const char *pin_text)
{
    if (pin_text == NULL || strlen(pin_text) != SECURITY_PIN_LEN) {
        return false;
    }
    uint8_t pin[SECURITY_PIN_LEN] = {0};
    for (size_t i = 0U; i < SECURITY_PIN_LEN; ++i) {
        if (pin_text[i] < '0' || pin_text[i] > '9') {
            memset(pin, 0, sizeof(pin));
            return false;
        }
        pin[i] = (uint8_t)(pin_text[i] - '0');
    }
    const bool valid = security_verify_pin(pin);
    memset(pin, 0, sizeof(pin));
    return valid;
}

static bool ws_authorized(httpd_req_t *req)
{
    if (!sys_state.security.enabled) {
        return true;
    }
    if (httpd_req_get_hdr_value_len(req, "X-Inverter-PIN") != SECURITY_PIN_LEN) {
        return false;
    }
    char pin_text[SECURITY_PIN_LEN + 1U] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Inverter-PIN", pin_text,
                                    sizeof(pin_text)) != ESP_OK) {
        return false;
    }
    const bool valid = ws_verify_pin_text(pin_text);
    memset(pin_text, 0, sizeof(pin_text));
    return valid;
}

static esp_err_t ws_send_json(int fd, cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL || s_server == NULL) {
        free(payload);
        return ESP_ERR_NO_MEM;
    }
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)payload,
        .len = strlen(payload),
    };
    const esp_err_t err = httpd_ws_send_frame_async(s_server, fd, &frame);
    free(payload);
    return err;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
        /* Browsers cannot set X-Inverter-PIN on a WebSocket constructor. A
         * header remains supported for native clients, while browser clients
         * authenticate with the first JSON message after this handshake. */
        const size_t pin_header_len = httpd_req_get_hdr_value_len(req, "X-Inverter-PIN");
        const bool header_authenticated = !sys_state.security.enabled ||
            (pin_header_len == SECURITY_PIN_LEN && ws_authorized(req));
        if (sys_state.security.enabled && pin_header_len > 0U && !header_authenticated) {
            httpd_resp_set_status(req, "401 Unauthorized");
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            return httpd_resp_sendstr(req, "Invalid PIN");
        }
        const int fd = httpd_req_to_sockfd(req);
        if (s_mutex) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
        }
        int client_idx = ws_find_client(fd);
        if (client_idx < 0) {
            client_idx = ws_alloc_client(req->handle, fd);
        }
        if (client_idx >= 0) {
            s_clients[client_idx].authenticated = header_authenticated;
        }
        if (s_mutex) {
            xSemaphoreGive(s_mutex);
        }
        ESP_LOGI(TAG, "WS handshake from fd %d", fd);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "WS recv frame failed: %s", esp_err_to_name(ret));
        return ret;
    }

    int fd = httpd_req_to_sockfd(req);

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT)
    {
        char *buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL)
        {
            return ESP_ERR_NO_MEM;
        }

        ws_pkt.payload = (uint8_t *)buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK)
        {
            buf[ws_pkt.len] = '\0';
            ESP_LOGI(TAG, "WS message from fd %d: %s", fd, buf);

            /* Parse JSON command */
            cJSON *json = cJSON_Parse(buf);
            if (json)
            {
                int client_idx = -1;
                if (s_mutex) {
                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                }
                client_idx = ws_find_client(fd);
                if (client_idx < 0) {
                    client_idx = ws_alloc_client(req->handle, fd);
                }
                const bool already_authenticated = client_idx >= 0 &&
                    s_clients[client_idx].authenticated;
                if (s_mutex) {
                    xSemaphoreGive(s_mutex);
                }

                cJSON *cmd = cJSON_GetObjectItem(json, "cmd");
                if (cmd && cJSON_IsString(cmd))
                {
                    const char *cmd_str = cJSON_GetStringValue(cmd);
                    if (strcmp(cmd_str, "authenticate") == 0)
                    {
                        cJSON *pin = cJSON_GetObjectItem(json, "pin");
                        const bool valid = !sys_state.security.enabled ||
                            (pin && cJSON_IsString(pin) &&
                             ws_verify_pin_text(cJSON_GetStringValue(pin)));
                        if (client_idx >= 0 && s_mutex) {
                            xSemaphoreTake(s_mutex, portMAX_DELAY);
                        }
                        if (client_idx >= 0) {
                            s_clients[client_idx].authenticated = valid;
                        }
                        if (client_idx >= 0 && s_mutex) {
                            xSemaphoreGive(s_mutex);
                        }
                        cJSON *reply = cJSON_CreateObject();
                        cJSON_AddStringToObject(reply, "type", "authenticated");
                        cJSON_AddBoolToObject(reply, "ok", valid);
                        if (!valid) {
                            cJSON_AddStringToObject(reply, "error", "invalid_pin");
                        }
                        (void)ws_send_json(fd, reply);
                    }
                    else if (!already_authenticated)
                    {
                        cJSON *reply = cJSON_CreateObject();
                        cJSON_AddStringToObject(reply, "type", "error");
                        cJSON_AddStringToObject(reply, "error", "authentication_required");
                        (void)ws_send_json(fd, reply);
                    }
                    else if (strcmp(cmd_str, "get_status") == 0)
                    {
                        /* Send current status immediately */
                        wifi_status_t status;
                        if (wifi_events_get_status_copy(&status) == ESP_OK)
                        {
                            websocket_broadcast_status(&status);
                        }
                    }
                    else if (strcmp(cmd_str, "subscribe") == 0)
                    {
                        /* Client wants updates - register fd */
                        if (s_mutex)
                        {
                            xSemaphoreTake(s_mutex, portMAX_DELAY);
                        }
                        if (ws_find_client(fd) < 0)
                        {
                            ws_alloc_client(req->handle, fd);
                            ESP_LOGI(TAG, "Client fd %d subscribed", fd);
                        }
                        if (s_mutex)
                        {
                            xSemaphoreGive(s_mutex);
                        }

                        /* Send immediate status */
                        wifi_status_t status;
                        if (wifi_events_get_status_copy(&status) == ESP_OK)
                        {
                            websocket_broadcast_status(&status);
                        }
                    }
                }
                cJSON_Delete(json);
            }
        }

        free(buf);
    }
    else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE)
    {
        ESP_LOGI(TAG, "WS close from fd %d", fd);
        if (s_mutex)
        {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
        }
        ws_remove_client(fd);
        if (s_mutex)
        {
            xSemaphoreGive(s_mutex);
        }
    }

    return ESP_OK;
}

/*----------------------------------------------------------
 * URI
 *---------------------------------------------------------*/
static const httpd_uri_t ws_uri = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = ws_handler,
    .user_ctx = NULL,
    .is_websocket = true,
    .handle_ws_control_frames = true,
};

/*----------------------------------------------------------
 * Init / Deinit
 *---------------------------------------------------------*/
esp_err_t websocket_server_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    memset(s_clients, 0, sizeof(s_clients));

    /* Register callback to get status updates */
    wifi_events_register_status_callback(ws_status_callback);

    s_initialized = true;

    ESP_LOGI(TAG, "WebSocket server initialized");

    return ESP_OK;
}

esp_err_t websocket_server_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_OK;
    }

    wifi_events_unregister_status_callback(ws_status_callback);

    if (s_mutex)
    {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    s_initialized = false;

    return ESP_OK;
}

esp_err_t websocket_server_register(httpd_handle_t server)
{
    if (server == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = httpd_register_uri_handler(server, &ws_uri);
    if (ret == ESP_OK)
    {
        s_server = server;
    }

    return ret;
}

esp_err_t websocket_server_unregister(httpd_handle_t server)
{
    if (server == NULL)
    {
        return ESP_OK;
    }

    esp_err_t ret = httpd_unregister_uri_handler(
        server,
        "/ws",
        HTTP_GET);

    if (ret == ESP_OK && s_server == server)
    {
        s_server = NULL;
    }

    return ret;
}