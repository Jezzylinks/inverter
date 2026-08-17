#include "server/network_services.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "server/json/json_api_server.h"
#include "server/mdns/mdns_service.h"
#include "server/mqtt/mqtt_client_manager.h"
#include "server/websocket/websocket_server.h"
#include "server/web/web_dashboard_server.h"
#include "wifi/wifi_config.h"
#include "wifi/wifi_events.h"
#include "wifi/wifi_manager.h"

#define NETWORK_SERVICES_TAG "NET_SERVICES"
#define NETWORK_HTTP_PORT 80U
#define NETWORK_HTTP_STACK_SIZE 8192U
#define NETWORK_SYNC_TASK_STACK_SIZE 8192U
#define NETWORK_HTTP_MAX_URI_HANDLERS 32U

static SemaphoreHandle_t s_mutex;
static httpd_handle_t s_http_server;
static network_mqtt_config_t s_mqtt_config;
static bool s_initialized;
static bool s_running;
static bool s_mdns_running;
static bool s_websocket_running;
static bool s_dashboard_running;
static bool s_station_ready;
static bool s_sync_scheduled;

static void services_lock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void services_unlock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}


static void mqtt_status_callback(mqtt_status_t status)
{
    ESP_LOGI(NETWORK_SERVICES_TAG, "MQTT status: %d", (int)status);
    if (status == MQTT_STATUS_CONNECTED) {
        network_mqtt_config_t config;
        if (network_services_get_mqtt_config(&config) == ESP_OK &&
            config.subscribe_topic[0] != '\0') {
            (void)mqtt_client_subscribe(config.subscribe_topic, config.qos);
        }
    }
}

static void network_services_sync_task(void *arg)
{
    (void)arg;

    services_lock();
    const bool ready = s_station_ready;
    const bool running = s_running;
    services_unlock();

    if (ready && !running) {
        (void)network_services_start();
    } else if (!ready && running) {
        (void)network_services_stop();
    }

    services_lock();
    s_sync_scheduled = false;
    services_unlock();
    vTaskDelete(NULL);
}

static void network_wifi_status_callback(const wifi_status_t *status)
{
    if (status == NULL || !s_initialized || s_mutex == NULL) {
        return;
    }
    const wifi_mode_t mode = wifi_manager_get_mode();
    const bool station_ready = status->state == WIFI_STATE_CONNECTED && status->got_ip;
    const bool ap_ready = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) &&
                          status->state == WIFI_STATE_AP_ACTIVE;
    const bool ready = station_ready || ap_ready;
    bool mdns_running = false;
    services_lock();
    mdns_running = s_mdns_running;
    services_unlock();
    if (mdns_running) {
        const char *state = ready ? "connected" : "offline";
        (void)mdns_service_update_status(state, status->rssi);
    }
    bool schedule = false;
    services_lock();
    s_station_ready = ready;
    if (!s_sync_scheduled) {
        s_sync_scheduled = true;
        schedule = true;
    }
    services_unlock();
    if (schedule && xTaskCreate(network_services_sync_task,
                                "net_services_sync", NETWORK_SYNC_TASK_STACK_SIZE, NULL, 4U,
                                NULL) != pdPASS) {
        services_lock();
        s_sync_scheduled = false;
        services_unlock();
        ESP_LOGW(NETWORK_SERVICES_TAG, "Unable to schedule network-service sync");
    }
}

static void mqtt_message_callback(const mqtt_message_t *message)
{
    if (message == NULL) {
        return;
    }
    ESP_LOGI(NETWORK_SERVICES_TAG, "MQTT message received: topic_len=%d data_len=%d",
             message->topic_len, message->data_len);
}

static void cleanup_http_services(void)
{
    (void)web_dashboard_server_unregister(s_http_server);
    s_dashboard_running = false;
    (void)web_dashboard_server_deinit();
    (void)websocket_server_unregister(s_http_server);
    s_websocket_running = false;
    (void)json_api_server_stop();
    (void)websocket_server_deinit();
    if (s_http_server != NULL) {
        (void)httpd_stop(s_http_server);
        s_http_server = NULL;
    }
}

