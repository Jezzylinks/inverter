#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define NETWORK_MQTT_BROKER_MAX 192U
#define NETWORK_MQTT_CLIENT_ID_MAX 64U
#define NETWORK_MQTT_USERNAME_MAX 64U
#define NETWORK_MQTT_PASSWORD_MAX 128U
#define NETWORK_MQTT_TOPIC_MAX 128U

typedef struct {
    bool enabled;
    char broker_url[NETWORK_MQTT_BROKER_MAX];
    char client_id[NETWORK_MQTT_CLIENT_ID_MAX];
    char username[NETWORK_MQTT_USERNAME_MAX];
    char password[NETWORK_MQTT_PASSWORD_MAX];
    char publish_topic[NETWORK_MQTT_TOPIC_MAX];
    char subscribe_topic[NETWORK_MQTT_TOPIC_MAX];
    int keepalive_sec;
    int qos;
    bool retain;
} network_mqtt_config_t;

void mqtt_config_defaults(network_mqtt_config_t *config);
bool mqtt_config_validate(const network_mqtt_config_t *config);
esp_err_t mqtt_config_load(network_mqtt_config_t *config);
esp_err_t mqtt_config_save(const network_mqtt_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CONFIG_H */
