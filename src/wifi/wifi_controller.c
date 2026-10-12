/**
 * @file wifi_controller.c
 * @brief High Level Wi-Fi Controller
 */

#include "wifi/wifi_controller.h"
#include <string.h>

#include "esp_log.h"
#include "freertos/semphr.h"

#include "wifi/wifi_storage.h"
#include "wifi/wifi_manager.h"
#include "wifi/wifi_config.h"
#if WIFI_RUNTIME_PROVISIONING_ENABLED
#include "wifi/wifi_provision.h"
#endif
#include "wifi/wifi_scan.h"
#include "wifi/wifi_monitor.h"

static const char *TAG = "WIFI_CONTROLLER";

static wifi_controller_state_t s_state = WIFI_CONTROLLER_IDLE;
static bool s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;
/* A user-requested station disconnect suppresses automatic reconnect until
 * the user explicitly connects again. Radio OFF/ON must not override it. */
static bool s_manual_disconnect = false;
static bool s_auto_connect_enabled = true;

static void wifi_controller_lock(void)
{
    if (s_mutex != NULL)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void wifi_controller_unlock(void)
{
    if (s_mutex != NULL)
    {
        xSemaphoreGive(s_mutex);
    }
}

#if WIFI_RUNTIME_PROVISIONING_ENABLED
static void wifi_controller_provision_complete(void)
{
    ESP_LOGI(TAG, "Provisioning complete");
    wifi_manager_enable_auto_reconnect(true);
    wifi_controller_lock();
    s_state = WIFI_CONTROLLER_CONNECTING;
    wifi_controller_unlock();

    esp_err_t err = wifi_manager_start();
    if (err == ESP_OK || err == ESP_ERR_WIFI_CONN) {
        err = wifi_manager_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not connect after provisioning: %s", esp_err_to_name(err));
    }
}
#endif

esp_err_t wifi_controller_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    esp_err_t err;

    /* Create mutex for thread-safe state access */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize storage */
    err = wifi_storage_init();
    if (err != ESP_OK)
    {
        goto rollback;
    }

    /* Initialize WiFi manager */
    err = wifi_manager_init();
    if (err != ESP_OK)
    {
        goto rollback_storage;
    }

#if WIFI_RUNTIME_PROVISIONING_ENABLED
    /* Provisioning is an opt-in compatibility feature. */
    err = wifi_provision_init();
    if (err != ESP_OK)
    {
        goto rollback_manager;
    }
#endif

    /* Scanning is a normal panel operation in STA and APSTA modes; it is
     * no longer coupled to the optional captive-portal provisioning feature. */
    err = wifi_scan_init();
    if (err != ESP_OK)
    {
        goto rollback_manager;
    }

    /* Initialize WiFi monitor */
    err = wifi_monitor_init();
    if (err != ESP_OK)
    {
        goto rollback_scan;
    }

#if WIFI_RUNTIME_PROVISIONING_ENABLED
    err = wifi_provision_register_complete_callback(wifi_controller_provision_complete);
    if (err != ESP_OK)
    {
        goto rollback_monitor;
    }
#endif

    wifi_controller_lock();
    s_state = WIFI_CONTROLLER_IDLE;
    s_initialized = true;
    wifi_controller_unlock();

    ESP_LOGI(TAG, "WiFi controller initialized");

    return ESP_OK;

#if WIFI_RUNTIME_PROVISIONING_ENABLED
rollback_monitor:
    wifi_monitor_deinit();
#endif
rollback_scan:
    (void)wifi_scan_deinit();
rollback_manager:
#if WIFI_RUNTIME_PROVISIONING_ENABLED
    (void)wifi_provision_deinit();
#endif
    wifi_manager_deinit();
rollback_storage:
    /* No wifi_storage_deinit available */
rollback:
    /* Clean up partial initialization */
    if (s_mutex != NULL)
    {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    return err;
}

esp_err_t wifi_controller_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;

    err = wifi_monitor_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Monitor deinit failed: %s", esp_err_to_name(err));
        return err;
    }

#if WIFI_RUNTIME_PROVISIONING_ENABLED
    err = wifi_provision_stop();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Provision stop failed: %s", esp_err_to_name(err));
    }
#endif

    err = wifi_manager_stop();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Manager stop failed: %s", esp_err_to_name(err));
    }

    (void)wifi_scan_deinit();
#if WIFI_RUNTIME_PROVISIONING_ENABLED
    (void)wifi_provision_deinit();
#endif

    err = wifi_manager_deinit();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Manager deinit failed: %s", esp_err_to_name(err));
    }

    wifi_controller_lock();
    s_state = WIFI_CONTROLLER_IDLE;
    s_initialized = false;
    wifi_controller_unlock();

    if (s_mutex != NULL)
    {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    ESP_LOGI(TAG, "WiFi controller deinitialized");

    return ESP_OK;
}

