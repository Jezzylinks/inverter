#ifndef NETWORK_SERVICES_H
#define NETWORK_SERVICES_H

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

typedef struct {
    bool http_running;
    bool websocket_running;
    bool mdns_running;
    bool mqtt_configured;
    bool mqtt_connected;
} network_services_status_t;

esp_err_t network_services_init(void);
esp_err_t network_services_deinit(void);
esp_err_t network_services_start(void);
esp_err_t network_services_stop(void);
bool network_services_is_running(void);
void network_services_get_status(network_services_status_t *status);

esp_err_t network_services_get_mqtt_config(network_mqtt_config_t *config);
esp_err_t network_services_set_mqtt_config(const network_mqtt_config_t *config);
esp_err_t network_services_mqtt_connect(void);
esp_err_t network_services_mqtt_disconnect(void);
int network_services_mqtt_publish(const char *topic, const char *data, int qos, bool retain);
int network_services_mqtt_subscribe(const char *topic, int qos);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_SERVICES_H */
