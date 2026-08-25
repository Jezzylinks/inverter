/**
 * @file wifi_events.c
 * @brief Wi-Fi runtime state and ESP-IDF event integration.
 */
#include "wifi/wifi_events.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi/wifi_config.h"

#define WIFI_EVENTS_TAG "WIFI_EVENTS"
#define WIFI_MAX_CALLBACKS 8
#define WIFI_RECONNECT_TASK_STACK 3072U
#define WIFI_RECONNECT_TASK_PRIORITY 4U

static wifi_status_t s_status;
static wifi_status_callback_t s_status_callbacks[WIFI_MAX_CALLBACKS];
static wifi_event_callback_t s_event_callbacks[WIFI_MAX_CALLBACKS];
static esp_event_handler_instance_t s_wifi_instance;
static esp_event_handler_instance_t s_ip_instance;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_reconnect_task;
static uint32_t s_reconnect_generation;
static bool s_initialized;
static bool s_auto_reconnect = true;
static uint8_t s_retry_limit = WIFI_MAXIMUM_RETRY;

static void events_lock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void events_unlock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

static void wifi_publish_state(wifi_connection_state_t state)
{
    wifi_status_t status = {0};
    wifi_status_callback_t status_callbacks[WIFI_MAX_CALLBACKS];
    wifi_event_callback_t event_callbacks[WIFI_MAX_CALLBACKS];

    events_lock();
    s_status.state = state;
    status = s_status;
    memcpy(status_callbacks, s_status_callbacks, sizeof(status_callbacks));
    memcpy(event_callbacks, s_event_callbacks, sizeof(event_callbacks));
    events_unlock();

    for (size_t i = 0U; i < WIFI_MAX_CALLBACKS; ++i) {
        if (status_callbacks[i] != NULL) {
            status_callbacks[i](&status);
        }
    }
    for (size_t i = 0U; i < WIFI_MAX_CALLBACKS; ++i) {
        if (event_callbacks[i] != NULL) {
            event_callbacks[i](state);
        }
    }
}

static uint32_t wifi_reconnect_backoff_ms(uint8_t retry_count)
{
    uint32_t delay_ms = WIFI_RECONNECT_DELAY_MS;
    const uint8_t shifts = retry_count > 4U ? 4U : retry_count;
    for (uint8_t i = 0U; i < shifts; ++i) {
        if (delay_ms >= 60000U / 2U) {
            return 60000U;
        }
        delay_ms *= 2U;
    }
    return delay_ms;
}

static void wifi_reconnect_task(void *arg)
{
    const uint32_t generation = (uint32_t)(uintptr_t)arg;
    uint32_t waited_ms = 0U;
    uint8_t retry_count = 0U;
    events_lock();
    retry_count = s_status.retry_count;
    events_unlock();
    const uint32_t backoff_ms = wifi_reconnect_backoff_ms(retry_count);
    ESP_LOGI(WIFI_EVENTS_TAG, "Reconnect backoff: %lums (retry %u)",
             (unsigned long)backoff_ms, (unsigned)retry_count);

    while (waited_ms < backoff_ms) {
        const uint32_t slice = (backoff_ms - waited_ms) > 250U
                                   ? 250U
                                   : (backoff_ms - waited_ms);
        vTaskDelay(pdMS_TO_TICKS(slice));
        waited_ms += slice;

        events_lock();
        const bool cancelled = !s_initialized || !s_auto_reconnect ||
                               generation != s_reconnect_generation;
        events_unlock();
        if (cancelled) {
            events_lock();
            if (s_reconnect_task == xTaskGetCurrentTaskHandle()) {
                s_reconnect_task = NULL;
            }
            events_unlock();
            vTaskDelete(NULL);
        }
    }

    events_lock();
    const bool should_connect = s_initialized && s_auto_reconnect &&
                                generation == s_reconnect_generation &&
                                s_status.state == WIFI_STATE_DISCONNECTED &&
                                s_status.retry_count < s_retry_limit;
    if (s_reconnect_task == xTaskGetCurrentTaskHandle()) {
        s_reconnect_task = NULL;
    }
    events_unlock();

    if (should_connect) {
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
            ESP_LOGW(WIFI_EVENTS_TAG, "Reconnect request failed: %s", esp_err_to_name(err));
        }
    }
    vTaskDelete(NULL);
}