esp_err_t wifi_controller_start(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_controller_lock();
    s_state = WIFI_CONTROLLER_STARTING;
    wifi_manager_enable_auto_reconnect(s_auto_connect_enabled && !s_manual_disconnect);

    const esp_err_t err = wifi_manager_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
    {
        ESP_LOGE(TAG, "Failed to start WiFi manager: %s", esp_err_to_name(err));
        s_state = WIFI_CONTROLLER_IDLE;
        wifi_controller_unlock();
        return err;
    }

    const wifi_mode_t mode = wifi_manager_get_mode();
    wifi_manager_config_t config = {0};
    const bool have_saved_station =
        mode != WIFI_MODE_AP &&
        wifi_manager_get_config(&config) == ESP_OK &&
        config.ssid[0] != '\0';
    const wifi_connection_state_t manager_state = wifi_manager_get_state();
    const bool connected = wifi_manager_is_connected();
    const bool should_auto_connect = have_saved_station &&
                                     s_auto_connect_enabled &&
                                     !s_manual_disconnect &&
                                     !connected &&
                                     manager_state != WIFI_STATE_CONNECTING &&
                                     manager_state != WIFI_STATE_RECONNECTING;

    if (connected) {
        s_state = WIFI_CONTROLLER_CONNECTED;
    } else if (manager_state == WIFI_STATE_CONNECTING ||
               manager_state == WIFI_STATE_RECONNECTING) {
        s_state = WIFI_CONTROLLER_CONNECTING;
    } else {
        s_state = WIFI_CONTROLLER_IDLE;
    }
    wifi_controller_unlock();

    if (mode != WIFI_MODE_AP) {
        (void)wifi_monitor_start();
    }

    /* Make at most one station-connect request. A disabled auto-connect
     * preference still permits a later explicit menu/API connect request. */
    if (should_auto_connect) {
        wifi_controller_lock();
        esp_err_t connect_err = ESP_ERR_INVALID_STATE;
        if (s_initialized && s_auto_connect_enabled && !s_manual_disconnect &&
            !wifi_manager_is_connected() &&
            wifi_manager_get_state() != WIFI_STATE_CONNECTING &&
            wifi_manager_get_state() != WIFI_STATE_RECONNECTING) {
            s_state = WIFI_CONTROLLER_CONNECTING;
            connect_err = wifi_manager_connect();
            if (connect_err != ESP_OK && connect_err != ESP_ERR_WIFI_CONN) {
                s_state = WIFI_CONTROLLER_IDLE;
            }
        }
        wifi_controller_unlock();

        if (connect_err == ESP_OK || connect_err == ESP_ERR_WIFI_CONN) {
            ESP_LOGI(TAG, "Saved station auto-connect initiated");
        } else if (connect_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Saved-station auto-connect could not start: %s",
                     esp_err_to_name(connect_err));
        } else {
            ESP_LOGI(TAG, "Saved-station auto-connect skipped; station state changed");
        }
    } else if (mode != WIFI_MODE_AP && !have_saved_station) {
        ESP_LOGI(TAG, "No saved station SSID; automatic connection skipped");
    }

    ESP_LOGI(TAG, "WiFi architecture started in %s mode (auto-connect=%s)",
             mode == WIFI_MODE_STA ? "STA" :
             mode == WIFI_MODE_AP ? "AP" :
             mode == WIFI_MODE_APSTA ? "APSTA" : "UNKNOWN",
             s_auto_connect_enabled ? "enabled" : "disabled");
    return ESP_OK;
}

void wifi_controller_set_auto_connect(bool enabled)
{
    if (!s_initialized) {
        s_auto_connect_enabled = enabled;
        return;
    }

    wifi_controller_lock();
    s_auto_connect_enabled = enabled;
    wifi_manager_enable_auto_reconnect(enabled && !s_manual_disconnect);
    wifi_controller_unlock();
}

esp_err_t wifi_controller_stop(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* See wifi_controller_start(): held for the whole operation, not just
     * the state assignment, so this can never overlap a concurrent
     * wifi_manager_start()/stop() from another task. */
    wifi_controller_lock();

    /* User-requested stop must win over any pending disconnect retry.
     * Incrementing s_reconnect_generation (inside wifi_manager_enable_auto_reconnect)
     * cancels any in-flight reconnect task so it exits without calling esp_wifi_connect. */
    ESP_LOGI(TAG, "Wi-Fi OFF requested: suppressing auto-reconnect and stopping radio");
    wifi_manager_enable_auto_reconnect(false);
    esp_err_t first_err = ESP_OK;

    esp_err_t err = ESP_OK;
    if (wifi_manager_get_mode() != WIFI_MODE_AP) {
        err = wifi_monitor_stop();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            first_err = err;
            ESP_LOGW(TAG, "Failed to stop monitor: %s", esp_err_to_name(err));
        }
    }

