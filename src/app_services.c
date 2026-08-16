#include "app_services.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd_writer.h"
#include "nvs.h"
#include "system_state.h"
#include "wifi/wifi_controller.h"
#include "wifi/wifi_events.h"
#include "wifi/wifi_manager.h"
#include "wifi/wifi_monitor.h"
#include "wifi/wifi_types.h"
#include "wifi/wifi_security.h"
#include "wifi/wifi_storage.h"
#include "wifi/wifi_config.h"
#include "wifi/wifi_scan.h"
#include "network_services.h"
#include "system_error_codes.h"

#include "task_watchdog.h"
#define APP_SERVICES_TAG "APP_SERVICES"
#define APP_SERVICES_NVS_NAMESPACE NVS_NS_SYSTEM
#define APP_WIFI_ENABLED_KEY "wifi_enabled"
#define APP_OTA_MANIFEST_KEY "ota_manifest"
#define APP_OTA_AUTOCHECK_KEY "ota_auto"
#define APP_OTA_CHECK_INTERVAL_MS (6U * 60U * 60U * 1000U)
#define APP_OTA_TASK_STACK_SIZE 4096U
#define APP_OTA_TASK_PRIORITY 3U
#define APP_WIFI_OPERATION_WATCH_STACK_SIZE 3072U
#define APP_WIFI_OPERATION_WATCH_PRIORITY 5U
#define APP_WIFI_OPERATION_TIMEOUT_MS 30000U
#define APP_WIFI_SCAN_DURATION_MS 40000U
#define APP_WIFI_SCAN_POLL_MS 250U
#define APP_WIFI_SCAN_INTERVAL_MS 2500U
#define APP_WIFI_SCAN_TASK_STACK_SIZE 6144U
#define APP_WIFI_SCAN_TASK_PRIORITY 5U

extern system_state_t sys_state;
extern SemaphoreHandle_t sys_state_mutex;

static SemaphoreHandle_t s_services_mutex;
static TaskHandle_t s_ota_check_task;
static TaskHandle_t s_wifi_operation_watch_task;
static TaskHandle_t s_wifi_scan_task;
static bool s_wifi_scan_active;
static bool s_wifi_scan_cancel_requested;
static char s_manifest_url[APP_OTA_MANIFEST_URL_MAX];
static app_ota_status_t s_ota_status;

typedef enum {
    APP_WIFI_OPERATION_NONE = 0,
    APP_WIFI_OPERATION_ENABLE,
    APP_WIFI_OPERATION_CONNECT_SAVED,
    APP_WIFI_OPERATION_DISABLE,
    APP_WIFI_OPERATION_DISCONNECT,
    APP_WIFI_OPERATION_PROVISION,
} app_wifi_operation_t;

static app_wifi_operation_t s_wifi_operation;
static char s_wifi_operation_ssid[WIFI_MAX_SSID_LEN + 1U];
static uint32_t s_wifi_operation_generation;
static TickType_t s_wifi_operation_started_tick;

static bool app_wifi_scan_cancel_requested(void)
{
    bool cancel = true;
    if (s_services_mutex != NULL) {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        cancel = s_wifi_scan_cancel_requested;
        xSemaphoreGive(s_services_mutex);
    }
    return cancel;
}