static esp_err_t start_mqtt_if_enabled(void)
{
    network_mqtt_config_t config;
    services_lock();
    config = s_mqtt_config;
    services_unlock();
    if (!config.enabled || config.broker_url[0] == '\0') {
        return ESP_OK;
    }

    mqtt_config_t mqtt = {
        .broker_url = config.broker_url,
        .client_id = config.client_id,
        .username = config.username[0] != '\0' ? config.username : NULL,
        .password = config.password[0] != '\0' ? config.password : NULL,
        .keepalive_sec = config.keepalive_sec,
    };
    esp_err_t err = mqtt_client_init(&mqtt);
    if (err == ESP_OK) {
        (void)mqtt_client_register_status_callback(mqtt_status_callback);
        (void)mqtt_client_register_message_callback(mqtt_message_callback);
        err = mqtt_client_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(NETWORK_SERVICES_TAG, "MQTT startup failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t network_services_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = mqtt_config_load(&s_mqtt_config);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }
    s_http_server = NULL;
    s_running = false;
    s_mdns_running = false;
    s_websocket_running = false;
    s_dashboard_running = false;
    s_station_ready = false;
    s_sync_scheduled = false;
    const esp_err_t callback_err = wifi_events_register_status_callback(network_wifi_status_callback);
    if (callback_err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return callback_err;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t network_services_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    (void)network_services_stop();
    (void)wifi_events_unregister_status_callback(network_wifi_status_callback);
    services_lock();
    s_initialized = false;
    services_unlock();
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    return ESP_OK;
}

esp_err_t network_services_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    services_lock();
    if (s_running) {
        services_unlock();
        return ESP_OK;
    }
    services_unlock();

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = NETWORK_HTTP_PORT;
    http_config.max_uri_handlers = NETWORK_HTTP_MAX_URI_HANDLERS;
    http_config.stack_size = NETWORK_HTTP_STACK_SIZE;
    http_config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &http_config);
    if (err != ESP_OK) {
        return err;
    }
    s_http_server = server;

    err = web_dashboard_server_init();
    if (err == ESP_OK) {
        err = web_dashboard_server_register(server);
        if (err == ESP_OK) {
            s_dashboard_running = true;
        } else {
            (void)web_dashboard_server_deinit();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(NETWORK_SERVICES_TAG, "Dashboard unavailable: %s", esp_err_to_name(err));
    }

    err = json_api_server_start(server);
    if (err != ESP_OK) {
        cleanup_http_services();
        return err;
    }
    err = websocket_server_init();
    if (err == ESP_OK) {
        err = websocket_server_register(server);
    }
    if (err != ESP_OK) {
        cleanup_http_services();
        return err;
    }
    s_websocket_running = true;

    err = mdns_service_init(WIFI_HOSTNAME);
    if (err == ESP_OK) {
        s_mdns_running = true;
    } else {
        ESP_LOGW(NETWORK_SERVICES_TAG, "mDNS startup failed: %s", esp_err_to_name(err));
    }

    services_lock();
    s_running = true;
    services_unlock();
    (void)start_mqtt_if_enabled();
    ESP_LOGI(NETWORK_SERVICES_TAG, "%s network services started",
             WIFI_COMPILED_OPERATION_MODE_NAME);
    return ESP_OK;
}

esp_err_t network_services_stop(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    services_lock();
    const bool was_running = s_running;
    s_running = false;
    const bool mdns_running = s_mdns_running;
    s_mdns_running = false;
    s_websocket_running = false;
    services_unlock();
    if (!was_running) {
        return ESP_OK;
    }

    (void)mqtt_client_disconnect();
    (void)mqtt_client_deinit();
    if (mdns_running) {
        (void)mdns_service_deinit();
    }
    cleanup_http_services();
    ESP_LOGI(NETWORK_SERVICES_TAG, "%s network services stopped",
             WIFI_COMPILED_OPERATION_MODE_NAME);
    return ESP_OK;
}

bool network_services_is_running(void)
{
    services_lock();
    const bool running = s_running;
    services_unlock();
    return running;
}

void network_services_get_status(network_services_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (!s_initialized) {
        return;
    }
    services_lock();
    status->http_running = s_http_server != NULL;
    status->dashboard_running = s_dashboard_running;
    status->websocket_running = s_websocket_running;
    status->mdns_running = s_mdns_running;
    status->mqtt_configured = s_mqtt_config.enabled && s_mqtt_config.broker_url[0] != '\0';
    services_unlock();
    status->mqtt_connected = mqtt_client_is_connected();
}

esp_err_t network_services_get_mqtt_config(network_mqtt_config_t *config)
{
    if (config == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    services_lock();
    *config = s_mqtt_config;
    services_unlock();
    return ESP_OK;
}

esp_err_t network_services_set_mqtt_config(const network_mqtt_config_t *config)
{
    if (!s_initialized || !mqtt_config_validate(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t save_err = mqtt_config_save(config);
    if (save_err != ESP_OK) {
        return save_err;
    }

    services_lock();
    s_mqtt_config = *config;
    const bool running = s_running;
    services_unlock();
    if (running) {
        (void)mqtt_client_disconnect();
        (void)mqtt_client_deinit();
        (void)start_mqtt_if_enabled();
    }
    return ESP_OK;
}

esp_err_t network_services_mqtt_connect(void)
{
    if (!network_services_is_running()) {
        return ESP_ERR_INVALID_STATE;
    }
    network_mqtt_config_t config;
    if (network_services_get_mqtt_config(&config) != ESP_OK ||
        !config.enabled || config.broker_url[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    return start_mqtt_if_enabled();
}

esp_err_t network_services_mqtt_disconnect(void)
{
    return mqtt_client_disconnect();
}

int network_services_mqtt_publish(const char *topic, const char *data, int qos, bool retain)
{
    char default_topic[NETWORK_MQTT_TOPIC_MAX] = {0};
    if (topic == NULL || topic[0] == '\0') {
        network_mqtt_config_t config;
        if (network_services_get_mqtt_config(&config) != ESP_OK) {
            return -1;
        }
        strncpy(default_topic, config.publish_topic, sizeof(default_topic) - 1U);
        topic = default_topic;
    }
    if (data == NULL || topic[0] == '\0' || qos < 0 || qos > 2) {
        return -1;
    }
    return mqtt_client_publish(topic, data, qos, retain ? 1 : 0);
}

int network_services_mqtt_subscribe(const char *topic, int qos)
{
    char default_topic[NETWORK_MQTT_TOPIC_MAX] = {0};
    if (topic == NULL || topic[0] == '\0') {
        network_mqtt_config_t config;
        if (network_services_get_mqtt_config(&config) != ESP_OK) {
            return -1;
        }
        strncpy(default_topic, config.subscribe_topic, sizeof(default_topic) - 1U);
        topic = default_topic;
    }
    if (topic[0] == '\0' || qos < 0 || qos > 2) {
        return -1;
    }
    return mqtt_client_subscribe(topic, qos);
}
