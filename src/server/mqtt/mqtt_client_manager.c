/**
 * @file mqtt_client_manager.c
 * @brief MQTT Client Manager for IoT Cloud Integration
 */

#include "mqtt_client.h"
#include "server/mqtt/mqtt_client_manager.h"
#include "esp_event_base.h"
#include <string.h>

#include "esp_log.h"
#include "mqtt_client.h" /* ESP-IDF MQTT */

static const char *TAG = "MQTT_CLIENT";

static esp_mqtt_client_handle_t s_client = NULL;
static mqtt_message_callback_t s_message_cb = NULL;
static mqtt_status_callback_t s_status_cb = NULL;
static bool s_connected = false;

/*----------------------------------------------------------
 * MQTT Event Handler
 *---------------------------------------------------------*/
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_connected = true;
        if (s_status_cb)
            s_status_cb(MQTT_STATUS_CONNECTED);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_connected = false;
        if (s_status_cb)
            s_status_cb(MQTT_STATUS_DISCONNECTED);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT subscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT unsubscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT published, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT data received on topic: %.*s",
                 event->topic_len, event->topic);
        if (s_message_cb)
        {
            mqtt_message_t msg = {
                .topic = event->topic,
                .topic_len = event->topic_len,
                .data = event->data,
                .data_len = event->data_len,
            };
            s_message_cb(&msg);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        if (s_status_cb)
            s_status_cb(MQTT_STATUS_ERROR);
        break;

    default:
        break;
    }
}

/*----------------------------------------------------------
 * Initialize
 *---------------------------------------------------------*/
esp_err_t mqtt_client_init(const mqtt_config_t *config)
{
    if (config == NULL || config->broker_url == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_client != NULL)
    {
        return ESP_OK;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = config->broker_url,
        .credentials.client_id = config->client_id,
        .credentials.username = config->username,
        .credentials.authentication.password = config->password,
        .session.keepalive = config->keepalive_sec > 0 ? config->keepalive_sec : 60,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 10000,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL)
    {
        ESP_LOGE(TAG, "MQTT client init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(
        s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK)
    {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "MQTT client initialized: %s", config->broker_url);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Deinitialize
 *---------------------------------------------------------*/
esp_err_t mqtt_client_deinit(void)
{
    if (s_client == NULL)
    {
        return ESP_OK;
    }

    mqtt_client_disconnect();

    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;

    ESP_LOGI(TAG, "MQTT client deinitialized");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Connect
 *---------------------------------------------------------*/
esp_err_t mqtt_client_connect(void)
{
    if (s_client == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_mqtt_client_start(s_client);
}

/*----------------------------------------------------------
 * Disconnect
 *---------------------------------------------------------*/
esp_err_t mqtt_client_disconnect(void)
{
    if (s_client == NULL)
    {
        return ESP_OK;
    }

    esp_err_t err = esp_mqtt_client_stop(s_client);
    s_connected = false;

    return err;
}

/*----------------------------------------------------------
 * Publish
 *---------------------------------------------------------*/
int mqtt_client_publish(const char *topic, const char *data, int qos, int retain)
{
    if (s_client == NULL || topic == NULL || data == NULL)
    {
        return -1;
    }

    return esp_mqtt_client_publish(s_client, topic, data, 0, qos, retain);
}

/*----------------------------------------------------------
 * Subscribe
 *---------------------------------------------------------*/
int mqtt_client_subscribe(const char *topic, int qos)
{
    if (s_client == NULL || topic == NULL)
    {
        return -1;
    }

    if (mqtt_client_is_connected() == false)
    {
        return -1;
    }

    return esp_mqtt_client_subscribe(
        s_client,
        (char *)topic,
        qos);
}

/*----------------------------------------------------------
 * Unsubscribe
 *---------------------------------------------------------*/
int mqtt_client_unsubscribe(const char *topic)
{
    if (s_client == NULL || topic == NULL)
    {
        return -1;
    }

    return esp_mqtt_client_unsubscribe(s_client, topic);
}

/*----------------------------------------------------------
 * Is connected?
 *---------------------------------------------------------*/
bool mqtt_client_is_connected(void)
{
    return s_connected;
}

/*----------------------------------------------------------
 * Register callbacks
 *---------------------------------------------------------*/
esp_err_t mqtt_client_register_message_callback(mqtt_message_callback_t callback)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_message_cb = callback;
    return ESP_OK;
}

esp_err_t mqtt_client_register_status_callback(mqtt_status_callback_t callback)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_status_cb = callback;
    return ESP_OK;
}