#if WIFI_RUNTIME_PROVISIONING_ENABLED
    err = wifi_provision_stop();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        first_err = err;
        ESP_LOGW(TAG, "Failed to stop provisioning: %s", esp_err_to_name(err));
    }
#endif

    err = wifi_manager_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) {
        if (first_err == ESP_OK) {
            first_err = err;
        }
        ESP_LOGW(TAG, "Wi-Fi OFF: radio stop failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi OFF: radio stopped (auto-reconnect suppressed)");
    }

    s_state = WIFI_CONTROLLER_IDLE;
    wifi_controller_unlock();

    return first_err;
}

esp_err_t wifi_controller_reconnect(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (wifi_manager_get_mode() == WIFI_MODE_AP) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* See wifi_controller_start(). */
    wifi_controller_lock();
    s_manual_disconnect = false;
    wifi_manager_enable_auto_reconnect(s_auto_connect_enabled);
    esp_err_t err = wifi_manager_reconnect();
    if (err == ESP_OK)
    {
        s_state = WIFI_CONTROLLER_CONNECTING;
    }
    wifi_controller_unlock();
    return err;
}

esp_err_t wifi_controller_disconnect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (wifi_manager_get_mode() == WIFI_MODE_AP) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* See wifi_controller_start(). This is the path reachable from
     * POST /api/v1/wifi/disconnect, which is NOT gated behind the runtime
     * provisioning flag -- it is the most likely source of an overlapping
     * start/stop from an HTTP worker task racing the menu-driven toggle. */
    wifi_controller_lock();
    s_manual_disconnect = true;
    wifi_manager_enable_auto_reconnect(false);
    const esp_err_t err = wifi_manager_disconnect();
    if (err == ESP_OK || err == ESP_ERR_WIFI_NOT_CONNECT) {
        s_state = WIFI_CONTROLLER_IDLE;
        wifi_controller_unlock();
        return ESP_OK;
    }
    wifi_controller_unlock();
    return err;
}

esp_err_t wifi_controller_start_provisioning(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* See wifi_controller_start(). */
    wifi_controller_lock();
    wifi_manager_enable_auto_reconnect(false);
    (void)wifi_manager_stop();
#if WIFI_RUNTIME_PROVISIONING_ENABLED
    esp_err_t err = wifi_provision_start();
    if (err == ESP_OK)
    {
        s_state = WIFI_CONTROLLER_PROVISIONING;
    }
    wifi_controller_unlock();
    return err;
#else
    wifi_controller_unlock();
    ESP_LOGW(TAG, "Runtime Wi-Fi provisioning is disabled; use menuconfig");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t wifi_controller_stop_provisioning(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

#if WIFI_RUNTIME_PROVISIONING_ENABLED
    esp_err_t err = wifi_provision_stop();
    if (err == ESP_OK)
    {
        return wifi_controller_start();
    }
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

wifi_controller_state_t wifi_controller_get_state(void)
{
    wifi_controller_state_t state;

    wifi_controller_lock();
    state = s_state;
    wifi_controller_unlock();

    /* Update state based on actual WiFi status */
    if (s_initialized)
    {
        wifi_connection_state_t wifi_state = wifi_manager_get_state();
        switch (wifi_state)
        {
        case WIFI_STATE_CONNECTED:
            state = WIFI_CONTROLLER_CONNECTED;
            break;
        case WIFI_STATE_CONNECTING:
        case WIFI_STATE_RECONNECTING:
            state = WIFI_CONTROLLER_CONNECTING;
            break;
        case WIFI_STATE_FAILED:
            state = WIFI_CONTROLLER_ERROR;
            break;
        case WIFI_STATE_PROVISIONING:
            state = WIFI_CONTROLLER_PROVISIONING;
            break;
        case WIFI_STATE_AP_ACTIVE:
            state = WIFI_CONTROLLER_AP_ACTIVE;
            break;
        default:
            break;
        }
    }

    return state;
}

bool wifi_controller_is_connected(void)
{
    if (!s_initialized)
    {
        return false;
    }

    return wifi_manager_is_connected();
}

const wifi_status_t *wifi_controller_get_status(void)
{
    if (!s_initialized)
    {
        return NULL;
    }

    return wifi_manager_get_status();
}

esp_err_t wifi_controller_set_config(const wifi_manager_config_t *config)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return wifi_manager_set_config(config);
}

esp_err_t wifi_controller_get_config(wifi_manager_config_t *config)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return wifi_manager_get_config(config);
}