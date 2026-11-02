/**
 * @file wifi_events.c
 * @brief Wi-Fi runtime state and ESP-IDF event integration.
 */
#include "wifi/wifi_events.h"
#include "system/core_affinity.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi/wifi_config.h"
#include "events/system_events.h"

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
static uint32_t s_reconnect_initial_delay_ms = WIFI_CONNECT_TIMEOUT_MS;
static bool s_initialized;
static bool s_ap_active;
static bool s_auto_reconnect = true;
/* Kept for the policy API. The background search no longer stops at this
 * limit; the foreground Connect operation still uses it (via
 * wifi_manager_get_retry_limit()) to decide a request has failed. */
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
    wifi_connection_state_t previous_state;
    bool state_changed;

    events_lock();
    previous_state = s_status.state;
    s_status.state = state;
    state_changed = previous_state != state;
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

    if (state_changed && (state == WIFI_STATE_CONNECTED ||
                          state == WIFI_STATE_AP_ACTIVE ||
                          state == WIFI_STATE_FAILED ||
                          state == WIFI_STATE_DISCONNECTED)) {
        system_event_t event = {
            .category = EVENT_CATEGORY_WIFI,
            .action = (state == WIFI_STATE_CONNECTED || state == WIFI_STATE_AP_ACTIVE)
                          ? EVENT_ACTION_SUCCESS : EVENT_ACTION_ERROR,
            .source = EVENT_SOURCE_WIFI,
            .priority = (state == WIFI_STATE_FAILED) ? EVENT_PRIORITY_HIGH
                                                       : EVENT_PRIORITY_NORMAL,
            .timestamp = xTaskGetTickCount(),
        };
        (void)system_event_post(&event);
    }
}

/* Search interval for a saved network that is out of reach. Constant (not
 * exponential): the goal is to notice quickly when the host comes back, so
 * the delay never grows beyond WIFI_RECONNECT_STEADY_DELAY_MS. */
static uint32_t wifi_reconnect_backoff_ms(uint8_t retry_count)
{
    return retry_count <= WIFI_RECONNECT_QUICK_ATTEMPTS
               ? (uint32_t)WIFI_RECONNECT_DELAY_MS
               : (uint32_t)WIFI_RECONNECT_STEADY_DELAY_MS;
}

