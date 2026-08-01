#include "wifi_events.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wifi_manager.h"

#define WIFI_MAX_CALLBACKS 8

extern bool s_auto_reconnect;
extern uint8_t s_retry_limit;

static const char *TAG = "wifi_events";

/*----------------------------------------------------------
 * Runtime status
 *---------------------------------------------------------*/
static wifi_status_t s_status;

static SemaphoreHandle_t s_mutex = NULL;

/*----------------------------------------------------------
 * Callback list
 *---------------------------------------------------------*/
static wifi_status_callback_t s_callbacks[WIFI_MAX_CALLBACKS];

/*----------------------------------------------------------
 * Event instances
 *---------------------------------------------------------*/
static esp_event_handler_instance_t s_wifi_instance;
static esp_event_handler_instance_t s_ip_instance;
static wifi_event_callback_t s_callback = NULL;

/*----------------------------------------------------------
 * Helpers
 *---------------------------------------------------------*/
static void wifi_notify_callbacks(void)
{
    wifi_status_t copy;

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        copy = s_status;
        xSemaphoreGive(s_mutex);
    }
    else
    {
        copy = s_status;
    }

    for (int i = 0; i < WIFI_MAX_CALLBACKS; i++)
    {
        if (s_callbacks[i] != NULL)
        {
            s_callbacks[i](&copy);
        }
    }
}

static void wifi_set_state(wifi_connection_state_t state)
{
    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.state = state;
        xSemaphoreGive(s_mutex);
    }
    else
    {
        s_status.state = state;
    }

    wifi_notify_callbacks();
}

/*----------------------------------------------------------
 * Initialization
 *---------------------------------------------------------*/
esp_err_t wifi_events_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    memset(s_callbacks, 0, sizeof(s_callbacks));

    s_status.state = WIFI_STATE_IDLE;

    s_mutex = xSemaphoreCreateMutex();

    if (s_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            &s_wifi_instance));

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL,
            &s_ip_instance));

    ESP_LOGI(TAG, "WiFi event system initialized");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Deinitialization
 *---------------------------------------------------------*/
