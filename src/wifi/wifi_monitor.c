/**
 * @file wifi_monitor.c
 * @brief Wi-Fi Runtime Monitor
 */

#include "wifi_monitor.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <netdb.h>

#include "lwip/sockets.h"
#include "ping/ping_sock.h"
#include "esp_netif.h"

#define WIFI_MONITOR_MAX_CALLBACKS 8

static const char *TAG = "WIFI_MONITOR";

/*==========================================================
 *
 *              PRIVATE DATA
 *
 *=========================================================*/

static TaskHandle_t s_monitor_task = NULL;

static SemaphoreHandle_t s_mutex = NULL;

static bool s_running = false;

static wifi_monitor_status_t s_status;

static wifi_internet_callback_t
    s_internet_callback = NULL;

static wifi_internet_status_t
    s_last_internet_state =
        WIFI_INTERNET_UNKNOWN;

static wifi_monitor_callback_t
    s_callbacks[WIFI_MONITOR_MAX_CALLBACKS];

/*==========================================================
 *
 *              PRIVATE FUNCTIONS
 *
 *=========================================================*/

static bool wifi_monitor_dns_test(void);
static bool wifi_monitor_ping_test(void);

static void wifi_monitor_notify(void)
{
    wifi_monitor_status_t copy;

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);

        memcpy(&copy,
               &s_status,
               sizeof(copy));

        xSemaphoreGive(s_mutex);
    }
    else
    {
        memcpy(&copy,
               &s_status,
               sizeof(copy));
    }

    for (int i = 0;
         i < WIFI_MONITOR_MAX_CALLBACKS;
         i++)
    {
        if (s_callbacks[i])
        {
            s_callbacks[i](&copy);
        }
    }
}

/*----------------------------------------------------------
 * Check internet availability
 *
 * Simple gateway check.
 * More advanced versions can use DNS/ping.
 *---------------------------------------------------------*/

static void wifi_monitor_check_internet(void)
{
    wifi_internet_status_t new_state;

    if (!wifi_monitor_dns_test())
    {
        new_state =
            WIFI_INTERNET_UNAVAILABLE;
    }
    else if (!wifi_monitor_ping_test())
    {
        new_state =
            WIFI_INTERNET_UNAVAILABLE;
    }
    else
    {
        new_state =
            WIFI_INTERNET_AVAILABLE;
    }

    s_status.internet =
        new_state;

    /*
     * Detect transition
     */

    if (new_state != s_last_internet_state)
    {
        ESP_LOGI(TAG,
                 "Internet state changed: %d",
                 new_state);

        if (s_internet_callback)
        {
            s_internet_callback(new_state);
        }

        s_last_internet_state =
            new_state;
    }
}

static bool wifi_monitor_dns_test(void)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;

    memset(&hints,
           0,
           sizeof(hints));

    hints.ai_family = AF_INET;

    int err =
        getaddrinfo(
            "www.google.com",
            NULL,
            &hints,
            &result);

    if (err != 0)
    {
        ESP_LOGW(TAG,
                 "DNS failed");

        return false;
    }

    freeaddrinfo(result);

    return true;
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

    esp_ping_handle_t ping;

    esp_err_t err =
        esp_ping_new_session(
            &config,
            NULL,
            &ping);

    if (err != ESP_OK)
    {
        return false;
    }

    esp_ping_start(ping);

    vTaskDelay(
        pdMS_TO_TICKS(3000));

    esp_ping_stop(ping);

    esp_ping_delete_session(ping);

    return true;
}

/*==========================================================
 *
 *              MONITOR TASK
 *
 *=========================================================*/

static void wifi_monitor_task(void *arg)
{
    (void)arg;

    while (s_running)
    {
        wifi_ap_record_t ap_info;

        memset(&ap_info,
               0,
               sizeof(ap_info));

        esp_err_t err =
            esp_wifi_sta_get_ap_info(
                &ap_info);

        if (err == ESP_OK)
        {
            if (s_mutex)
            {
                xSemaphoreTake(
                    s_mutex,
                    portMAX_DELAY);
            }

            s_status.connected = true;

            s_status.rssi =
                ap_info.rssi;

            wifi_monitor_check_internet();

            if (s_mutex)
            {
                xSemaphoreGive(
                    s_mutex);
            }
        }
        else
        {
            if (s_mutex)
            {
                xSemaphoreTake(
                    s_mutex,
                    portMAX_DELAY);
            }

            s_status.connected = false;

            s_status.internet =
                WIFI_INTERNET_UNAVAILABLE;

            if (s_mutex)
            {
                xSemaphoreGive(
                    s_mutex);
            }
        }

        s_status.uptime_seconds =
            esp_log_timestamp() / 1000;

        wifi_monitor_notify();

        vTaskDelay(
            pdMS_TO_TICKS(
                WIFI_MONITOR_INTERVAL_MS));
    }

    s_monitor_task = NULL;

    vTaskDelete(NULL);
}

/*==========================================================
 *
 *              INITIALIZATION
 *
 *=========================================================*/

esp_err_t wifi_monitor_init(void)
{
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

/*==========================================================
 *
 *              START / STOP
 *
 *=========================================================*/

esp_err_t wifi_monitor_start(void)
{
    if (s_running)
    {
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
    if (!s_running)
    {
        return ESP_OK;
    }

    s_running = false;

    if (s_monitor_task)
    {
        vTaskDelete(s_monitor_task);

        s_monitor_task = NULL;
    }

    return ESP_OK;
}

esp_err_t wifi_monitor_deinit(void)
{
    wifi_monitor_stop();

    if (s_mutex)
    {
        vSemaphoreDelete(s_mutex);

        s_mutex = NULL;
    }

    return ESP_OK;
}

/*==========================================================
 *
 *              STATUS API
 *
 *=========================================================*/

bool wifi_monitor_is_online(void)
{
    return s_status.connected;
}

int8_t wifi_monitor_get_rssi(void)
{
    return s_status.rssi;
}

wifi_internet_status_t
wifi_monitor_get_internet_status(void)
{
    return s_status.internet;
}

const wifi_monitor_status_t *
wifi_monitor_get_status(void)
{
    return &s_status;
}

/*==========================================================
 *
 *              CALLBACK API
 *
 *=========================================================*/

esp_err_t wifi_monitor_register_callback(
    wifi_monitor_callback_t callback)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0;
         i < WIFI_MONITOR_MAX_CALLBACKS;
         i++)
    {
        if (s_callbacks[i] == NULL)
        {
            s_callbacks[i] = callback;

            return ESP_OK;
        }
    }

    return ESP_ERR_NO_MEM;
}

esp_err_t wifi_monitor_unregister_callback(
    wifi_monitor_callback_t callback)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0;
         i < WIFI_MONITOR_MAX_CALLBACKS;
         i++)
    {
        if (s_callbacks[i] == callback)
        {
            s_callbacks[i] = NULL;

            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_monitor_register_internet_callback(
    wifi_internet_callback_t callback)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_internet_callback = callback;

    return ESP_OK;
}