static void wifi_reconnect_task(void *arg)
{
    const uint32_t generation = (uint32_t)(uintptr_t)arg;
    events_lock();
    uint32_t delay_ms = s_reconnect_initial_delay_ms;
    events_unlock();

    /*
     * Keep one low-priority supervisor alive while auto-reconnect is enabled.
     * A failed esp_wifi_connect() normally produces STA_DISCONNECTED, but the
     * driver can also finish a scan without a useful AP and leave no further
     * event to trigger the old one-shot retry task. The bounded attempt wait
     * below guarantees another search even in that case.
     */
    for (;;)
    {
        uint32_t waited_ms = 0U;
        while (waited_ms < delay_ms)
        {
            const uint32_t remaining = delay_ms - waited_ms;
            const uint32_t slice = remaining > 250U ? 250U : remaining;
            const uint32_t notified =
                (uint32_t)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(slice));

            events_lock();
            const bool cancelled = !s_initialized || !s_auto_reconnect ||
                                   generation != s_reconnect_generation;
            const bool connected = s_status.connected;
            const uint8_t retry_count = s_status.retry_count;
            events_unlock();

            if (cancelled || connected)
            {
                goto reconnect_done;
            }

            if (notified > 0U)
            {
                /* A disconnect event arrived while waiting. Start a fresh
                 * bounded backoff from that event rather than waiting out a
                 * stale startup guard. */
                delay_ms = wifi_reconnect_backoff_ms(retry_count);
                waited_ms = 0U;
            }
            else
            {
                waited_ms += slice;
            }
        }

        events_lock();
        const bool cancelled = !s_initialized || !s_auto_reconnect ||
                               generation != s_reconnect_generation;
        const bool connected = s_status.connected;
        const uint8_t retry_before = s_status.retry_count;
        const wifi_connection_state_t state = s_status.state;
        /* IDLE is valid immediately after STA_START; AP_ACTIVE is the normal
         * published state in APSTA while the station is searching. */
        const bool station_may_connect =
            state == WIFI_STATE_IDLE ||
            state == WIFI_STATE_DISCONNECTED ||
            state == WIFI_STATE_FAILED ||
            state == WIFI_STATE_AP_ACTIVE;
        events_unlock();

        if (cancelled || connected)
        {
            goto reconnect_done;
        }

        if (!station_may_connect)
        {
            delay_ms = wifi_reconnect_backoff_ms(retry_before);
            continue;
        }

        const esp_err_t connect_err = esp_wifi_connect();
        if (connect_err != ESP_OK && connect_err != ESP_ERR_WIFI_STATE &&
            connect_err != ESP_ERR_WIFI_CONN)
        {
            ESP_LOGW(WIFI_EVENTS_TAG, "Reconnect request failed: %s",
                     esp_err_to_name(connect_err));
        }
        else
        {
            ESP_LOGI(WIFI_EVENTS_TAG,
                     "Searching for saved station network (attempt %u)",
                     (unsigned)(retry_before + 1U));
        }

        uint32_t attempt_waited_ms = 0U;
        bool attempt_failed = false;
        while (attempt_waited_ms < (uint32_t)WIFI_CONNECT_TIMEOUT_MS)
        {
            const uint32_t slice =
                ((uint32_t)WIFI_CONNECT_TIMEOUT_MS - attempt_waited_ms) > 250U
                    ? 250U
                    : ((uint32_t)WIFI_CONNECT_TIMEOUT_MS - attempt_waited_ms);
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(slice));
            attempt_waited_ms += slice;

            events_lock();
            const bool stop = !s_initialized || !s_auto_reconnect ||
                              generation != s_reconnect_generation;
            const bool now_connected = s_status.connected;
            const bool retry_event_seen = s_status.retry_count != retry_before;
            const uint8_t retry_count = s_status.retry_count;
            events_unlock();

            if (stop || now_connected)
            {
                goto reconnect_done;
            }
            if (retry_event_seen)
            {
                attempt_failed = true;
                delay_ms = wifi_reconnect_backoff_ms(retry_count);
                break;
            }
        }

        if (!attempt_failed)
        {
            events_lock();
            const bool still_enabled = s_initialized && s_auto_reconnect &&
                                       generation == s_reconnect_generation &&
                                       !s_status.connected;
            const uint8_t retry_count = s_status.retry_count;
            events_unlock();

            if (!still_enabled)
            {
                goto reconnect_done;
            }

            /* No connect/disconnect event arrived within the bounded window.
             * Abort any stale driver-side attempt before the next search. */
            ESP_LOGW(WIFI_EVENTS_TAG,
                     "Station search timed out without a completion event; restarting search");
            (void)esp_wifi_disconnect();
            delay_ms = wifi_reconnect_backoff_ms(retry_count);
        }
    }

reconnect_done:
    events_lock();
    if (s_reconnect_task == xTaskGetCurrentTaskHandle())
    {
        s_reconnect_task = NULL;
    }
    events_unlock();
    vTaskDelete(NULL);
}

/* Keep the task handle protected by the event mutex. If a worker already
 * exists, notify it so a new disconnect can shorten its current wait. */
static void wifi_schedule_reconnect(uint32_t delay_ms)
{
    events_lock();
    if (!s_initialized || !s_auto_reconnect)
    {
        events_unlock();
        return;
    }

    if (s_reconnect_task != NULL)
    {
        xTaskNotifyGive(s_reconnect_task);
        events_unlock();
        return;
    }

    const BaseType_t result =
        xTaskCreatePinnedToCore(wifi_reconnect_task, "wifi_reconnect",
                                WIFI_RECONNECT_TASK_STACK,
                                (void *)(uintptr_t)s_reconnect_generation,
                                WIFI_RECONNECT_TASK_PRIORITY,
                                &s_reconnect_task, APP_CORE_SYSTEM);
    if (result != pdPASS)
    {
        s_reconnect_task = NULL;
        ESP_LOGW(WIFI_EVENTS_TAG, "Unable to schedule reconnect supervisor");
    }
    events_unlock();
}

