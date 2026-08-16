/**
 * @file wifi_monitor.c
 * @brief Wi-Fi Runtime Monitor
 */

#include "wifi_monitor.h"
#include <string.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "wifi_events.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"
#include "ping/ping_sock.h"

#define WIFI_MONITOR_MAX_CALLBACKS 8
#define WIFI_MONITOR_STOP_TIMEOUT_MS 5000

static const char *TAG = "WIFI_MONITOR";

/*----------------------------------------------------------
 *
 * PRIVATE DATA
 *
 *---------------------------------------------------------*/

static TaskHandle_t s_monitor_task = NULL;

static SemaphoreHandle_t s_mutex = NULL;

static volatile bool s_running = false;

static wifi_monitor_status_t s_status;

static wifi_internet_callback_t
    s_internet_callback = NULL;

static wifi_internet_status_t
    s_last_internet_state =
        WIFI_INTERNET_UNKNOWN;

static wifi_monitor_callback_t
    s_callbacks[WIFI_MONITOR_MAX_CALLBACKS];

/*----------------------------------------------------------
 *
 * PRIVATE FUNCTIONS
 *
 *---------------------------------------------------------*/

static bool wifi_monitor_ping_test(void);

static void wifi_monitor_notify(void)
{
    wifi_monitor_status_t status_copy;
    wifi_monitor_callback_t callbacks[WIFI_MONITOR_MAX_CALLBACKS];

    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memcpy(&status_copy, &s_status, sizeof(status_copy));
        memcpy(callbacks, s_callbacks, sizeof(callbacks));
        xSemaphoreGive(s_mutex);
    } else {
        memcpy(&status_copy, &s_status, sizeof(status_copy));
        memcpy(callbacks, s_callbacks, sizeof(callbacks));
    }

    for (size_t i = 0; i < WIFI_MONITOR_MAX_CALLBACKS; ++i) {
        if (callbacks[i] != NULL) {
            callbacks[i](&status_copy);
        }
    }
}

/*----------------------------------------------------------
 * Check internet availability
 *
 * Simple gateway check.
 * More advanced versions can use DNS/ping.
 *---------------------------------------------------------*/

static wifi_internet_status_t wifi_monitor_check_internet(void)
{
    /* The ping is performed in a bounded worker path. Avoid getaddrinfo()
     * here: lwIP DNS resolution can block for an uncontrolled interval. */
    return wifi_monitor_ping_test()
               ? WIFI_INTERNET_AVAILABLE
               : WIFI_INTERNET_UNAVAILABLE;
}

static bool wifi_monitor_ping_test(void)
{
    esp_ping_config_t config =
        ESP_PING_DEFAULT_CONFIG();

    ip_addr_t target_addr;

    inet_pton(
        AF_INET,
        "8.8.8.8",
        &target_addr);

    config.target_addr =
        target_addr;
    config.count = 3;         /* Send 3 pings */
    config.interval_ms = 500; /* 500ms between pings */
    config.timeout_ms = 1000; /* 1s timeout per ping */

    esp_ping_callbacks_t cbs = {0};
    uint32_t received = 0;
    cbs.on_ping_success = NULL;
    cbs.on_ping_timeout = NULL;
    cbs.on_ping_end = NULL;

    esp_ping_handle_t ping;

    esp_err_t err =
        esp_ping_new_session(
            &config,
            &cbs,
            &ping);

    if (err != ESP_OK)
    {
        return false;
    }

    esp_ping_start(ping);

    /* Wait in short slices. A stop notification interrupts the probe
     * immediately, so Wi-Fi off/disconnect cannot wait behind a long
     * network operation. */
    uint32_t waited_ms = 0U;
    while (waited_ms < 3500U && s_running) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U)) > 0U) {
            break;
        }
        waited_ms += 100U;
    }
    /* Get statistics */
    uint32_t transmitted = 0;
    esp_ping_get_profile(ping, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(ping, ESP_PING_PROF_REPLY, &received, sizeof(received));

    esp_ping_stop(ping);

    esp_ping_delete_session(ping);

    ESP_LOGI(TAG, "Ping: %lu/%lu received", received, transmitted);

    return (received > 0);
}

/*----------------------------------------------------------
 *
 * MONITOR TASK
 *
 *---------------------------------------------------------*/

