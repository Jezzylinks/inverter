#include "network_services.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "system_state.h"

#include "ota/json_api_server.h"
#include "ota/mdns_service.h"
#include "ota/mqtt_client_manager.h"
#include "ota/websocket_server.h"
#include "ota/web_dashboard_server.h"
#include "wifi/wifi_config.h"
#include "wifi/wifi_events.h"
#include "task_watchdog.h"

#define NETWORK_SERVICES_TAG "NET_SERVICES"
#define NETWORK_SERVICES_NVS_NAMESPACE NVS_NS_SYSTEM
#define NETWORK_MQTT_ENABLED_KEY "mqtt_enabled"
#define NETWORK_MQTT_BROKER_KEY "mqtt_broker"
#define NETWORK_MQTT_CLIENT_ID_KEY "mqtt_client_id"
#define NETWORK_MQTT_USERNAME_KEY "mqtt_user"
#define NETWORK_MQTT_PASSWORD_KEY "mqtt_pass"
#define NETWORK_MQTT_PUB_TOPIC_KEY "mqtt_pub_topic"
#define NETWORK_MQTT_SUB_TOPIC_KEY "mqtt_sub_topic"
#define NETWORK_MQTT_KEEPALIVE_KEY "mqtt_keepalive"
#define NETWORK_MQTT_QOS_KEY "mqtt_qos"
#define NETWORK_MQTT_RETAIN_KEY "mqtt_retain"
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

static bool valid_text(const char *value, size_t capacity, bool required)
{
    if (value == NULL) {
        return !required;
    }
    const size_t length = strnlen(value, capacity);
    if (length >= capacity || (required && length == 0U)) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        const unsigned char c = (unsigned char)value[i];
        if (c < 0x20U || c == 0x7FU) {
            return false;
        }
    }
    return true;
}

static bool valid_broker_url(const char *url)
{
    return url != NULL &&
           (strncmp(url, "mqtt://", 7U) == 0 || strncmp(url, "mqtts://", 8U) == 0) &&
           strlen(url) < NETWORK_MQTT_BROKER_MAX;
}

static bool mqtt_config_valid(const network_mqtt_config_t *config)
{
    if (config == NULL || config->keepalive_sec < 10 || config->keepalive_sec > 3600 ||
        config->qos < 0 || config->qos > 2 ||
        !valid_text(config->client_id, sizeof(config->client_id), config->enabled) ||
        !valid_text(config->username, sizeof(config->username), false) ||
        !valid_text(config->password, sizeof(config->password), false) ||
        !valid_text(config->publish_topic, sizeof(config->publish_topic), false) ||
        !valid_text(config->subscribe_topic, sizeof(config->subscribe_topic), false)) {
        return false;
    }
    if (!valid_text(config->broker_url, sizeof(config->broker_url), false) ||
        (config->broker_url[0] != '\0' && !valid_broker_url(config->broker_url)) ||
        (config->enabled && !valid_broker_url(config->broker_url))) {
        return false;
    }
    return true;
}

static void mqtt_config_defaults(network_mqtt_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->enabled = false;
    strncpy(config->client_id, WIFI_HOSTNAME, sizeof(config->client_id) - 1U);
    strncpy(config->publish_topic, "inverter/status", sizeof(config->publish_topic) - 1U);
    strncpy(config->subscribe_topic, "inverter/command", sizeof(config->subscribe_topic) - 1U);
    config->keepalive_sec = 60;
    config->qos = 0;
    config->retain = false;
}

static esp_err_t nvs_get_string(nvs_handle_t handle, const char *key,
                                char *value, size_t capacity)
{
    size_t length = capacity;
    const esp_err_t err = nvs_get_str(handle, key, value, &length);
    if (err != ESP_OK) {
        value[0] = '\0';
    }
    return err;
}