static void wifi_schedule_reconnect(void)
{
    events_lock();
    const bool should_schedule = s_initialized && s_auto_reconnect &&
                                 s_status.retry_count < s_retry_limit &&
                                 s_reconnect_task == NULL;
    if (should_schedule) {
        const BaseType_t result = xTaskCreate(wifi_reconnect_task, "wifi_reconnect",
                                              WIFI_RECONNECT_TASK_STACK,
                                              (void *)(uintptr_t)s_reconnect_generation,
                                              WIFI_RECONNECT_TASK_PRIORITY, &s_reconnect_task);
        if (result != pdPASS) {
            s_reconnect_task = NULL;
        }
    }
    events_unlock();

    if (should_schedule && s_reconnect_task == NULL) {
        ESP_LOGW(WIFI_EVENTS_TAG, "Unable to schedule reconnect task");
    }
}

void wifi_events_set_retry_policy(bool enabled, uint8_t retry_limit)
{
    events_lock();
    const bool was_enabled = s_auto_reconnect;
    s_auto_reconnect = enabled;
    s_retry_limit = retry_limit;
    if (enabled && !was_enabled) {
        s_status.retry_count = 0U;
    }
    if (!enabled) {
        ++s_reconnect_generation;
    }
    events_unlock();
}

esp_err_t wifi_events_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_status, 0, sizeof(s_status));
    memset(s_status_callbacks, 0, sizeof(s_status_callbacks));
    memset(s_event_callbacks, 0, sizeof(s_event_callbacks));
    s_status.state = WIFI_STATE_IDLE;
    s_reconnect_generation = 0U;

    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL,
                                                         &s_wifi_instance);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, NULL, &s_ip_instance);
    if (err != ESP_OK) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_instance);
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_events_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_instance);
    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_instance);

    events_lock();
    ++s_reconnect_generation;
    s_auto_reconnect = false;
    TaskHandle_t reconnect_task = s_reconnect_task;
    s_reconnect_task = NULL;
    s_initialized = false;
    events_unlock();
    if (reconnect_task != NULL) {
        vTaskDelete(reconnect_task);
    }

    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    memset(&s_status, 0, sizeof(s_status));
    memset(s_status_callbacks, 0, sizeof(s_status_callbacks));
    memset(s_event_callbacks, 0, sizeof(s_event_callbacks));
    return ESP_OK;
}