static void app_wifi_scan_task(void *parameter)
{
    (void)parameter;
    const TickType_t started = xTaskGetTickCount();
    const TickType_t duration = pdMS_TO_TICKS(APP_WIFI_SCAN_DURATION_MS);
    uint8_t spinner = 0U;
    wifi_ap_record_t records[WIFI_MAX_SCAN_RESULTS] = {0};
    char ssids[LCD_WIFI_MAX_AP][LCD_WIFI_SSID_MAX_LEN + 1U] = {{0}};
    int8_t rssi[LCD_WIFI_MAX_AP] = {0};

    while (!app_wifi_scan_cancel_requested() &&
           (xTaskGetTickCount() - started) < duration) {
        uint16_t count = WIFI_MAX_SCAN_RESULTS;
        const esp_err_t err = wifi_scan_start_records(records, &count);
        if (err == ESP_OK) {
            const uint8_t visible = count > LCD_WIFI_MAX_AP ? LCD_WIFI_MAX_AP : (uint8_t)count;
            memset(ssids, 0, sizeof(ssids));
            memset(rssi, 0, sizeof(rssi));
            for (uint8_t i = 0U; i < visible; ++i) {
                strncpy(ssids[i], (const char *)records[i].ssid, LCD_WIFI_SSID_MAX_LEN);
                ssids[i][LCD_WIFI_SSID_MAX_LEN] = '\0';
                rssi[i] = records[i].rssi;
            }
            lcd_update_wifi_scan_results(visible, ssids, rssi, spinner++);
        } else {
            lcd_update_wifi_scan_spinner(spinner++);
        }

        const TickType_t wait_until = xTaskGetTickCount() +
                                      pdMS_TO_TICKS(APP_WIFI_SCAN_INTERVAL_MS);
        while (!app_wifi_scan_cancel_requested() &&
               (int32_t)(wait_until - xTaskGetTickCount()) > 0 &&
               (xTaskGetTickCount() - started) < duration) {
            vTaskDelay(pdMS_TO_TICKS(APP_WIFI_SCAN_POLL_MS));
        }
    }

    if (!app_wifi_scan_cancel_requested()) {
        uint8_t selected = 0U;
        uint8_t top = 0U;
        uint8_t count = 0U;
        LCD_LOCK();
        count = sys_lcd.wifi_scan.count;
        selected = sys_lcd.wifi_scan.selected_index;
        top = sys_lcd.wifi_scan.top_index;
        LCD_UNLOCK();
        if (count > 0U && selected >= count) {
            selected = count - 1U;
        }
        lcd_show_wifi_scan(count, ssids, rssi, selected, top);
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_scan_active = false;
    s_wifi_scan_cancel_requested = false;
    s_wifi_scan_task = NULL;
    xSemaphoreGive(s_services_mutex);
    vTaskDelete(NULL);
}

static void app_wifi_operation_watch_task(void *parameter)
{
    (void)parameter;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500U));

        bool timed_out = false;
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        if (s_wifi_operation != APP_WIFI_OPERATION_NONE &&
            (uint32_t)(xTaskGetTickCount() - s_wifi_operation_started_tick) >=
                pdMS_TO_TICKS(APP_WIFI_OPERATION_TIMEOUT_MS)) {
            s_wifi_operation = APP_WIFI_OPERATION_NONE;
            s_wifi_operation_ssid[0] = '\0';
            ++s_wifi_operation_generation;
            timed_out = true;
        }
        xSemaphoreGive(s_services_mutex);

        if (timed_out) {
            ESP_LOGW(APP_SERVICES_TAG, "Wi-Fi operation timed out after %ums",
                     (unsigned)APP_WIFI_OPERATION_TIMEOUT_MS);
            lcd_show_wifi_result(false, true, true, "Wi-Fi timeout");
        }
    }
}

