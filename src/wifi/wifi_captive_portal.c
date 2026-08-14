/**
 * @file wifi_captive_portal.c
 * @brief Transactional captive-portal lifecycle.
 */
#include "wifi_captive_portal.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "wifi_dns_server.h"
#include "wifi_http_server.h"

#define WIFI_CAPTIVE_TAG "WIFI_CAPTIVE"

static bool s_initialized;
static bool s_running;
static SemaphoreHandle_t s_mutex;

static void captive_lock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void captive_unlock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t wifi_captive_portal_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = wifi_dns_server_init();
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }
    s_running = false;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_captive_portal_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    const esp_err_t stop_err = wifi_captive_portal_stop();
    (void)wifi_dns_server_deinit();
    s_initialized = false;
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    return stop_err;
}

esp_err_t wifi_captive_portal_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    captive_lock();
    if (s_running) {
        captive_unlock();
        return ESP_OK;
    }
    captive_unlock();

    esp_err_t err = wifi_http_server_start();
    if (err != ESP_OK) {
        return err;
    }
    err = wifi_dns_server_start();
    if (err != ESP_OK) {
        (void)wifi_http_server_stop();
        return err;
    }

    captive_lock();
    s_running = true;
    captive_unlock();
    ESP_LOGI(WIFI_CAPTIVE_TAG, "Captive portal started");
    return ESP_OK;
}

esp_err_t wifi_captive_portal_stop(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    captive_lock();
    const bool was_running = s_running;
    s_running = false;
    captive_unlock();
    if (!was_running) {
        return ESP_OK;
    }

    esp_err_t first_err = wifi_dns_server_stop();
    const esp_err_t http_err = wifi_http_server_stop();
    if (first_err == ESP_OK) {
        first_err = http_err;
    }
    if (first_err == ESP_OK) {
        ESP_LOGI(WIFI_CAPTIVE_TAG, "Captive portal stopped");
    }
    return first_err;
}

bool wifi_captive_portal_is_running(void)
{
    captive_lock();
    const bool running = s_running;
    captive_unlock();
    return running;
}