void wifi_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    (void)arg;
    if (!s_initialized || s_mutex == NULL) {
        return;
    }

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            events_lock();
            s_status.connected = false;
            s_status.got_ip = false;
            events_unlock();
            /* Radio startup is not a station-connect request. The manager
             * issues esp_wifi_connect() only after an explicit user action or
             * a reconnect task. Leave APSTA's AP_ACTIVE state untouched. */
            break;
        case WIFI_EVENT_STA_CONNECTED:
            events_lock();
            s_status.connected = true;
            s_status.retry_count = 0U;
            events_unlock();
            wifi_publish_state(WIFI_STATE_CONNECTING);
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            uint8_t retry_count;
            bool retry;
            events_lock();
            s_status.connected = false;
            s_status.got_ip = false;
            s_status.internet_available = false;
            memset(&s_status.ip, 0, sizeof(s_status.ip));
            memset(&s_status.gateway, 0, sizeof(s_status.gateway));
            memset(&s_status.netmask, 0, sizeof(s_status.netmask));
            if (s_status.retry_count < UINT8_MAX) {
                ++s_status.retry_count;
            }
            retry_count = s_status.retry_count;
            retry = s_auto_reconnect && retry_count < s_retry_limit;
            events_unlock();
            wifi_publish_state(retry ? WIFI_STATE_DISCONNECTED : WIFI_STATE_FAILED);
            if (event_data != NULL) {
                const wifi_event_sta_disconnected_t *disc = event_data;
                ESP_LOGW(WIFI_EVENTS_TAG, "STA disconnect reason=%d retry=%u/%u",
                         disc->reason, (unsigned)retry_count, (unsigned)s_retry_limit);
            }
            if (retry) {
                wifi_schedule_reconnect();
            }
            break;
        }
        case WIFI_EVENT_STA_STOP:
            events_lock();
            s_status.connected = false;
            s_status.got_ip = false;
            s_status.internet_available = false;
            events_unlock();
            wifi_publish_state(WIFI_STATE_IDLE);
            break;
        case WIFI_EVENT_AP_START:
            wifi_publish_state(WIFI_STATE_AP_ACTIVE);
            break;
        case WIFI_EVENT_AP_STOP:
            if (!wifi_events_is_connected()) {
                wifi_publish_state(WIFI_STATE_IDLE);
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            if (event_data != NULL) {
                const wifi_event_ap_staconnected_t *evt = event_data;
                ESP_LOGI(WIFI_EVENTS_TAG, "Portal client " MACSTR " joined", MAC2STR(evt->mac));
            }
            break;
        default:
            break;
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP && event_data != NULL) {
        const ip_event_got_ip_t *event = event_data;
        events_lock();
        s_status.connected = true;
        s_status.got_ip = true;
        /* DHCP proves local network access, not internet reachability. The
         * monitor owns the internet-availability result. */
        s_status.internet_available = false;
        s_status.retry_count = 0U;
        s_status.ip = event->ip_info.ip;
        s_status.gateway = event->ip_info.gw;
        s_status.netmask = event->ip_info.netmask;
        events_unlock();
        wifi_publish_state(WIFI_STATE_CONNECTED);
        ESP_LOGI(WIFI_EVENTS_TAG, "Station address " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

const wifi_status_t *wifi_events_get_status(void)
{
    return &s_status;
}

esp_err_t wifi_events_get_status_copy(wifi_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    events_lock();
    *status = s_status;
    events_unlock();
    return s_initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

wifi_connection_state_t wifi_events_get_state(void)
{
    wifi_status_t status;
    return wifi_events_get_status_copy(&status) == ESP_OK ? status.state : WIFI_STATE_IDLE;
}

bool wifi_events_is_connected(void)
{
    wifi_status_t status;
    return wifi_events_get_status_copy(&status) == ESP_OK && status.connected;
}

bool wifi_events_has_ip(void)
{
    wifi_status_t status;
    return wifi_events_get_status_copy(&status) == ESP_OK && status.got_ip;
}

static esp_err_t wifi_callbacks_register(void **callbacks, void *callback)
{
    if (callback == NULL || !s_initialized) {
        return callback == NULL ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    events_lock();
    for (size_t i = 0U; i < WIFI_MAX_CALLBACKS; ++i) {
        if (callbacks[i] == callback) {
            events_unlock();
            return ESP_OK;
        }
    }
    for (size_t i = 0U; i < WIFI_MAX_CALLBACKS; ++i) {
        if (callbacks[i] == NULL) {
            callbacks[i] = callback;
            events_unlock();
            return ESP_OK;
        }
    }
    events_unlock();
    return ESP_ERR_NO_MEM;
}

static esp_err_t wifi_callbacks_unregister(void **callbacks, void *callback)
{
    if (callback == NULL || !s_initialized) {
        return callback == NULL ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    events_lock();
    for (size_t i = 0U; i < WIFI_MAX_CALLBACKS; ++i) {
        if (callbacks[i] == callback) {
            callbacks[i] = NULL;
            events_unlock();
            return ESP_OK;
        }
    }
    events_unlock();
    return ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_events_register_status_callback(wifi_status_callback_t callback)
{
    return wifi_callbacks_register((void **)s_status_callbacks, (void *)callback);
}

esp_err_t wifi_events_unregister_status_callback(wifi_status_callback_t callback)
{
    return wifi_callbacks_unregister((void **)s_status_callbacks, (void *)callback);
}

esp_err_t wifi_events_register_event_callback(wifi_event_callback_t callback)
{
    return wifi_callbacks_register((void **)s_event_callbacks, (void *)callback);
}

esp_err_t wifi_events_unregister_event_callback(wifi_event_callback_t callback)
{
    return wifi_callbacks_unregister((void **)s_event_callbacks, (void *)callback);
}
