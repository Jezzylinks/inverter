#include "server/mqtt/mqtt_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "system/system_state.h"
#include "wifi/wifi_config.h"

#define MQTT_CONFIG_TAG "NET_SERVICES"
#define MQTT_CONFIG_NVS_NAMESPACE NVS_NS_SYSTEM
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

bool mqtt_config_validate(const network_mqtt_config_t *config)
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

void mqtt_config_defaults(network_mqtt_config_t *config)
{
    if (config == NULL) {
        return;
    }
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

esp_err_t mqtt_config_load(network_mqtt_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    mqtt_config_defaults(config);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(MQTT_CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t value = 0U;
    if (nvs_get_u8(handle, NETWORK_MQTT_ENABLED_KEY, &value) == ESP_OK) {
        config->enabled = value != 0U;
    }
    (void)nvs_get_string(handle, NETWORK_MQTT_BROKER_KEY, config->broker_url,
                         sizeof(config->broker_url));
    (void)nvs_get_string(handle, NETWORK_MQTT_CLIENT_ID_KEY, config->client_id,
                         sizeof(config->client_id));
    (void)nvs_get_string(handle, NETWORK_MQTT_USERNAME_KEY, config->username,
                         sizeof(config->username));
    (void)nvs_get_string(handle, NETWORK_MQTT_PASSWORD_KEY, config->password,
                         sizeof(config->password));
    (void)nvs_get_string(handle, NETWORK_MQTT_PUB_TOPIC_KEY, config->publish_topic,
                         sizeof(config->publish_topic));
    (void)nvs_get_string(handle, NETWORK_MQTT_SUB_TOPIC_KEY, config->subscribe_topic,
                         sizeof(config->subscribe_topic));
    int32_t keepalive = config->keepalive_sec;
    int32_t qos = config->qos;
    if (nvs_get_i32(handle, NETWORK_MQTT_KEEPALIVE_KEY, &keepalive) == ESP_OK) {
        config->keepalive_sec = (int)keepalive;
    }
    if (nvs_get_i32(handle, NETWORK_MQTT_QOS_KEY, &qos) == ESP_OK) {
        config->qos = (int)qos;
    }
    if (nvs_get_u8(handle, NETWORK_MQTT_RETAIN_KEY, &value) == ESP_OK) {
        config->retain = value != 0U;
    }
    nvs_close(handle);

    if (!mqtt_config_validate(config)) {
        ESP_LOGW(MQTT_CONFIG_TAG, "Stored MQTT configuration is invalid; MQTT disabled");
        mqtt_config_defaults(config);
    }
    return ESP_OK;
}

esp_err_t mqtt_config_save(const network_mqtt_config_t *config)
{
    if (!mqtt_config_validate(config)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(MQTT_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
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