esp_err_t wifi_events_deinit(void)
{
    esp_event_handler_instance_unregister(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        s_wifi_instance);

    esp_event_handler_instance_unregister(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        s_ip_instance);

    if (s_mutex)
    {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    memset(&s_status, 0, sizeof(s_status));
    memset(s_callbacks, 0, sizeof(s_callbacks));

    return ESP_OK;
}

/*----------------------------------------------------------
 * ESP-IDF Event Handler
 *---------------------------------------------------------*/
void wifi_event_handler(void *arg,
                        esp_event_base_t event_base,
                        int32_t event_id,
                        void *event_data)
{
    (void)arg;

    if (s_mutex == NULL)
    {
        return;
    }

    /*==========================
     * Wi-Fi Events
     *=========================*/

    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {

        case WIFI_EVENT_STA_START:
        {
            xSemaphoreTake(
                s_mutex,
                portMAX_DELAY);

            s_status.state =
                WIFI_STATE_CONNECTING;

            s_status.connected = false;

            s_status.got_ip = false;

            xSemaphoreGive(s_mutex);

            ESP_LOGI(TAG,
                     "WiFi started, connecting...");

            esp_wifi_connect();

            wifi_notify_callbacks();

            break;
        }

        case WIFI_EVENT_STA_CONNECTED:
        {
            xSemaphoreTake(
                s_mutex,
                portMAX_DELAY);

            /*
             * Connected to AP,
             * waiting for DHCP/IP
             */

            s_status.state =
                WIFI_STATE_CONNECTING;

            xSemaphoreGive(s_mutex);

            ESP_LOGI(TAG,
                     "Associated with AP");

            wifi_notify_callbacks();

            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t *disc =
                (wifi_event_sta_disconnected_t *)event_data;

            xSemaphoreTake(
                s_mutex,
                portMAX_DELAY);

            s_status.state =
                WIFI_STATE_DISCONNECTED;

            s_status.connected = false;

            s_status.got_ip = false;

            s_status.internet_available = false;

            memset(&s_status.ip,
                   0,
                   sizeof(s_status.ip));

            memset(&s_status.gateway,
                   0,
                   sizeof(s_status.gateway));

            memset(&s_status.netmask,
                   0,
                   sizeof(s_status.netmask));

            s_status.retry_count++;

            bool retry =
                s_auto_reconnect &&
                (s_status.retry_count <
                 s_retry_limit);

            if (!retry)
            {
                s_status.state =
                    WIFI_STATE_FAILED;
            }

            xSemaphoreGive(s_mutex);

            ESP_LOGW(TAG,
                     "Disconnected reason=%d retry=%d/%d",
                     disc->reason,
                     s_status.retry_count,
                     s_retry_limit);

            if (retry)
            {
                ESP_LOGI(TAG,
                         "Reconnecting...");

                vTaskDelay(
                    pdMS_TO_TICKS(
                        WIFI_RECONNECT_DELAY_MS));

                esp_wifi_connect();
            }
            else
            {
                ESP_LOGE(TAG,
                         "WiFi connection failed");
            }

            wifi_notify_callbacks();

            break;
        }

        default:
            break;
        }
    }

    /*==========================
     * IP Events
     *=========================*/

    if (event_base == IP_EVENT)
    {
        switch (event_id)
        {

        case IP_EVENT_STA_GOT_IP:
        {
            ip_event_got_ip_t *event =
                (ip_event_got_ip_t *)event_data;

            xSemaphoreTake(
                s_mutex,
                portMAX_DELAY);

            s_status.state =
                WIFI_STATE_CONNECTED;

            s_status.connected = true;

            s_status.got_ip = true;

            /*
             * Internet test module
             * can update this later
             */
            s_status.internet_available =
                true;

            s_status.retry_count = 0;

            s_status.ip =
                event->ip_info.ip;

            s_status.gateway =
                event->ip_info.gw;

            s_status.netmask =
                event->ip_info.netmask;

            xSemaphoreGive(s_mutex);

            ESP_LOGI(TAG,
                     "Got IP: " IPSTR,
                     IP2STR(
                         &event->ip_info.ip));

            wifi_notify_callbacks();

            break;
        }

        case IP_EVENT_STA_LOST_IP:
        {
            xSemaphoreTake(
                s_mutex,
                portMAX_DELAY);

            s_status.got_ip = false;

            s_status.connected = false;

            s_status.internet_available = false;

            s_status.state =
                WIFI_STATE_DISCONNECTED;

            xSemaphoreGive(s_mutex);

            ESP_LOGW(TAG,
                     "Lost IP");

            wifi_notify_callbacks();

            break;
        }

        default:
            break;
        }
    }
}

/*----------------------------------------------------------
 * Public API
 *---------------------------------------------------------*/

const wifi_status_t *wifi_events_get_status(void)
{
    return &s_status;
}

wifi_connection_state_t wifi_events_get_state(void)
{
    wifi_connection_state_t state;

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        state = s_status.state;
        xSemaphoreGive(s_mutex);
    }
    else
    {
        state = s_status.state;
    }

    return state;
}

bool wifi_events_is_connected(void)
{
    bool connected;

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        connected = s_status.connected;
        xSemaphoreGive(s_mutex);
    }
    else
    {
        connected = s_status.connected;
    }

    return connected;
}

bool wifi_events_has_ip(void)
{
    bool got_ip;

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        got_ip = s_status.got_ip;
        xSemaphoreGive(s_mutex);
    }
    else
    {
        got_ip = s_status.got_ip;
    }

    return got_ip;
}

/*----------------------------------------------------------
 * Callback Registration
 *---------------------------------------------------------*/

esp_err_t wifi_events_register_callback(
    wifi_status_callback_t callback)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Prevent duplicate registration */
    for (int i = 0; i < WIFI_MAX_CALLBACKS; i++)
    {
        if (s_callbacks[i] == callback)
        {
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }

    /* Find a free slot */
    for (int i = 0; i < WIFI_MAX_CALLBACKS; i++)
    {
        if (s_callbacks[i] == NULL)
        {
            s_callbacks[i] = callback;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_mutex);

    return ESP_ERR_NO_MEM;
}

esp_err_t wifi_events_unregister_callback(
    wifi_status_callback_t callback)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int i = 0; i < WIFI_MAX_CALLBACKS; i++)
    {
        if (s_callbacks[i] == callback)
        {
            s_callbacks[i] = NULL;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_mutex);

    return ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_events_register_callback(
    wifi_event_callback_t callback)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_callback = callback;

    return ESP_OK;
}

static void wifi_events_notify(
    wifi_connection_state_t state)
{
    if (s_callback)
    {
        s_callback(state);
    }
}