void wifi_events_set_retry_policy(bool enabled, uint8_t retry_limit)
{
    bool should_schedule = false;
    events_lock();
    const bool was_enabled = s_auto_reconnect;
    s_auto_reconnect = enabled;
    s_retry_limit = retry_limit;
    if (enabled && !was_enabled) {
        s_status.retry_count = 0U;
        should_schedule = s_initialized &&
                          (s_status.state == WIFI_STATE_DISCONNECTED ||
                           s_status.state == WIFI_STATE_AP_ACTIVE ||
                           s_status.state == WIFI_STATE_FAILED);
    }
    if (!enabled) {
        ++s_reconnect_generation;
    }
    events_unlock();

    if (should_schedule) {
        wifi_schedule_reconnect(wifi_reconnect_backoff_ms(0U));
    }
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

    s_ap_active = false;
    memset(&s_status, 0, sizeof(s_status));
    memset(s_status_callbacks, 0, sizeof(s_status_callbacks));
    memset(s_event_callbacks, 0, sizeof(s_event_callbacks));
    s_status.state = WIFI_STATE_IDLE;
    s_reconnect_generation = 0U;
    s_ap_active = false;

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
            /* The controller makes the first connection request after start.
             * Arm a delayed supervisor as a fallback in case that first scan
             * fails without producing a follow-up DISCONNECTED event. */
            wifi_schedule_reconnect((uint32_t)WIFI_CONNECT_TIMEOUT_MS);
            break;
        case WIFI_EVENT_STA_CONNECTED:
            events_lock();
            s_status.connected = true;
            s_status.retry_count = 0U;
            if (s_reconnect_task != NULL) {
                xTaskNotifyGive(s_reconnect_task);
            }
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
            /* retry_count keeps counting (saturating) so the foreground
             * Connect operation can still call a request failed once it
             * reaches the retry limit, but the background search never
             * stops while auto-reconnect is enabled. While searching the
             * state stays DISCONNECTED (not FAILED): an unavailable saved
             * network is a normal, silent condition. */
            retry = s_auto_reconnect;
            events_unlock();
            if (s_ap_active) {
                wifi_publish_state(WIFI_STATE_AP_ACTIVE);
            } else {
                wifi_publish_state(retry ? WIFI_STATE_DISCONNECTED : WIFI_STATE_FAILED);
            }
            if (event_data != NULL) {
                const wifi_event_sta_disconnected_t *disc = event_data;
                /* Warn once per outage; repeats of the same search are debug
                 * only so a long outage does not flood the log. */
                if (retry_count <= 1U) {
                    ESP_LOGW(WIFI_EVENTS_TAG,
                             "STA disconnect reason=%d; searching for saved network",
                             disc->reason);
                } else {
                    ESP_LOGD(WIFI_EVENTS_TAG, "STA disconnect reason=%d attempt=%u",
                             disc->reason, (unsigned)retry_count);
                }
            }
            if (retry) {
                wifi_schedule_reconnect(wifi_reconnect_backoff_ms(retry_count));
            }
            break;
        }
        case WIFI_EVENT_STA_STOP: {
            bool ap_active;
            events_lock();
            s_status.connected = false;
            s_status.got_ip = false;
            s_status.internet_available = false;
            ap_active = s_ap_active;
            events_unlock();
            wifi_publish_state(ap_active ? WIFI_STATE_AP_ACTIVE : WIFI_STATE_IDLE);
            break;
        }
        case WIFI_EVENT_AP_START:
            events_lock();
            s_ap_active = true;
            events_unlock();
            wifi_publish_state(WIFI_STATE_AP_ACTIVE);
            break;
        case WIFI_EVENT_AP_STOP: {
            bool sta_connected = wifi_events_is_connected();
            events_lock();
            s_ap_active = false;
            events_unlock();
            if (!sta_connected) {
                wifi_publish_state(WIFI_STATE_IDLE);
            }
            break;
        }
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
        if (s_reconnect_task != NULL) {
            xTaskNotifyGive(s_reconnect_task);
        }
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

bool wifi_events_is_ap_active(void)
{
    if (!s_initialized || s_mutex == NULL) {
        return false;
    }
    events_lock();
    const bool active = s_ap_active;
    events_unlock();
    return active;
}

bool wifi_events_has_ip(void)
{
    wifi_status_t status;
    return wifi_events_get_status_copy(&status) == ESP_OK && status.got_ip;
}

esp_err_t wifi_events_set_internet_available(bool available)
{
    if (!s_initialized || s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_status_t status_copy = {0};
    wifi_status_callback_t callbacks[WIFI_MAX_CALLBACKS] = {0};
    bool changed = false;

    events_lock();
    /* Never report internet access unless the station link and IP lease are
     * both valid. This state is measured separately by wifi_monitor. */
    const bool effective = available && s_status.connected && s_status.got_ip;
    changed = s_status.internet_available != effective;
    if (changed) {
        s_status.internet_available = effective;
        status_copy = s_status;
        memcpy(callbacks, s_status_callbacks, sizeof(callbacks));
    }
    events_unlock();

    if (changed) {
        for (size_t i = 0U; i < WIFI_MAX_CALLBACKS; ++i) {
            if (callbacks[i] != NULL) {
                callbacks[i](&status_copy);
            }
        }
        ESP_LOGI(WIFI_EVENTS_TAG, "Internet availability: %s",
                 status_copy.internet_available ? "available" : "unavailable");
    }
    return ESP_OK;
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