static void wifi_monitor_task(void *arg)
{
    (void)arg;

    while (s_running) {

        wifi_status_t event_status = {0};
        const bool have_event_status =
            wifi_events_get_status_copy(&event_status) == ESP_OK;
        const bool connected = have_event_status && event_status.connected;
        const bool got_ip = have_event_status && event_status.got_ip;
        const wifi_internet_status_t internet = got_ip
            ? wifi_monitor_check_internet()
            : WIFI_INTERNET_UNAVAILABLE;
        bool internet_changed = false;
        wifi_internet_callback_t internet_callback = NULL;

        if (s_mutex != NULL) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.connected = connected;
            s_status.got_ip = got_ip;
            s_status.rssi = connected ? event_status.rssi : -127;
            s_status.ip = event_status.ip;
            s_status.internet = internet;
            s_status.uptime_seconds = esp_log_timestamp() / 1000U;
            if (internet != s_last_internet_state) {
                s_last_internet_state = internet;
                internet_changed = true;
                internet_callback = s_internet_callback;
            }
            xSemaphoreGive(s_mutex);
        }

        if (internet_changed) {
            ESP_LOGI(TAG, "Internet state changed: %d", internet);
            if (internet_callback != NULL) {
                internet_callback(internet);
            }
        }
        wifi_monitor_notify();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WIFI_MONITOR_INTERVAL_MS));
    }

    s_monitor_task = NULL;
    vTaskDelete(NULL);
}

/*----------------------------------------------------------
 *
 * INITIALIZATION
 *
 *---------------------------------------------------------*/

esp_err_t wifi_monitor_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;
    }

    s_running = false;
    s_monitor_task = NULL;
    s_last_internet_state = WIFI_INTERNET_UNKNOWN;
    s_internet_callback = NULL;
    memset(&s_status,
           0,
           sizeof(s_status));

    s_status.internet =
        WIFI_INTERNET_UNKNOWN;

    s_mutex =
        xSemaphoreCreateMutex();

    if (s_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    memset(s_callbacks,
           0,
           sizeof(s_callbacks));

    ESP_LOGI(TAG,
             "WiFi monitor initialized");

    return ESP_OK;
}

/*----------------------------------------------------------
 *
 * START / STOP
 *
 *---------------------------------------------------------*/

esp_err_t wifi_monitor_start(void)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running || s_monitor_task != NULL) {
        return ESP_OK;
    }

    s_running = true;

    BaseType_t ret =
        xTaskCreate(
            wifi_monitor_task,
            "wifi_monitor",
            WIFI_MONITOR_TASK_STACK_SIZE,
            NULL,
            WIFI_MONITOR_TASK_PRIORITY,
            &s_monitor_task);

    if (ret != pdPASS)
    {
        s_running = false;

        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t wifi_monitor_stop(void)
{
    s_running = false;
    if (s_monitor_task == NULL) {
        return ESP_OK;
    }

    xTaskNotifyGive(s_monitor_task);
    const int max_waits = WIFI_MONITOR_STOP_TIMEOUT_MS / 50;
    for (int wait = 0; s_monitor_task != NULL && wait < max_waits; ++wait) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return (s_monitor_task == NULL) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_monitor_deinit(void)
{
    const esp_err_t err = wifi_monitor_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Monitor did not stop cleanly: %s", esp_err_to_name(err));
        return err;
    }

    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    memset(s_callbacks, 0, sizeof(s_callbacks));
    s_internet_callback = NULL;
    return ESP_OK;
}

/*----------------------------------------------------------
 *
 * STATUS API
 *
 *---------------------------------------------------------*/

bool wifi_monitor_is_online(void)
{
    if (s_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool online = s_status.connected && s_status.got_ip &&
                        s_status.internet == WIFI_INTERNET_AVAILABLE;
    xSemaphoreGive(s_mutex);
    return online;
}

int8_t wifi_monitor_get_rssi(void)
{
    int8_t rssi;

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        rssi = s_status.rssi;
        xSemaphoreGive(s_mutex);
    }
    else
    {
        rssi = s_status.rssi;
    }

    return rssi;
}

wifi_internet_status_t
wifi_monitor_get_internet_status(void)
{
    wifi_internet_status_t internet;

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        internet = s_status.internet;
        xSemaphoreGive(s_mutex);
    }
    else
    {
        internet = s_status.internet;
    }

    return internet;
}

const wifi_monitor_status_t *
wifi_monitor_get_status(void)
{
    return &s_status;
}

/*----------------------------------------------------------
 *
 * CALLBACK API
 *
 *---------------------------------------------------------*/

esp_err_t wifi_monitor_register_callback(
    wifi_monitor_callback_t callback)
{
    if (callback == NULL || s_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < WIFI_MONITOR_MAX_CALLBACKS; ++i) {
        if (s_callbacks[i] == callback) {
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
        if (s_callbacks[i] == NULL) {
            s_callbacks[i] = callback;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NO_MEM;
}

esp_err_t wifi_monitor_unregister_callback(
    wifi_monitor_callback_t callback)
{
    if (callback == NULL || s_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < WIFI_MONITOR_MAX_CALLBACKS; ++i) {
        if (s_callbacks[i] == callback) {
            s_callbacks[i] = NULL;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_monitor_register_internet_callback(
    wifi_internet_callback_t callback)
{
    if (callback == NULL || s_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_internet_callback = callback;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
