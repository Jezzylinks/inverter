/**
 * @file wifi_provision.c
 * @brief SoftAP provisioning lifecycle built on manager-owned ESP-IDF objects.
 */
#include "wifi_provision.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "wifi_captive_portal.h"
#include "wifi_http_server.h"
#include "wifi_storage.h"

#define WIFI_PROVISION_TAG "WIFI_PROVISION"

static wifi_provision_complete_callback_t s_complete_callback;
static wifi_provision_state_t s_state = WIFI_PROVISION_IDLE;
static SemaphoreHandle_t s_mutex;

static void provision_lock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void provision_unlock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

static esp_err_t wifi_provision_configure_ap(void)
{
    wifi_network_config_t config;
    esp_err_t err = wifi_storage_load_network_config(&config);
    if (err != ESP_OK) {
        ESP_LOGW(WIFI_PROVISION_TAG, "Network config unavailable: %s; using defaults",
                 esp_err_to_name(err));
        wifi_storage_set_default_network_config(&config);
    }

    const size_t password_len = strnlen(config.ap_password, sizeof(config.ap_password));
    if (config.ap_ssid[0] == '\0' || password_len < 8U ||
        config.ap_channel < 1U || config.ap_channel > 13U ||
        config.ap_max_connection < 1U || config.ap_max_connection > 10U) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, config.ap_ssid, sizeof(ap_config.ap.ssid) - 1U);
    strncpy((char *)ap_config.ap.password, config.ap_password, sizeof(ap_config.ap.password) - 1U);
    ap_config.ap.channel = config.ap_channel;
    ap_config.ap.max_connection = config.ap_max_connection;
    ap_config.ap.authmode = config.ap_authmode;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

static void wifi_provision_credentials_saved(void)
{
    wifi_provision_complete_callback_t callback = NULL;
    provision_lock();
    callback = s_complete_callback;
    provision_unlock();

    ESP_LOGI(WIFI_PROVISION_TAG, "Provisioning credentials committed");
    (void)wifi_provision_stop();
    if (callback != NULL) {
        callback();
    }
}

esp_err_t wifi_provision_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = wifi_captive_portal_init();
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }
    s_state = WIFI_PROVISION_IDLE;
    return ESP_OK;
}

esp_err_t wifi_provision_deinit(void)
{
    if (s_mutex == NULL) {
        return ESP_OK;
    }
    const esp_err_t stop_err = wifi_provision_stop();
    (void)wifi_captive_portal_deinit();
    provision_lock();
    s_complete_callback = NULL;
    s_state = WIFI_PROVISION_IDLE;
    provision_unlock();
    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;
    return stop_err;
}

esp_err_t wifi_provision_start(void)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    provision_lock();
    if (s_state == WIFI_PROVISION_RUNNING) {
        provision_unlock();
        return ESP_OK;
    }
    provision_unlock();

    esp_err_t err = wifi_provision_configure_ap();
    if (err != ESP_OK) {
        provision_lock();
        s_state = WIFI_PROVISION_FAILED;
        provision_unlock();
        return err;
    }

    err = esp_wifi_start();
    if (err == ESP_ERR_WIFI_CONN) {
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        provision_lock();
        s_state = WIFI_PROVISION_FAILED;
        provision_unlock();
        return err;
    }

    err = wifi_http_server_register_save_callback(wifi_provision_credentials_saved);
    if (err == ESP_OK) {
        err = wifi_captive_portal_start();
    }
    if (err != ESP_OK) {
        (void)wifi_http_server_register_save_callback(NULL);
        (void)esp_wifi_stop();
        provision_lock();
        s_state = WIFI_PROVISION_FAILED;
        provision_unlock();
        return err;
    }

    provision_lock();
    s_state = WIFI_PROVISION_RUNNING;
    provision_unlock();
    ESP_LOGI(WIFI_PROVISION_TAG, "Provisioning AP and portal started");
    return ESP_OK;
}

esp_err_t wifi_provision_stop(void)
{
    if (s_mutex == NULL) {
        return ESP_OK;
    }

    provision_lock();
    const bool running = s_state == WIFI_PROVISION_RUNNING;
    s_state = WIFI_PROVISION_IDLE;
    provision_unlock();
    if (!running) {
        return ESP_OK;
    }

    esp_err_t first_err = wifi_captive_portal_stop();
    const esp_err_t callback_err = wifi_http_server_register_save_callback(NULL);
    if (first_err == ESP_OK) {
        first_err = callback_err;
    }
    const esp_err_t stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_INIT && first_err == ESP_OK) {
        first_err = stop_err;
    }
    ESP_LOGI(WIFI_PROVISION_TAG, "Provisioning stopped");
    return first_err;
}

wifi_provision_state_t wifi_provision_get_state(void)
{
    provision_lock();
    const wifi_provision_state_t state = s_state;
    provision_unlock();
    return state;
}

bool wifi_provision_is_running(void)
{
    return wifi_provision_get_state() == WIFI_PROVISION_RUNNING;
}

esp_err_t wifi_provision_register_complete_callback(wifi_provision_complete_callback_t callback)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    provision_lock();
    s_complete_callback = callback;
    provision_unlock();
    return ESP_OK;
}
