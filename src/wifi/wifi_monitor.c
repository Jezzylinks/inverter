/**
 * @file wifi_monitor.c
 * @brief Wi-Fi Runtime Monitor
 */

#include "wifi/wifi_monitor.h"
#include "system/core_affinity.h"
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "esp_log.h"
#include "wifi/wifi_events.h"
#include "system/task_watchdog.h"

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

#define WIFI_MONITOR_CONNECT_TIMEOUT_MS 1500U

static bool wifi_monitor_tcp_connect_test(const char *address, uint16_t port);

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
 * Internet reachability
 *
 * Use outbound TCP/443 rather than ICMP. Some networks and ISPs block
 * ICMP while normal HTTPS traffic remains available.
 *---------------------------------------------------------*/

static bool wifi_monitor_tcp_connect_test(const char *address, uint16_t port)
{
    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_port = htons(port);

    if (inet_pton(AF_INET, address, &server.sin_addr) != 1) {
        return false;
    }

    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return false;
    }

    const int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sock);
        return false;
    }

    const int result = connect(
        sock,
        (struct sockaddr *)&server,
        sizeof(server));

    if (result == 0) {
        close(sock);
        return true;
    }

    if (errno != EINPROGRESS) {
        close(sock);
        return false;
    }

    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(sock, &write_set);

    struct timeval timeout = {
        .tv_sec = WIFI_MONITOR_CONNECT_TIMEOUT_MS / 1000U,
        .tv_usec = (WIFI_MONITOR_CONNECT_TIMEOUT_MS % 1000U) * 1000U
    };

    const int selected = select(
        sock + 1,
        NULL,
        &write_set,
        NULL,
        &timeout);

    if (selected <= 0 || !FD_ISSET(sock, &write_set)) {
        close(sock);
        return false;
    }

    int socket_error = 0;
    socklen_t socket_error_len = sizeof(socket_error);

    if (getsockopt(
            sock,
            SOL_SOCKET,
            SO_ERROR,
            &socket_error,
            &socket_error_len) < 0) {
        close(sock);
        return false;
    }

    close(sock);
    return socket_error == 0;
}

/*
 * Two independent public HTTPS endpoints are tested. A temporary failure
 * at one endpoint therefore does not make the whole Internet appear down.
 */
static bool wifi_monitor_internet_test(void)
{
    static const char *const targets[] = {
        "1.1.1.1",
        "8.8.8.8"
    };

    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        if (!s_running) {
            return false;
        }

        if (wifi_monitor_tcp_connect_test(targets[i], 443U)) {
            ESP_LOGI(TAG, "Internet TCP test: %s:443 reachable", targets[i]);
            return true;
        }
    }

    ESP_LOGW(TAG, "Internet TCP test: no public HTTPS endpoint reachable");
    return false;
}

static wifi_internet_status_t wifi_monitor_check_internet(void)
{
    return wifi_monitor_internet_test()
               ? WIFI_INTERNET_AVAILABLE
               : WIFI_INTERNET_UNAVAILABLE;
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

        /* Keep the shared Wi-Fi event snapshot in sync with the independent
         * internet probe. A DHCP lease alone must not imply internet access. */
        (void)wifi_events_set_internet_available(
            got_ip && internet == WIFI_INTERNET_AVAILABLE);

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
        xTaskCreatePinnedToCore(
            wifi_monitor_task,
            "wifi_monitor",
            WIFI_MONITOR_TASK_STACK_SIZE,
            NULL,
            WIFI_MONITOR_TASK_PRIORITY,
            &s_monitor_task, APP_CORE_SYSTEM);

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
        /* Monitor shutdown is a bounded wait, but it runs in the caller's
         * context. Keep the caller visible to the shared TWDT while the
         * monitor exits, without changing the timeout or safety behavior. */
        task_watchdog_feed();
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
