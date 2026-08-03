/**
 * @file mqtt_client.h
 * @brief MQTT Client Interface
 */

#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include "esp_err.h"

    typedef enum
    {
        MQTT_STATUS_DISCONNECTED = 0,
        MQTT_STATUS_CONNECTED,
        MQTT_STATUS_ERROR,
    } mqtt_status_t;

    typedef struct
    {
        const char *topic;
        int topic_len;
        const char *data;
        int data_len;
    } mqtt_message_t;

    typedef struct
    {
        const char *broker_url;
        const char *client_id;
        const char *username;
        const char *password;
        int keepalive_sec;
    } mqtt_config_t;

    typedef void (*mqtt_message_callback_t)(const mqtt_message_t *message);
    typedef void (*mqtt_status_callback_t)(mqtt_status_t status);

    /**
     * @brief Initialize MQTT client
     * @param config MQTT configuration
     * @return ESP_OK on success
     */
    esp_err_t mqtt_client_init(const mqtt_config_t *config);

    /**
     * @brief Deinitialize MQTT client
     * @return ESP_OK on success
     */
    esp_err_t mqtt_client_deinit(void);

    /**
     * @brief Connect to MQTT broker
     * @return ESP_OK on success
     */
    esp_err_t mqtt_client_connect(void);

    /**
     * @brief Disconnect from MQTT broker
     * @return ESP_OK on success
     */
    esp_err_t mqtt_client_disconnect(void);

    /**
     * @brief Publish message
     * @param topic Topic string
     * @param data Message payload
     * @param qos QoS level (0-2)
     * @param retain Retain flag
     * @return Message ID or -1 on error
     */
    int mqtt_client_publish(const char *topic, const char *data, int qos, int retain);

    /**
     * @brief Subscribe to topic
     * @param topic Topic string
     * @param qos QoS level (0-2)
     * @return Message ID or -1 on error
     */
    int mqtt_client_subscribe(const char *topic, int qos);

    /**
     * @brief Unsubscribe from topic
     * @param topic Topic string
     * @return Message ID or -1 on error
     */
    int mqtt_client_unsubscribe(const char *topic);

    /**
     * @brief Check if connected to broker
     * @return true if connected
     */
    bool mqtt_client_is_connected(void);

    /**
     * @brief Register message callback
     * @param callback Called on incoming messages
     * @return ESP_OK on success
     */
    esp_err_t mqtt_client_register_message_callback(mqtt_message_callback_t callback);

    /**
     * @brief Register status callback
     * @param callback Called on connection state changes
     * @return ESP_OK on success
     */
    esp_err_t mqtt_client_register_status_callback(mqtt_status_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CLIENT_H */