static void app_wifi_begin_operation(app_wifi_operation_t operation,
                                     const char *ssid)
{
    if (s_services_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_operation = operation;
    ++s_wifi_operation_generation;
    s_wifi_operation_started_tick = xTaskGetTickCount();
    s_wifi_operation_ssid[0] = '\0';
    if (ssid != NULL) {
        strncpy(s_wifi_operation_ssid, ssid, sizeof(s_wifi_operation_ssid) - 1U);
        s_wifi_operation_ssid[sizeof(s_wifi_operation_ssid) - 1U] = '\0';
    }
    xSemaphoreGive(s_services_mutex);
}

static void app_wifi_end_operation(void)
{
    if (s_services_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_operation = APP_WIFI_OPERATION_NONE;
    s_wifi_operation_ssid[0] = '\0';
    xSemaphoreGive(s_services_mutex);
}

static bool app_wifi_operation_pending(void)
{
    if (s_services_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool pending = s_wifi_operation != APP_WIFI_OPERATION_NONE;
    xSemaphoreGive(s_services_mutex);
    return pending;
}

static void app_wifi_status_callback(const wifi_status_t *status)
{
    if (status == NULL || s_services_mutex == NULL) {
        return;
    }

    app_wifi_operation_t operation;
    char ssid[sizeof(s_wifi_operation_ssid)] = {0};
    bool terminal = false;
    bool connected = false;
    bool failed = false;
    const char *message = NULL;

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    operation = s_wifi_operation;
    strncpy(ssid, s_wifi_operation_ssid, sizeof(ssid) - 1U);
    xSemaphoreGive(s_services_mutex);

    if (operation == APP_WIFI_OPERATION_NONE) {
        return;
    }

    switch (operation) {
    case APP_WIFI_OPERATION_ENABLE:
        if (status->state == WIFI_STATE_CONNECTED && status->got_ip) {
            terminal = true;
            connected = true;
            message = ssid[0] != '\0' ? ssid : "Connected";
        } else if (status->state == WIFI_STATE_PROVISIONING) {
            terminal = true;
            message = "Setup AP Active";
        } else if (status->state == WIFI_STATE_FAILED) {
            terminal = true;
            failed = true;
            message = "Wi-Fi unavailable";
        }
        break;
    case APP_WIFI_OPERATION_CONNECT_SAVED:
        if (status->state == WIFI_STATE_CONNECTED && status->got_ip) {
            terminal = true;
            connected = true;
            message = ssid[0] != '\0' ? ssid : "Connected";
        } else if (status->state == WIFI_STATE_FAILED) {
            terminal = true;
            failed = true;
            message = "Connect failed";
        }
        break;
    case APP_WIFI_OPERATION_DISABLE:
        if (status->state == WIFI_STATE_IDLE ||
            (status->state == WIFI_STATE_FAILED && !status->connected)) {
            terminal = true;
            message = "Wi-Fi Disabled";
        }
        break;
    case APP_WIFI_OPERATION_DISCONNECT:
        if (!status->connected &&
            (status->state == WIFI_STATE_IDLE ||
             status->state == WIFI_STATE_DISCONNECTED ||
             status->state == WIFI_STATE_FAILED)) {
            terminal = true;
            message = "Disconnected";
        }
        break;
    case APP_WIFI_OPERATION_PROVISION:
        if (status->state == WIFI_STATE_PROVISIONING) {
            terminal = true;
            message = "Setup AP Active";
        } else if (status->state == WIFI_STATE_FAILED) {
            terminal = true;
            failed = true;
            message = "Setup AP failed";
        }
        break;
    case APP_WIFI_OPERATION_NONE:
    default:
        break;
    }

    if (!terminal) {
        return;
    }

    app_wifi_end_operation();
    if (operation == APP_WIFI_OPERATION_DISABLE ||
        operation == APP_WIFI_OPERATION_DISCONNECT) {
        lcd_flash_message(message, "Saved", 1200U);
    } else {
        lcd_show_wifi_result(connected, failed, false, message);
    }
}

static esp_err_t persist_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_SERVICES_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t persist_manifest_url(const char *url)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_SERVICES_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }
    if (url[0] == '\0')
    {
        err = nvs_erase_key(handle, APP_OTA_MANIFEST_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            err = ESP_OK;
        }
    }
    else
    {
        err = nvs_set_str(handle, APP_OTA_MANIFEST_KEY, url);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void load_persisted_config(void)
{
    uint8_t wifi_enabled = 0U;
    uint8_t auto_check = 1U;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_SERVICES_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK)
    {
        (void)nvs_get_u8(handle, APP_WIFI_ENABLED_KEY, &wifi_enabled);
        (void)nvs_get_u8(handle, APP_OTA_AUTOCHECK_KEY, &auto_check);
        size_t length = sizeof(s_manifest_url);
        err = nvs_get_str(handle, APP_OTA_MANIFEST_KEY, s_manifest_url, &length);
        if (err != ESP_OK)
        {
            s_manifest_url[0] = '\0';
        }
        nvs_close(handle);
    }
    else
    {
        s_manifest_url[0] = '\0';
    }

    sys_state.wifi.enabled = wifi_enabled != 0U;
    sys_state.inverter.wifi_enabled = sys_state.wifi.enabled;
    s_ota_status.auto_check_enabled = auto_check != 0U;
}

static const char *wifi_state_text(wifi_controller_state_t state)
{
    switch (state)
    {
    case WIFI_CONTROLLER_CONNECTED:
        return "Connected";
    case WIFI_CONTROLLER_CONNECTING:
    case WIFI_CONTROLLER_STARTING:
        return "Connecting";
    case WIFI_CONTROLLER_PROVISIONING:
        return "Setup AP Active";
    case WIFI_CONTROLLER_AP_ACTIVE:
        return "AP active";
    case WIFI_CONTROLLER_ERROR:
        return "Controller Error";
    case WIFI_CONTROLLER_IDLE:
    default:
        return "Not connected";
    }
}

static void ota_progress_callback(int percent)
{
    if (!s_services_mutex)
    {
        return;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_ota_status.state = APP_OTA_DOWNLOADING;
    s_ota_status.progress_percent = percent;
    xSemaphoreGive(s_services_mutex);
}

static void ota_status_callback(ota_status_t status, int percent)
{
    if (!s_services_mutex)
    {
        return;
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_ota_status.progress_percent = percent;
    switch (status)
    {
    case OTA_STATUS_STARTED:
    case OTA_STATUS_DOWNLOADING:
    case OTA_STATUS_VERIFYING:
        s_ota_status.state = APP_OTA_DOWNLOADING;
        break;
    case OTA_STATUS_SUCCESS:
        s_ota_status.state = APP_OTA_COMPLETE;
        break;
    case OTA_STATUS_CANCELLED:
        s_ota_status.state = APP_OTA_IDLE;
        s_ota_status.confirmation_pending = false;
        break;
    case OTA_STATUS_FAILED:
        s_ota_status.state = APP_OTA_ERROR;
        s_ota_status.confirmation_pending = false;
        break;
    case OTA_STATUS_IDLE:
    default:
        break;
    }
    xSemaphoreGive(s_services_mutex);

    if (status == OTA_STATUS_VERIFYING)
    {
        lcd_flash_message("Verifying Update", "Please wait", 1200U);
    }
    else if (status == OTA_STATUS_FAILED)
    {
        lcd_flash_message("Update Failed", "Current kept", 1800U);
    }
    else if (status == OTA_STATUS_CANCELLED)
    {
        lcd_flash_message("Update Cancelled", "Current kept", 1200U);
    }
    else if (status == OTA_STATUS_SUCCESS)
    {
        lcd_flash_message("Update Complete", "Restarting", 800U);
    }
}

static void ota_auto_check_task(void *parameter)
{
    task_watchdog_register("ota_auto_check_task");
    (void)parameter;
    uint32_t wait_ms = 30000U;
    while (wait_ms > 0U)
    {
        task_watchdog_feed();
        const uint32_t slice = wait_ms > 1000U ? 1000U : wait_ms;
        vTaskDelay(pdMS_TO_TICKS(slice));
        wait_ms -= slice;
        task_watchdog_feed();
    }
    while (true)
    {
        task_watchdog_feed();
        app_ota_status_t status;
        app_services_get_ota_status(&status);
        if (status.auto_check_enabled &&
            status.state != APP_OTA_DOWNLOADING &&
            status.state != APP_OTA_CHECKING &&
            wifi_controller_is_connected())
        {
            (void)app_services_check_for_update(false);
        }
        wait_ms = APP_OTA_CHECK_INTERVAL_MS;
        while (wait_ms > 0U)
        {
            const uint32_t slice = wait_ms > 1000U ? 1000U : wait_ms;
            vTaskDelay(pdMS_TO_TICKS(slice));
            wait_ms -= slice;
            task_watchdog_feed();
        }
    }
}

esp_err_t app_services_init(void)
{
    if (!s_services_mutex)
    {
        s_services_mutex = xSemaphoreCreateMutex();
        if (!s_services_mutex)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    memset(&s_ota_status, 0, sizeof(s_ota_status));
    s_ota_status.state = APP_OTA_IDLE;
    s_wifi_operation = APP_WIFI_OPERATION_NONE;
    s_wifi_operation_ssid[0] = '\0';
    s_wifi_operation_generation = 0U;
    s_wifi_operation_started_tick = 0U;
    s_wifi_scan_active = false;
    s_wifi_scan_cancel_requested = false;
    s_wifi_scan_task = NULL;
    xSemaphoreGive(s_services_mutex);

    load_persisted_config();

    if (s_manifest_url[0] == '\0')
    {
        const char *bootstrap_manifest_url =
            "https://github.com/Jezzylinks/inverter/releases/latest/download/inverter.csv";
        const esp_err_t manifest_err =
            app_services_set_ota_manifest_url(bootstrap_manifest_url);
        if (manifest_err != ESP_OK)
        {
            ESP_LOGW(APP_SERVICES_TAG,
                     "Could not provision OTA manifest URL: %s",
                     esp_err_to_name(manifest_err));
        }
        else
        {
            ESP_LOGI(APP_SERVICES_TAG, "OTA manifest URL provisioned");
        }
    }

    esp_err_t err = wifi_security_init();

    if (err != ESP_OK)
    {
        ESP_LOGW(APP_SERVICES_TAG, "Wi-Fi security storage unavailable: %s", esp_err_to_name(err));
    }
    err = wifi_controller_init();
    if (err != ESP_OK)
    {
        ESP_LOGW(APP_SERVICES_TAG, "Wi-Fi controller unavailable: %s", esp_err_to_name(err));
    }
    else
    {
        const esp_err_t callback_err =
            wifi_events_register_status_callback(app_wifi_status_callback);
        if (callback_err != ESP_OK)
        {
            ESP_LOGW(APP_SERVICES_TAG,
                     "Could not register Wi-Fi status callback: %s",
                     esp_err_to_name(callback_err));
        }
        const esp_err_t network_err = network_services_init();
        if (network_err != ESP_OK) {
            ESP_LOGW(APP_SERVICES_TAG,
                     "Network services unavailable: %s",
                     esp_err_to_name(network_err));
            if (err == ESP_OK) {
                err = network_err;
            }
        }
        if (sys_state.wifi.enabled)
        {
            err = wifi_controller_start();
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
            {
                ESP_LOGW(APP_SERVICES_TAG, "Could not restore Wi-Fi state: %s", esp_err_to_name(err));
            }
        }
    }

    const esp_err_t ota_err = ota_service_init();
    if (ota_err != ESP_OK)
    {
        ESP_LOGW(APP_SERVICES_TAG, "OTA service unavailable: %s", esp_err_to_name(ota_err));
    }
    else
    {
        (void)ota_service_register_progress_callback(ota_progress_callback);
        (void)ota_service_register_status_callback(ota_status_callback);
        (void)ota_service_get_current_version(s_ota_status.installed_version,
                                              sizeof(s_ota_status.installed_version));
    }

    if (!s_ota_check_task)
    {
        if (xTaskCreate(ota_auto_check_task, "ota_check", APP_OTA_TASK_STACK_SIZE,
                        NULL, APP_OTA_TASK_PRIORITY, &s_ota_check_task) != pdPASS)
        {
            s_ota_check_task = NULL;
            ESP_LOGW(APP_SERVICES_TAG, "Could not create OTA availability task");
        }
    }
    if (!s_wifi_operation_watch_task)
    {
        if (xTaskCreate(app_wifi_operation_watch_task,
                        "wifi_op_watch",
                        APP_WIFI_OPERATION_WATCH_STACK_SIZE,
                        NULL,
                        APP_WIFI_OPERATION_WATCH_PRIORITY,
                        &s_wifi_operation_watch_task) != pdPASS)
        {
            s_wifi_operation_watch_task = NULL;
            ESP_LOGW(APP_SERVICES_TAG, "Could not create Wi-Fi operation watcher");
        }
    }
    return err == ESP_OK ? ota_err : err;
}

esp_err_t app_services_set_wifi_enabled(bool enabled)
{
    char ssid[WIFI_MAX_SSID_LEN + 1U] = {0};
    if (enabled) {
        strncpy(ssid, WIFI_COMPILED_STA_SSID, sizeof(ssid) - 1U);
    }

    sys_state.wifi.enabled = enabled;
    sys_state.inverter.wifi_enabled = enabled;
    app_wifi_begin_operation(enabled ? APP_WIFI_OPERATION_ENABLE
                                     : APP_WIFI_OPERATION_DISABLE,
                             ssid);

    if (!enabled) {
        (void)network_services_stop();
    }
    esp_err_t controller_err = enabled ? wifi_controller_start()
                                       : wifi_controller_stop();
    if (controller_err == ESP_ERR_INVALID_STATE && enabled) {
        controller_err = wifi_controller_reconnect();
    }
    if (!enabled && (controller_err == ESP_ERR_INVALID_STATE ||
                     controller_err == ESP_ERR_WIFI_NOT_INIT ||
                     controller_err == ESP_ERR_WIFI_NOT_STARTED)) {
        controller_err = ESP_OK;
    }

    const esp_err_t nvs_err = persist_u8(APP_WIFI_ENABLED_KEY, enabled ? 1U : 0U);
    if (nvs_err != ESP_OK)
    {
        ESP_LOGE(APP_SERVICES_TAG, "Could not persist Wi-Fi intent: %s", esp_err_to_name(nvs_err));
    }

    if (controller_err == ESP_OK && app_wifi_operation_pending() &&
        (wifi_manager_get_mode() == WIFI_MODE_AP || !enabled)) {
        app_wifi_end_operation();
        lcd_flash_message(enabled ? "Wi-Fi ON" : "Wi-Fi OFF", "Ready", 900U);
    } else if (controller_err != ESP_OK && controller_err != ESP_ERR_WIFI_CONN) {
        app_wifi_end_operation();
        if (enabled && controller_err == ESP_ERR_NOT_FOUND) {
            lcd_flash_message("Wi-Fi Not Config", "Use menuconfig", 1800U);
        } else {
            lcd_flash_message(enabled ? "Wi-Fi Start Failed" : "Wi-Fi Stop Failed",
                              "Try again", 1500U);
        }
    }
    return controller_err != ESP_OK ? controller_err : nvs_err;
}

bool app_services_wifi_enabled(void)
{
    return sys_state.wifi.enabled;
}

const char *app_services_wifi_mode_name(void)
{
    switch (wifi_manager_get_mode()) {
    case WIFI_MODE_STA:
        return "STA";
    case WIFI_MODE_AP:
        return "AP";
    case WIFI_MODE_APSTA:
        return "APSTA";
    default:
        return "OFF";
    }
}

const char *app_services_wifi_connect_action_label(void)
{
    const wifi_mode_t mode = wifi_manager_get_mode();
    if (mode == WIFI_MODE_AP) {
        return "AP Clients";
    }
    return wifi_controller_is_connected() ? "Disconnect" : "Connect";
}

const char *app_services_wifi_secondary_action_label(void)
{
    return wifi_manager_get_mode() == WIFI_MODE_AP ? "AP Status" : "AP Clients";
}

bool app_services_wifi_can_manage_clients(void)
{
    const wifi_mode_t mode = wifi_manager_get_mode();
    return mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA;
}

bool app_services_wifi_is_ap_only(void)
{
    return wifi_manager_get_mode() == WIFI_MODE_AP;
}

esp_err_t app_services_wifi_scan(void)
{
    if (!sys_state.wifi.enabled) {
        lcd_flash_message("Wi-Fi Disabled", "Enable first", 1400U);
        return ESP_ERR_INVALID_STATE;
    }
    if (wifi_manager_get_mode() == WIFI_MODE_AP) {
        lcd_flash_message("Scan Unavailable", "AP mode only", 1400U);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_services_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    if (s_wifi_scan_active) {
        xSemaphoreGive(s_services_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_wifi_scan_active = true;
    s_wifi_scan_cancel_requested = false;
    xSemaphoreGive(s_services_mutex);

    lcd_show_wifi_scan_start();
    if (xTaskCreate(app_wifi_scan_task, "wifi_scan_ui",
                    APP_WIFI_SCAN_TASK_STACK_SIZE, NULL,
                    APP_WIFI_SCAN_TASK_PRIORITY, &s_wifi_scan_task) != pdPASS) {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_wifi_scan_active = false;
        s_wifi_scan_cancel_requested = false;
        s_wifi_scan_task = NULL;
        xSemaphoreGive(s_services_mutex);
        lcd_flash_message("Scan Failed", "Try again", 1400U);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t app_services_wifi_scan_cancel(void)
{
    if (s_services_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool active = s_wifi_scan_active;
    if (active) {
        s_wifi_scan_cancel_requested = true;
    }
    xSemaphoreGive(s_services_mutex);
    return active ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool app_services_wifi_scan_is_active(void)
{
    bool active = false;
    if (s_services_mutex != NULL) {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        active = s_wifi_scan_active;
        xSemaphoreGive(s_services_mutex);
    }
    return active;
}

esp_err_t app_services_wifi_connect_selected(uint8_t selected_index)
{
    char ssid[LCD_WIFI_SSID_MAX_LEN + 1U] = {0};
    LCD_LOCK();
    if (sys_lcd.screen != LCD_SCREEN_WIFI_SCAN ||
        sys_lcd.wifi_scan.stage != LCD_WIFI_SCAN_COMPLETE ||
        selected_index >= sys_lcd.wifi_scan.count) {
        LCD_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    strncpy(ssid, sys_lcd.wifi_scan.ssid[selected_index], sizeof(ssid) - 1U);
    LCD_UNLOCK();

    const char *password = "";
    if (strncmp(ssid, WIFI_COMPILED_STA_SSID, sizeof(ssid)) == 0) {
        password = WIFI_COMPILED_STA_PASSWORD;
    }
    return app_services_wifi_connect_network(ssid, password);
}

esp_err_t app_services_wifi_connect_network(const char *ssid,
                                               const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || password == NULL ||
        strlen(ssid) > WIFI_MAX_SSID_LEN || strlen(password) > 63U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sys_state.wifi.enabled) {
        lcd_flash_message("Wi-Fi Disabled", "Enable first", 1400U);
        return ESP_ERR_INVALID_STATE;
    }
    if (wifi_manager_get_mode() == WIFI_MODE_AP) {
        lcd_flash_message("AP Mode", "No STA Connect", 1400U);
        return ESP_ERR_NOT_SUPPORTED;
    }

    wifi_manager_config_t config = {0};
    esp_err_t err = wifi_manager_get_config(&config);
    if (err != ESP_OK) {
        return err;
    }
    strncpy(config.ssid, ssid, sizeof(config.ssid) - 1U);
    strncpy(config.password, password, sizeof(config.password) - 1U);
    err = wifi_manager_set_config(&config);
    if (err != ESP_OK) {
        lcd_flash_message("Network Invalid", "Try again", 1400U);
        return err;
    }

    wifi_credentials_t credentials = {0};
    strncpy(credentials.ssid, ssid, sizeof(credentials.ssid) - 1U);
    strncpy(credentials.password, password, sizeof(credentials.password) - 1U);
    (void)wifi_storage_save_credentials(&credentials);

    app_wifi_begin_operation(APP_WIFI_OPERATION_CONNECT_SAVED, ssid);
    lcd_show_wifi_connecting(ssid);
    err = wifi_controller_reconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        app_wifi_end_operation();
        lcd_show_wifi_result(false, true, false, "Connect failed");
    }
    return err;
}

esp_err_t app_services_wifi_connect_saved(void)
{
    return app_services_wifi_reconnect();
}

esp_err_t app_services_wifi_reconnect(void)
{
    if (wifi_manager_get_mode() == WIFI_MODE_AP) {
        lcd_flash_message("AP Mode", "No STA Connect", 1400U);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!sys_state.wifi.enabled) {
        lcd_flash_message("Wi-Fi Disabled", "Enable first", 1400U);
        return ESP_ERR_INVALID_STATE;
    }
    wifi_manager_config_t config = {0};
    if (wifi_manager_get_config(&config) != ESP_OK || config.ssid[0] == '\0') {
        lcd_flash_message("Wi-Fi Not Config", "Scan a network", 1800U);
        return ESP_ERR_NOT_FOUND;
    }
    app_wifi_begin_operation(APP_WIFI_OPERATION_CONNECT_SAVED, config.ssid);
    lcd_show_wifi_connecting(config.ssid);
    esp_err_t err = wifi_controller_reconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        app_wifi_end_operation();
        lcd_show_wifi_result(false, true, false, "Connect failed");
    }
    return err;
}

esp_err_t app_services_wifi_disconnect(void)
{
    app_wifi_begin_operation(APP_WIFI_OPERATION_DISCONNECT, NULL);
    const esp_err_t err = wifi_controller_disconnect();
    if (err != ESP_OK) {
        app_wifi_end_operation();
        lcd_show_wifi_result(false, true, false, "Disconnect failed");
    } else if (!wifi_controller_is_connected() && app_wifi_operation_pending()) {
        app_wifi_end_operation();
        lcd_show_wifi_result(false, false, false, "Disconnected");
    }
    return err;
}

esp_err_t app_services_wifi_start_provisioning(void)
{
    lcd_flash_message("Setup AP Disabled", "Use menuconfig", 1800U);
    return ESP_ERR_NOT_SUPPORTED;
}

void app_services_show_wifi_status(void)
{
    wifi_status_t status = {0};
    const wifi_status_t *status_ptr = wifi_controller_get_status();
    if (status_ptr) {
        status = *status_ptr;
    }

    const char *ssid = wifi_manager_get_mode() == WIFI_MODE_AP
                           ? WIFI_COMPILED_AP_SSID
                           : (WIFI_COMPILED_STA_SSID[0] != '\0'
                                  ? WIFI_COMPILED_STA_SSID
                                  : "Not configured");

    char ip[16] = "0.0.0.0";
    char gateway[16] = "0.0.0.0";
    if (status.got_ip) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&status.ip));
        snprintf(gateway, sizeof(gateway), IPSTR, IP2STR(&status.gateway));
    }

    int8_t rssi = status.connected ? wifi_manager_get_rssi() : status.rssi;
    char state_text[sizeof(status.ip) + 24U];
    snprintf(state_text, sizeof(state_text), "%s %s",
             app_services_wifi_mode_name(),
             wifi_manager_get_mode() == WIFI_MODE_AP
                 ? "AP active"
                 : wifi_state_text(wifi_controller_get_state()));
    lcd_show_wifi_status(state_text, ssid,
                         ip, gateway, rssi, status.connected, status.got_ip,
                         status.internet_available);
}

esp_err_t app_services_get_ap_clients(wifi_ap_client_info_t clients[],
                                      size_t capacity,
                                      size_t *count)
{
    return wifi_manager_get_ap_clients(clients, capacity, count);
}

esp_err_t app_services_disconnect_ap_client(const uint8_t mac[6])
{
    return wifi_manager_disconnect_ap_client(mac);
}

esp_err_t app_services_disconnect_ap_client_at(uint8_t index)
{
    wifi_ap_client_info_t clients[WIFI_AP_MAX_CLIENTS] = {0};
    size_t count = 0U;
    esp_err_t err = app_services_get_ap_clients(clients, WIFI_AP_MAX_CLIENTS, &count);
    if (err != ESP_OK && err != ESP_ERR_INVALID_SIZE) {
        return err;
    }
    if (index >= count) {
        return ESP_ERR_NOT_FOUND;
    }
    return app_services_disconnect_ap_client(clients[index].mac);
}

void app_services_show_ap_clients(void)
{
    if (!app_services_wifi_can_manage_clients()) {
        lcd_flash_message("AP Clients", "STA mode only", 1400U);
        return;
    }
    wifi_ap_client_info_t clients[WIFI_AP_MAX_CLIENTS] = {0};
    char macs[WIFI_AP_MAX_CLIENTS][18] = {{0}};
    size_t count = 0U;
    const esp_err_t err = app_services_get_ap_clients(clients, WIFI_AP_MAX_CLIENTS, &count);
    if (err != ESP_OK && err != ESP_ERR_INVALID_SIZE) {
        lcd_show_system_error(SYSTEM_ERROR_WIFI_AP_CLIENT_LIMIT);
        return;
    }
    for (size_t i = 0U; i < count; ++i) {
        snprintf(macs[i], sizeof(macs[i]), "%02X:%02X:%02X:%02X:%02X:%02X",
                 clients[i].mac[0], clients[i].mac[1], clients[i].mac[2],
                 clients[i].mac[3], clients[i].mac[4], clients[i].mac[5]);
    }
    lcd_show_wifi_clients((uint8_t)count, (const char (*)[18])macs, 0U);
}

esp_err_t app_services_set_ota_manifest_url(const char *url)
{
    if (!url || strlen(url) >= sizeof(s_manifest_url) ||
        (url[0] != '\0' && strncmp(url, "https://", 8U) != 0))
    {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = persist_manifest_url(url);
    if (err == ESP_OK)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        strncpy(s_manifest_url, url, sizeof(s_manifest_url) - 1U);
        s_manifest_url[sizeof(s_manifest_url) - 1U] = '\0';
        xSemaphoreGive(s_services_mutex);
    }
    return err;
}

esp_err_t app_services_get_ota_manifest_url(char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    if (strlen(s_manifest_url) >= buffer_len)
    {
        xSemaphoreGive(s_services_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(buffer, s_manifest_url);
    xSemaphoreGive(s_services_mutex);
    return ESP_OK;
}

esp_err_t app_services_check_for_update(bool user_initiated)
{
    char manifest_url[APP_OTA_MANIFEST_URL_MAX] = {0};
    if (!wifi_controller_is_connected())
    {
        if (user_initiated)
        {
            lcd_show_system_error(SYSTEM_ERROR_WIFI_NOT_CONNECTED);
        }
        ESP_LOGW(APP_SERVICES_TAG, "Update check skipped: station is not connected");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_monitor_is_online())
    {
        if (user_initiated)
        {
            lcd_show_system_error(SYSTEM_ERROR_WIFI_INTERNET_UNAVAILABLE);
        }
        ESP_LOGW(APP_SERVICES_TAG, "Internet Not Available; OTA check skipped");
        return ESP_ERR_INVALID_STATE;
    }

    (void)app_services_get_ota_manifest_url(manifest_url, sizeof(manifest_url));
    if (manifest_url[0] == '\0')
    {
        if (user_initiated)
        {
            lcd_flash_message("OTA Not Set", "Configure URL", 1500U);
        }
        return ESP_ERR_NOT_FOUND;
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    if (s_ota_status.state == APP_OTA_DOWNLOADING || s_ota_status.state == APP_OTA_CHECKING)
    {
        xSemaphoreGive(s_services_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_ota_status.state = APP_OTA_CHECKING;
    s_ota_status.update_available = false;
    xSemaphoreGive(s_services_mutex);

    ota_manifest_entry_t entry = {0};
    bool update_available = false;
    const esp_err_t err = ota_service_check_csv_manifest(manifest_url, &entry, &update_available);

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    if (err == ESP_OK && update_available)
    {
        s_ota_status.state = APP_OTA_AVAILABLE;
        s_ota_status.update_available = true;
        strncpy(s_ota_status.available_version, entry.version,
                sizeof(s_ota_status.available_version) - 1U);
    }
    else
    {
        s_ota_status.state = err == ESP_OK ? APP_OTA_IDLE : APP_OTA_ERROR;
        s_ota_status.update_available = false;
        s_ota_status.available_version[0] = '\0';
    }
    xSemaphoreGive(s_services_mutex);

    if (err != ESP_OK)
    {
        ESP_LOGW(APP_SERVICES_TAG, "OTA check failed: %s", esp_err_to_name(err));
        if (user_initiated)
        {
            lcd_show_system_error(SYSTEM_ERROR_OTA_MANIFEST);
        }
    }
    else if (update_available)
    {
        char line[LCD_LINE_SIZE];
        snprintf(line, sizeof(line), "Version %.8s", entry.version);
        lcd_flash_message("Update Available", line, 1800U);
    }
    else if (user_initiated)
    {
        lcd_flash_message("Firmware Current", "No update", 1400U);
    }
    return err;
}

esp_err_t app_services_request_update_confirmation(void)
{
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool available = s_ota_status.update_available;
    const bool busy = s_ota_status.state == APP_OTA_DOWNLOADING;
    if (available && !busy)
    {
        s_ota_status.state = APP_OTA_CONFIRMING;
        s_ota_status.confirmation_pending = true;
    }
    xSemaphoreGive(s_services_mutex);

    if (!available || busy)
    {
        lcd_flash_message("No Update Ready", "Check first", 1400U);
        return ESP_ERR_INVALID_STATE;
    }
    lcd_show_confirm("Install Update?", "Enter=Yes Back=No");
    return ESP_OK;
}

esp_err_t app_services_confirm_update(void)
{
    char manifest_url[APP_OTA_MANIFEST_URL_MAX] = {0};
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool confirmed = s_ota_status.confirmation_pending;
    if (confirmed)
    {
        s_ota_status.confirmation_pending = false;
        s_ota_status.state = APP_OTA_DOWNLOADING;
        s_ota_status.progress_percent = 0;
    }
    strncpy(manifest_url, s_manifest_url, sizeof(manifest_url) - 1U);
    xSemaphoreGive(s_services_mutex);

    if (!confirmed || manifest_url[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_monitor_is_online())
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_ota_status.confirmation_pending = false;
        s_ota_status.state = APP_OTA_AVAILABLE;
        xSemaphoreGive(s_services_mutex);
        lcd_show_system_error(SYSTEM_ERROR_WIFI_INTERNET_UNAVAILABLE);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = ota_service_start_from_csv(manifest_url);
    if (err != ESP_OK)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_ota_status.state = APP_OTA_ERROR;
        xSemaphoreGive(s_services_mutex);
        lcd_flash_message("Update Start Fail", "Current kept", 1500U);
        return err;
    }
    lcd_flash_message("Updating", "Do not power off", 1500U);
    return ESP_OK;
}

esp_err_t app_services_cancel_update(void)
{
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool pending = s_ota_status.confirmation_pending;
    s_ota_status.confirmation_pending = false;
    if (pending)
    {
        s_ota_status.state = APP_OTA_AVAILABLE;
    }
    xSemaphoreGive(s_services_mutex);

    if (ota_service_in_progress())
    {
        return ota_service_cancel();
    }
    if (pending)
    {
        lcd_flash_message("Update Deferred", "Current kept", 1200U);
    }
    return ESP_OK;
}

void app_services_get_ota_status(app_ota_status_t *status)
{
    if (!status)
    {
        return;
    }
    if (!s_services_mutex)
    {
        memset(status, 0, sizeof(*status));
        return;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    *status = s_ota_status;
    xSemaphoreGive(s_services_mutex);
}