static esp_err_t load_mqtt_config(void)
{
    mqtt_config_defaults(&s_mqtt_config);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NETWORK_SERVICES_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t value = 0U;
    if (nvs_get_u8(handle, NETWORK_MQTT_ENABLED_KEY, &value) == ESP_OK) {
        s_mqtt_config.enabled = value != 0U;
    }
    (void)nvs_get_string(handle, NETWORK_MQTT_BROKER_KEY, s_mqtt_config.broker_url,
                         sizeof(s_mqtt_config.broker_url));
    (void)nvs_get_string(handle, NETWORK_MQTT_CLIENT_ID_KEY, s_mqtt_config.client_id,
                         sizeof(s_mqtt_config.client_id));
    (void)nvs_get_string(handle, NETWORK_MQTT_USERNAME_KEY, s_mqtt_config.username,
                         sizeof(s_mqtt_config.username));
    (void)nvs_get_string(handle, NETWORK_MQTT_PASSWORD_KEY, s_mqtt_config.password,
                         sizeof(s_mqtt_config.password));
    (void)nvs_get_string(handle, NETWORK_MQTT_PUB_TOPIC_KEY, s_mqtt_config.publish_topic,
                         sizeof(s_mqtt_config.publish_topic));
    (void)nvs_get_string(handle, NETWORK_MQTT_SUB_TOPIC_KEY, s_mqtt_config.subscribe_topic,
                         sizeof(s_mqtt_config.subscribe_topic));
    int32_t keepalive = s_mqtt_config.keepalive_sec;
    int32_t qos = s_mqtt_config.qos;
    if (nvs_get_i32(handle, NETWORK_MQTT_KEEPALIVE_KEY, &keepalive) == ESP_OK) {
        s_mqtt_config.keepalive_sec = (int)keepalive;
    }
    if (nvs_get_i32(handle, NETWORK_MQTT_QOS_KEY, &qos) == ESP_OK) {
        s_mqtt_config.qos = (int)qos;
    }
    if (nvs_get_u8(handle, NETWORK_MQTT_RETAIN_KEY, &value) == ESP_OK) {
        s_mqtt_config.retain = value != 0U;
    }
    nvs_close(handle);

    if (!mqtt_config_valid(&s_mqtt_config)) {
        ESP_LOGW(NETWORK_SERVICES_TAG, "Stored MQTT configuration is invalid; MQTT disabled");
        mqtt_config_defaults(&s_mqtt_config);
    }
    return ESP_OK;
}

static esp_err_t save_mqtt_config(const network_mqtt_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NETWORK_SERVICES_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, NETWORK_MQTT_ENABLED_KEY, config->enabled ? 1U : 0U);
    if (err == ESP_OK) err = nvs_set_str(handle, NETWORK_MQTT_BROKER_KEY, config->broker_url);
    if (err == ESP_OK) err = nvs_set_str(handle, NETWORK_MQTT_CLIENT_ID_KEY, config->client_id);
    if (err == ESP_OK) err = nvs_set_str(handle, NETWORK_MQTT_USERNAME_KEY, config->username);
    if (err == ESP_OK) err = nvs_set_str(handle, NETWORK_MQTT_PASSWORD_KEY, config->password);
    if (err == ESP_OK) err = nvs_set_str(handle, NETWORK_MQTT_PUB_TOPIC_KEY, config->publish_topic);
    if (err == ESP_OK) err = nvs_set_str(handle, NETWORK_MQTT_SUB_TOPIC_KEY, config->subscribe_topic);
    if (err == ESP_OK) err = nvs_set_i32(handle, NETWORK_MQTT_KEEPALIVE_KEY, config->keepalive_sec);
    if (err == ESP_OK) err = nvs_set_i32(handle, NETWORK_MQTT_QOS_KEY, config->qos);
    if (err == ESP_OK) err = nvs_set_u8(handle, NETWORK_MQTT_RETAIN_KEY, config->retain ? 1U : 0U);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
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
    task_watchdog_register("network_services_sync");
    (void)arg;

    services_lock();
    const bool ready = s_station_ready;
    const bool running = s_running;
    services_unlock();

    task_watchdog_feed();
    if (ready && !running) {
        (void)network_services_start();
    } else if (!ready && running) {
        (void)network_services_stop();
    }
    task_watchdog_feed();

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
    const bool ready = status->state == WIFI_STATE_CONNECTED && status->got_ip;
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
    esp_err_t err = load_mqtt_config();
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
    task_watchdog_feed();
    esp_err_t err = httpd_start(&server, &http_config);
    if (err != ESP_OK) {
        return err;
    }
    s_http_server = server;
    task_watchdog_feed();

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

    task_watchdog_feed();
    err = json_api_server_start(server);
    if (err != ESP_OK) {
        cleanup_http_services();
        return err;
    }
    task_watchdog_feed();
    err = websocket_server_init();
    if (err == ESP_OK) {
        err = websocket_server_register(server);
    }
    if (err != ESP_OK) {
        cleanup_http_services();
        return err;
    }
    s_websocket_running = true;

    task_watchdog_feed();
    err = mdns_service_init(WIFI_HOSTNAME);
    if (err == ESP_OK) {
        s_mdns_running = true;
    } else {
        ESP_LOGW(NETWORK_SERVICES_TAG, "mDNS startup failed: %s", esp_err_to_name(err));
    }

    services_lock();
    s_running = true;
    services_unlock();
    task_watchdog_feed();
    (void)start_mqtt_if_enabled();
    task_watchdog_feed();
    ESP_LOGI(NETWORK_SERVICES_TAG, "Station network services started");
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
    ESP_LOGI(NETWORK_SERVICES_TAG, "Station network services stopped");
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
    if (!s_initialized || !mqtt_config_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t save_err = save_mqtt_config(config);
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
