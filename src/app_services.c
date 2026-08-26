#include "app/app_services.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd/lcd_writer.h"
#include "lcd/lcd_flash_queue.h"
#include "nvs.h"
#include "system/system_state.h"
#include "wifi/wifi_controller.h"
#include "wifi/wifi_events.h"
#include "wifi/wifi_manager.h"
#include "wifi/wifi_monitor.h"
#include "wifi/wifi_types.h"
#include "wifi/wifi_security.h"
#include "wifi/wifi_storage.h"
#include "wifi/wifi_config.h"
#include "wifi/wifi_scan.h"
#include "server/network_services.h"
#include "server/websocket/websocket_server.h"
#include "system/system_error_codes.h"

#include "system/task_watchdog.h"
#define APP_SERVICES_TAG "APP_SERVICES"
#ifdef CONFIG_INVERTER_OTA_MANIFEST_URL
#define APP_OTA_DEFAULT_MANIFEST_URL CONFIG_INVERTER_OTA_MANIFEST_URL
#else
#define APP_OTA_DEFAULT_MANIFEST_URL \
    "https://github.com/Jezzylinks/inverter/releases/latest/download/inverter.csv"
#endif
#define APP_SERVICES_NVS_NAMESPACE NVS_NS_SYSTEM
#define APP_WIFI_ENABLED_KEY "wifi_enabled"
#define APP_OTA_MANIFEST_KEY "ota_manifest"
#define APP_OTA_AUTOCHECK_KEY "ota_auto"
#define APP_OTA_CHECK_INTERVAL_MS (6U * 60U * 60U * 1000U)
#define APP_OTA_TASK_STACK_SIZE 4096U
#define APP_OTA_CHECK_TASK_STACK_SIZE 6144U
#define APP_OTA_CHECK_TASK_PRIORITY 5U
#define APP_WIFI_OPERATION_WATCH_STACK_SIZE 3072U
#define APP_WIFI_OPERATION_WATCH_PRIORITY 5U
#define APP_WIFI_TOGGLE_TASK_STACK_SIZE 4096U
#define APP_WIFI_TOGGLE_TASK_PRIORITY 5U
#define APP_WIFI_TOGGLE_QUEUE_LENGTH 1U
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
static TaskHandle_t s_ota_manifest_check_task;
static bool s_ota_manifest_check_active;
static TaskHandle_t s_wifi_operation_watch_task;
static TaskHandle_t s_wifi_toggle_task;
static QueueHandle_t s_wifi_toggle_queue;
static TaskHandle_t s_wifi_scan_task;
static bool s_wifi_forget_pending;
static bool s_wifi_disconnect_pending;
static bool s_ota_check_cancel_requested;
static bool s_ota_cancel_confirmation_pending;
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
static int8_t s_wifi_operation_rssi = -127;

typedef struct {
    bool enabled;
    bool previous_enabled;
    uint64_t requested_ms;
} wifi_toggle_request_t;

static esp_err_t persist_u8(const char *key, uint8_t value);

static bool app_manifest_url_is_valid(const char *url)
{
    return url != NULL && url[0] != '\0' &&
           strncmp(url, "https://", 8U) == 0 &&
           strlen(url) < APP_OTA_MANIFEST_URL_MAX;
}

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
    uint8_t channel[LCD_WIFI_MAX_AP] = {0};
    uint8_t authmode[LCD_WIFI_MAX_AP] = {0};
    uint8_t last_count = 0U;
    bool had_successful_scan = false;
    esp_err_t last_scan_error = ESP_OK;

    while (!app_wifi_scan_cancel_requested() &&
           (xTaskGetTickCount() - started) < duration) {
        uint16_t count = WIFI_MAX_SCAN_RESULTS;
        const esp_err_t err = wifi_scan_start_records(records, &count);
        last_scan_error = err;
        if (err == ESP_OK) {
            const uint8_t visible = count > LCD_WIFI_MAX_AP ? LCD_WIFI_MAX_AP : (uint8_t)count;
            last_count = visible;
            had_successful_scan = true;
            memset(ssids, 0, sizeof(ssids));
            memset(rssi, 0, sizeof(rssi));
            for (uint8_t i = 0U; i < visible; ++i) {
                strncpy(ssids[i], (const char *)records[i].ssid, LCD_WIFI_SSID_MAX_LEN);
                ssids[i][LCD_WIFI_SSID_MAX_LEN] = '\0';
                rssi[i] = records[i].rssi;
                channel[i] = records[i].primary;
                authmode[i] = (uint8_t)records[i].authmode;
            }
            lcd_update_wifi_scan_results(visible, ssids, rssi, channel, authmode, spinner++);
        } else if (!app_wifi_scan_cancel_requested()) {
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

    bool keep_scan_screen = false;
    LCD_LOCK();
    keep_scan_screen = sys_lcd.screen == LCD_SCREEN_WIFI_SCAN;
    LCD_UNLOCK();
    if (keep_scan_screen) {
        if (!had_successful_scan && !app_wifi_scan_cancel_requested() &&
            last_scan_error != ESP_OK) {
            lcd_show_wifi_scan_failed();
        } else {
            /* Natural completion and ENTER cancellation both expose the latest
             * valid result set and reset selection to the first network. */
            lcd_show_wifi_scan(last_count, ssids, rssi, channel, authmode, 0U, 0U);
        }
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
        char timed_out_ssid[sizeof(s_wifi_operation_ssid)] = {0};
        int8_t timed_out_rssi = -127;
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        if (s_wifi_operation != APP_WIFI_OPERATION_NONE &&
            (uint32_t)(xTaskGetTickCount() - s_wifi_operation_started_tick) >=
                pdMS_TO_TICKS(APP_WIFI_OPERATION_TIMEOUT_MS)) {
            strncpy(timed_out_ssid, s_wifi_operation_ssid,
                    sizeof(timed_out_ssid) - 1U);
            timed_out_rssi = s_wifi_operation_rssi;
            s_wifi_operation = APP_WIFI_OPERATION_NONE;
            s_wifi_operation_ssid[0] = '\0';
            s_wifi_operation_rssi = -127;
            ++s_wifi_operation_generation;
            timed_out = true;
        }
        xSemaphoreGive(s_services_mutex);

        if (timed_out) {
            ESP_LOGW(APP_SERVICES_TAG, "Wi-Fi operation timed out after %ums",
                     (unsigned)APP_WIFI_OPERATION_TIMEOUT_MS);
            lcd_show_wifi_result(false, true, true,
                                 timed_out_ssid[0] ? timed_out_ssid : "Wi-Fi",
                                 timed_out_rssi, "Connection timed out");
        }
    }
}

static void app_wifi_begin_operation(app_wifi_operation_t operation,
                                     const char *ssid, int8_t rssi)
{
    if (s_services_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_operation = operation;
    ++s_wifi_operation_generation;
    s_wifi_operation_started_tick = xTaskGetTickCount();
    s_wifi_operation_rssi = rssi;
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
    s_wifi_operation_rssi = -127;
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

static esp_err_t app_services_execute_wifi_toggle(bool enabled,
                                                    bool previous_enabled)
{
    const uint64_t started_ms = (uint64_t)(esp_timer_get_time() / 1000LL);

    if (!enabled) {
        /* Service shutdown can include MQTT, mDNS, HTTP, and WebSocket
         * teardown. Keep it off the button and panel-input task. */
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

    esp_err_t nvs_err = ESP_OK;
    if (controller_err == ESP_OK) {
        nvs_err = persist_u8(APP_WIFI_ENABLED_KEY, enabled ? 1U : 0U);
        if (nvs_err != ESP_OK) {
            ESP_LOGE(APP_SERVICES_TAG, "Could not persist Wi-Fi intent: %s",
                     esp_err_to_name(nvs_err));
        }
    }

    if (controller_err == ESP_OK && app_wifi_operation_pending()) {
        /* Radio start/stop is the ON/OFF transition. Station association and
         * DHCP are separate operations and must not hold this operation open. */
        sys_state.wifi.enabled = enabled;
        sys_state.inverter.wifi_enabled = enabled;
        app_wifi_end_operation();
        lcd_flash_message(enabled ? "Wi-Fi ON" : "Wi-Fi OFF", "Ready", 900U);
    } else if (controller_err != ESP_OK && controller_err != ESP_ERR_WIFI_CONN) {
        sys_state.wifi.enabled = previous_enabled;
        sys_state.inverter.wifi_enabled = previous_enabled;
        app_wifi_end_operation();
        if (enabled && controller_err == ESP_ERR_NOT_FOUND) {
            lcd_flash_message("Wi-Fi Not Config", "Use menuconfig", 1800U);
        } else {
            lcd_flash_message(enabled ? "Wi-Fi Start Failed" : "Wi-Fi Stop Failed",
                              "Try again", 1500U);
        }
    }

    ESP_LOGI(APP_SERVICES_TAG,
             "Wi-Fi %s worker finished in %llums (controller=%s, nvs=%s)",
             enabled ? "ON" : "OFF",
             (unsigned long long)((uint64_t)(esp_timer_get_time() / 1000LL) - started_ms),
             esp_err_to_name(controller_err), esp_err_to_name(nvs_err));
    return controller_err != ESP_OK ? controller_err : nvs_err;
}

static void app_wifi_toggle_task(void *parameter)
{
    (void)parameter;
    task_watchdog_register("wifi_toggle_task");

    wifi_toggle_request_t request;
    while (true) {
        if (xQueueReceive(s_wifi_toggle_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        task_watchdog_feed();
        const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        ESP_LOGI(APP_SERVICES_TAG,
                 "Wi-Fi %s worker dispatch after %llums",
                 request.enabled ? "ON" : "OFF",
                 (unsigned long long)(now_ms >= request.requested_ms
                                          ? now_ms - request.requested_ms
                                          : 0U));
        (void)app_services_execute_wifi_toggle(request.enabled,
                                                request.previous_enabled);
        task_watchdog_feed();
    }
}

static void app_wifi_status_callback(const wifi_status_t *status)
{
    if (status == NULL || s_services_mutex == NULL) {
        return;
    }

    app_wifi_operation_t operation;
    char ssid[sizeof(s_wifi_operation_ssid)] = {0};
    int8_t operation_rssi = -127;
    bool terminal = false;
    bool connected = false;
    bool failed = false;
    const char *message = NULL;

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    operation = s_wifi_operation;
    strncpy(ssid, s_wifi_operation_ssid, sizeof(ssid) - 1U);
    operation_rssi = s_wifi_operation_rssi;
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
        const char *display_ssid = ssid[0] != '\0' ? ssid : message;
        const char *detail = connected ? "Connection succeeded" : message;
        lcd_show_wifi_result(connected, failed, false, display_ssid,
                             status->rssi != -127 ? status->rssi : operation_rssi,
                             detail);
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
    if (s_manifest_url[0] != '\0' && !app_manifest_url_is_valid(s_manifest_url)) {
        ESP_LOGW(APP_SERVICES_TAG, "Persisted OTA manifest URL rejected");
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

static void app_services_show_ota_lcd(const app_ota_status_t *status)
{
    if (!status) {
        return;
    }
    lcd_ota_view_state_t view = LCD_OTA_VIEW_ERROR;
    switch (status->state) {
    case APP_OTA_CHECKING: view = LCD_OTA_VIEW_CHECKING; break;
    case APP_OTA_PREPARING: view = LCD_OTA_VIEW_PREPARING; break;
    case APP_OTA_DOWNLOADING: view = LCD_OTA_VIEW_DOWNLOADING; break;
    case APP_OTA_VERIFYING: view = LCD_OTA_VIEW_VERIFYING; break;
    case APP_OTA_AVAILABLE: view = LCD_OTA_VIEW_AVAILABLE; break;
    case APP_OTA_COMPLETE: view = LCD_OTA_VIEW_COMPLETE; break;
    case APP_OTA_CANCELLED: view = LCD_OTA_VIEW_CANCELLED; break;
    case APP_OTA_IDLE: view = LCD_OTA_VIEW_CURRENT; break;
    case APP_OTA_ERROR: view = LCD_OTA_VIEW_ERROR; break;
    case APP_OTA_CONFIRMING:
    default:
        return;
    }
    const char *detail = status->state == APP_OTA_ERROR ? "Try again" :
                         status->state == APP_OTA_CANCELLED ? "Current kept" :
                         status->state == APP_OTA_IDLE ? "No update" : "";
    lcd_show_ota_status(view, (uint8_t)(status->progress_percent < 0 ? 0 :
                                       status->progress_percent),
                        status->installed_version, status->available_version,
                        detail, status->state == APP_OTA_ERROR ||
                                  status->state == APP_OTA_CANCELLED);
}

static const char *app_ota_state_text(app_ota_state_t state)
{
    switch (state) {
    case APP_OTA_IDLE: return "idle";
    case APP_OTA_CHECKING: return "checking";
    case APP_OTA_AVAILABLE: return "available";
    case APP_OTA_CONFIRMING: return "confirming";
    case APP_OTA_PREPARING: return "preparing";
    case APP_OTA_DOWNLOADING: return "downloading";
    case APP_OTA_VERIFYING: return "verifying";
    case APP_OTA_COMPLETE: return "complete";
    case APP_OTA_ERROR: return "error";
    case APP_OTA_CANCELLED: return "cancelled";
    default: return "unknown";
    }
}

static void ota_progress_callback(int percent)
{
    if (!s_services_mutex) {
        return;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_ota_status.state = APP_OTA_DOWNLOADING;
    s_ota_status.progress_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    app_ota_status_t snapshot = s_ota_status;
    xSemaphoreGive(s_services_mutex);
    websocket_broadcast_ota_status(app_ota_state_text(snapshot.state),
                                   snapshot.progress_percent,
                                   snapshot.available_version,
                                   snapshot.error_detail);
    app_services_show_ota_lcd(&snapshot);
}

static void ota_status_callback(ota_status_t status, int percent)
{
    if (!s_services_mutex) {
        return;
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_ota_status.progress_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    switch (status) {
    case OTA_STATUS_STARTED:
        s_ota_status.state = APP_OTA_PREPARING;
        s_ota_status.error_detail[0] = '\0';
        break;
    case OTA_STATUS_DOWNLOADING:
        s_ota_status.state = APP_OTA_DOWNLOADING;
        break;
    case OTA_STATUS_VERIFYING:
        s_ota_status.state = APP_OTA_VERIFYING;
        break;
    case OTA_STATUS_SUCCESS:
        s_ota_status.state = APP_OTA_COMPLETE;
        s_ota_status.error_detail[0] = '\0';
        s_ota_status.confirmation_pending = false;
        s_ota_status.cancel_confirmation_pending = false;
        break;
    case OTA_STATUS_CANCELLED:
        s_ota_status.state = APP_OTA_CANCELLED;
        s_ota_status.confirmation_pending = false;
        s_ota_status.cancel_confirmation_pending = false;
        break;
    case OTA_STATUS_FAILED:
        s_ota_status.state = APP_OTA_ERROR;
        snprintf(s_ota_status.error_detail, sizeof(s_ota_status.error_detail),
                 "ERR:IMAGE");
        s_ota_status.confirmation_pending = false;
        s_ota_status.cancel_confirmation_pending = false;
        break;
    case OTA_STATUS_IDLE:
    default:
        break;
    }
    app_ota_status_t snapshot = s_ota_status;
    xSemaphoreGive(s_services_mutex);
    websocket_broadcast_ota_status(app_ota_state_text(snapshot.state),
                                   snapshot.progress_percent,
                                   snapshot.available_version,
                                   snapshot.error_detail);
    app_services_show_ota_lcd(&snapshot);
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
            status.state != APP_OTA_PREPARING &&
            status.state != APP_OTA_DOWNLOADING &&
            status.state != APP_OTA_VERIFYING &&
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
    s_ota_manifest_check_task = NULL;
    s_ota_manifest_check_active = false;
    s_ota_check_cancel_requested = false;
    s_ota_cancel_confirmation_pending = false;
    s_wifi_operation = APP_WIFI_OPERATION_NONE;
    s_wifi_operation_ssid[0] = '\0';
    s_wifi_operation_generation = 0U;
    s_wifi_operation_started_tick = 0U;
    s_wifi_toggle_task = NULL;
    s_wifi_toggle_queue = NULL;
    s_wifi_scan_active = false;
    s_wifi_scan_cancel_requested = false;
    s_wifi_scan_task = NULL;
    s_wifi_forget_pending = false;
    s_wifi_disconnect_pending = false;
    xSemaphoreGive(s_services_mutex);

    load_persisted_config();

    if (s_manifest_url[0] == '\0')
    {
        const char *bootstrap_manifest_url = APP_OTA_DEFAULT_MANIFEST_URL;
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
                        NULL, APP_OTA_CHECK_TASK_PRIORITY, &s_ota_check_task) != pdPASS)
        {
            s_ota_check_task = NULL;
            ESP_LOGW(APP_SERVICES_TAG, "Could not create OTA availability task");
        }
    }
    if (!s_wifi_toggle_queue)
    {
        s_wifi_toggle_queue = xQueueCreate(WIFI_TOGGLE_QUEUE_LENGTH,
                                           sizeof(wifi_toggle_request_t));
        if (!s_wifi_toggle_queue) {
            ESP_LOGE(APP_SERVICES_TAG, "Could not create Wi-Fi toggle queue");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_wifi_toggle_task)
    {
        if (xTaskCreate(app_wifi_toggle_task,
                        "wifi_toggle",
                        APP_WIFI_TOGGLE_TASK_STACK_SIZE,
                        NULL,
                        APP_WIFI_TOGGLE_TASK_PRIORITY,
                        &s_wifi_toggle_task) != pdPASS)
        {
            vQueueDelete(s_wifi_toggle_queue);
            s_wifi_toggle_queue = NULL;
            s_wifi_toggle_task = NULL;
            ESP_LOGW(APP_SERVICES_TAG, "Could not create Wi-Fi toggle worker");
            return ESP_ERR_NO_MEM;
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
    if (s_services_mutex == NULL || s_wifi_toggle_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char ssid[WIFI_MAX_SSID_LEN + 1U] = {0};
    if (enabled) {
        strncpy(ssid, WIFI_COMPILED_STA_SSID, sizeof(ssid) - 1U);
    }

    wifi_toggle_request_t request = {
        .enabled = enabled,
        .previous_enabled = sys_state.wifi.enabled,
        .requested_ms = (uint64_t)(esp_timer_get_time() / 1000ULL),
    };

    app_wifi_begin_operation(enabled ? APP_WIFI_OPERATION_ENABLE
                                     : APP_WIFI_OPERATION_DISABLE,
                             ssid, -127);

    if (xQueueSend(s_wifi_toggle_queue, &request, 0) != pdTRUE) {
        app_wifi_end_operation();
        lcd_flash_message("Wi-Fi Busy", "Please wait", 900U);
        return ESP_ERR_TIMEOUT;
    }

    /* This is an operation-progress indication, not a claim that the radio
     * or station connection is already complete. Actual Wi-Fi events update
     * the terminal result asynchronously. */
    lcd_flash_message(enabled ? "Wi-Fi STARTING" : "Wi-Fi STOPPING",
                      "Please wait", 900U);
    ESP_LOGI(APP_SERVICES_TAG, "Wi-Fi %s request queued",
             enabled ? "ON" : "OFF");
    return ESP_OK;
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

    if (!active) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Stop the lower-level ESP-IDF scan immediately when one is in progress;
     * the worker task will then publish any results the scan API retained. */
    const esp_err_t err = wifi_scan_cancel();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return ESP_OK;
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

void app_services_show_wifi_network_details(uint8_t selected_index)
{
    char ssid[LCD_WIFI_SSID_MAX_LEN + 1U] = {0};
    int8_t rssi = 0;
    uint8_t channel = 0U;
    uint8_t authmode = 0U;

    LCD_LOCK();
    if (sys_lcd.screen != LCD_SCREEN_WIFI_SCAN ||
        sys_lcd.wifi_scan.stage != LCD_WIFI_SCAN_COMPLETE ||
        selected_index >= sys_lcd.wifi_scan.count) {
        LCD_UNLOCK();
        return;
    }
    strncpy(ssid, sys_lcd.wifi_scan.ssid[selected_index], sizeof(ssid) - 1U);
    rssi = sys_lcd.wifi_scan.rssi[selected_index];
    channel = sys_lcd.wifi_scan.channel[selected_index];
    authmode = sys_lcd.wifi_scan.authmode[selected_index];
    LCD_UNLOCK();
    lcd_show_wifi_network_details(ssid, rssi, channel, authmode);
}

static esp_err_t app_services_wifi_connect_network_with_authmode(
    const char *ssid, const char *password, int8_t rssi,
    wifi_auth_mode_t authmode)
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
    config.authmode = authmode;
    err = wifi_manager_set_config(&config);
    if (err != ESP_OK) {
        lcd_flash_message("Network Invalid", "Try again", 1400U);
        return err;
    }

    wifi_credentials_t credentials = {0};
    strncpy(credentials.ssid, ssid, sizeof(credentials.ssid) - 1U);
    strncpy(credentials.password, password, sizeof(credentials.password) - 1U);
    (void)wifi_storage_save_credentials(&credentials);

    app_wifi_begin_operation(APP_WIFI_OPERATION_CONNECT_SAVED, ssid, rssi);
    lcd_show_wifi_connecting(ssid, rssi);
    err = wifi_controller_reconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        app_wifi_end_operation();
        lcd_show_wifi_result(false, true, false, ssid, rssi, "Connect failed");
    }
    return err;
}

esp_err_t app_services_wifi_connect_selected(uint8_t selected_index)
{
    char ssid[LCD_WIFI_SSID_MAX_LEN + 1U] = {0};
    int8_t selected_rssi = -127;
    uint8_t authmode = (uint8_t)WIFI_AUTH_OPEN;
    LCD_LOCK();
    if (sys_lcd.screen == LCD_SCREEN_WIFI_NETWORK_DETAILS) {
        strncpy(ssid, sys_lcd.wifi_network_detail.ssid, sizeof(ssid) - 1U);
        selected_rssi = sys_lcd.wifi_network_detail.rssi;
        authmode = sys_lcd.wifi_network_detail.authmode;
    } else if (sys_lcd.screen == LCD_SCREEN_WIFI_SCAN &&
               sys_lcd.wifi_scan.stage == LCD_WIFI_SCAN_COMPLETE &&
               selected_index < sys_lcd.wifi_scan.count) {
        strncpy(ssid, sys_lcd.wifi_scan.ssid[selected_index], sizeof(ssid) - 1U);
        selected_rssi = sys_lcd.wifi_scan.rssi[selected_index];
        authmode = sys_lcd.wifi_scan.authmode[selected_index];
    } else {
        LCD_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    LCD_UNLOCK();

    if ((wifi_auth_mode_t)authmode == WIFI_AUTH_OPEN) {
        return app_services_wifi_connect_network_with_authmode(
            ssid, "", selected_rssi, WIFI_AUTH_OPEN);
    }

    lcd_show_wifi_password(ssid, selected_rssi, authmode);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t app_services_wifi_submit_password(void)
{
    char ssid[LCD_WIFI_SSID_MAX_LEN + 1U] = {0};
    char password[LCD_WIFI_PASSWORD_MAX_LEN + 1U] = {0};
    int8_t rssi = -127;
    uint8_t authmode = (uint8_t)WIFI_AUTH_OPEN;
    LCD_LOCK();
    if (sys_lcd.screen != LCD_SCREEN_WIFI_PASSWORD) {
        LCD_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    strncpy(ssid, sys_lcd.wifi_password.ssid, sizeof(ssid) - 1U);
    strncpy(password, sys_lcd.wifi_password.password, sizeof(password) - 1U);
    rssi = sys_lcd.wifi_password.rssi;
    authmode = sys_lcd.wifi_password.authmode;
    LCD_UNLOCK();

    if (password[0] == '\0') {
        lcd_flash_message("ENTER PASSWORD", "Hold=submit", 1200U);
        return ESP_ERR_INVALID_STATE;
    }

    return app_services_wifi_connect_network_with_authmode(
        ssid, password, rssi, (wifi_auth_mode_t)authmode);
}

esp_err_t app_services_wifi_connect_network(const char *ssid,
                                               const char *password)
{
    return app_services_wifi_connect_network_with_rssi(ssid, password, -127);
}

esp_err_t app_services_wifi_connect_network_with_rssi(const char *ssid,
                                                      const char *password,
                                                      int8_t rssi)
{
    return app_services_wifi_connect_network_with_authmode(
        ssid, password, rssi, INVERTER_WIFI_AUTH_MODE);
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
    app_wifi_begin_operation(APP_WIFI_OPERATION_CONNECT_SAVED, config.ssid, -127);
    lcd_show_wifi_connecting(config.ssid, -127);
    esp_err_t err = wifi_controller_reconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        app_wifi_end_operation();
        lcd_show_wifi_result(false, true, false, config.ssid, -127, "Connect failed");
    }
    return err;
}

esp_err_t app_services_wifi_disconnect(void)
{
    app_wifi_begin_operation(APP_WIFI_OPERATION_DISCONNECT, NULL, -127);
    const esp_err_t err = wifi_controller_disconnect();
    if (err != ESP_OK) {
        app_wifi_end_operation();
        lcd_show_wifi_result(false, true, false, "", -127, "Disconnect failed");
    } else if (!wifi_controller_is_connected() && app_wifi_operation_pending()) {
        app_wifi_end_operation();
        lcd_show_wifi_result(false, false, false, "", -127, "Disconnected");
    }
    return err;
}

esp_err_t app_services_wifi_start_provisioning(void)
{
#if WIFI_RUNTIME_PROVISIONING_ENABLED
    /* The captive portal owns port 80 only while the normal dashboard/API
     * server is stopped. The Wi-Fi controller remains the owner of the
     * provisioning radio and captive-DNS lifecycle. */
    (void)network_services_stop();
    return wifi_controller_start_provisioning();
#else
    lcd_flash_message("Setup AP Disabled", "Use menuconfig", 1800U);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void app_services_show_wifi_status(void)
{
    wifi_status_t status = {0};
    wifi_manager_config_t config = {0};
    (void)wifi_events_get_status_copy(&status);
    (void)wifi_manager_get_config(&config);

    char ssid[WIFI_MAX_SSID_LEN + 1U] = {0};
    if (wifi_manager_get_mode() == WIFI_MODE_AP) {
        strncpy(ssid, config.ap_ssid, sizeof(ssid) - 1U);
    } else {
        strncpy(ssid, config.ssid, sizeof(ssid) - 1U);
    }
    if (ssid[0] == '\0') {
        strncpy(ssid, "Not configured", sizeof(ssid) - 1U);
    }

    char ip[16] = "0.0.0.0";
    char gateway[16] = "0.0.0.0";
    if (status.got_ip) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&status.ip));
        snprintf(gateway, sizeof(gateway), IPSTR, IP2STR(&status.gateway));
    }

    char state_text[LCD_LINE_SIZE] = {0};
    if (!sys_state.wifi.enabled) {
        snprintf(state_text, sizeof(state_text), "Wi-Fi OFF");
    } else if (wifi_manager_get_mode() == WIFI_MODE_AP) {
        snprintf(state_text, sizeof(state_text), "AP active");
    } else {
        snprintf(state_text, sizeof(state_text), "%s",
                 wifi_state_text(wifi_controller_get_state()));
    }
    const int8_t rssi = status.connected ? wifi_manager_get_rssi() : status.rssi;
    lcd_show_wifi_status(state_text, ssid, ip, gateway, rssi,
                         status.connected, status.got_ip,
                         status.connected && status.internet_available);
}

const char *app_services_wifi_saved_network_label(void)
{
    static char label[LCD_LINE_SIZE];
    wifi_credentials_t credentials = {0};
    if (wifi_storage_load_credentials(&credentials) == ESP_OK &&
        credentials.ssid[0] != '\0') {
        snprintf(label, sizeof(label), "Saved: %.9s", credentials.ssid);
    } else {
        snprintf(label, sizeof(label), "Saved: None");
    }
    return label;
}

bool app_services_wifi_forget_confirmation_pending(void)
{
    bool pending = false;
    if (s_services_mutex != NULL) {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        pending = s_wifi_forget_pending;
        xSemaphoreGive(s_services_mutex);
    }
    return pending;
}

esp_err_t app_services_wifi_request_forget_saved(void)
{
    wifi_credentials_t credentials = {0};
    if (wifi_storage_load_credentials(&credentials) != ESP_OK ||
        credentials.ssid[0] == '\0') {
        lcd_flash_message("No Saved Network", "Nothing to forget", 1400U);
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_forget_pending = true;
    xSemaphoreGive(s_services_mutex);
    lcd_show_confirm("Forget network?", "Enter=Yes Back=No");
    return ESP_OK;
}

esp_err_t app_services_wifi_confirm_forget_saved(void)
{
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_forget_pending = false;
    xSemaphoreGive(s_services_mutex);

    esp_err_t err = wifi_storage_erase_credentials();
    if (err != ESP_OK) {
        lcd_flash_message("Forget failed", "Try again", 1400U);
        return err;
    }

    wifi_manager_config_t config = {0};
    if (wifi_manager_get_config(&config) == ESP_OK) {
        config.ssid[0] = '\0';
        config.password[0] = '\0';
        (void)wifi_manager_set_config(&config);
    }
    if (wifi_controller_is_connected()) {
        (void)wifi_controller_disconnect();
    }
    lcd_flash_message("Network Forgotten", "Wi-Fi ready", 1200U);
    return ESP_OK;
}

void app_services_wifi_cancel_forget_saved(void)
{
    if (s_services_mutex != NULL) {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_wifi_forget_pending = false;
        xSemaphoreGive(s_services_mutex);
    }
}

bool app_services_wifi_disconnect_confirmation_pending(void)
{
    bool pending = false;
    if (s_services_mutex != NULL) {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        pending = s_wifi_disconnect_pending;
        xSemaphoreGive(s_services_mutex);
    }
    return pending;
}

esp_err_t app_services_wifi_request_disconnect(void)
{
    if (!wifi_controller_is_connected()) {
        return app_services_wifi_reconnect();
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_disconnect_pending = true;
    xSemaphoreGive(s_services_mutex);
    lcd_show_confirm("Disconnect Wi-Fi?", "Enter=Yes Back=No");
    return ESP_OK;
}

esp_err_t app_services_wifi_confirm_disconnect(void)
{
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_disconnect_pending = false;
    xSemaphoreGive(s_services_mutex);
    return app_services_wifi_disconnect();
}

void app_services_wifi_cancel_disconnect(void)
{
    if (s_services_mutex != NULL) {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_wifi_disconnect_pending = false;
        xSemaphoreGive(s_services_mutex);
    }
}

bool app_services_wifi_dhcp_enabled(void)
{
    wifi_manager_config_t config = {0};
    return wifi_manager_get_config(&config) == ESP_OK ? config.dhcp : true;
}

esp_err_t app_services_wifi_toggle_dhcp(void)
{
    wifi_manager_config_t config = {0};
    esp_err_t err = wifi_manager_get_config(&config);
    if (err != ESP_OK) {
        return err;
    }
    config.dhcp = !config.dhcp;
    err = wifi_manager_set_config(&config);
    if (err != ESP_OK) {
        return err;
    }

    wifi_network_config_t stored = {0};
    if (wifi_storage_load_network_config(&stored) != ESP_OK) {
        wifi_storage_set_default_network_config(&stored);
    }
    stored.dhcp = config.dhcp;
    stored.ip_info = config.ip_info;
    stored.dns = config.dns;
    err = wifi_storage_save_network_config(&stored);
    if (err == ESP_OK) {
        lcd_flash_message(config.dhcp ? "DHCP ON" : "DHCP OFF",
                          wifi_controller_is_connected() ? "Reconnect needed" : "Saved",
                          1400U);
    }
    return err;
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
        (url[0] != '\0' && !app_manifest_url_is_valid(url)))
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

typedef struct {
    char manifest_url[APP_OTA_MANIFEST_URL_MAX];
    bool user_initiated;
} ota_manifest_check_job_t;

static void ota_manifest_check_task(void *parameter)
{
    ota_manifest_check_job_t *job = (ota_manifest_check_job_t *)parameter;
    if (!job) {
        vTaskDelete(NULL);
        return;
    }

    ota_manifest_entry_t entry = {0};
    bool update_available = false;
    const esp_err_t err = ota_service_check_csv_manifest(job->manifest_url,
                                                         &entry,
                                                         &update_available);
    bool cancelled = false;
    app_ota_status_t snapshot;
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    cancelled = s_ota_check_cancel_requested;
    s_ota_check_cancel_requested = false;
    if (cancelled) {
        s_ota_status.state = APP_OTA_CANCELLED;
        s_ota_status.progress_percent = 0;
    } else if (err == ESP_OK && update_available) {
        s_ota_status.state = APP_OTA_AVAILABLE;
        s_ota_status.error_detail[0] = '\0';
        s_ota_status.update_available = true;
        strncpy(s_ota_status.available_version, entry.version,
                sizeof(s_ota_status.available_version) - 1U);
    } else if (err == ESP_OK) {
        s_ota_status.state = APP_OTA_IDLE;
        s_ota_status.error_detail[0] = '\0';
        s_ota_status.update_available = false;
        s_ota_status.available_version[0] = '\0';
    } else {
        s_ota_status.state = APP_OTA_ERROR;
        snprintf(s_ota_status.error_detail, sizeof(s_ota_status.error_detail),
                 "ERR:%s", esp_err_to_name(err));
        /* Preserve a previously valid available manifest so Install/Retry can
         * still begin a clean, freshly downloaded OTA transaction. */
    }
    snapshot = s_ota_status;
    s_ota_manifest_check_task = NULL;
    s_ota_manifest_check_active = false;
    xSemaphoreGive(s_services_mutex);

    if (err != ESP_OK && !cancelled) {
        ESP_LOGW(APP_SERVICES_TAG, "OTA check failed: %s", esp_err_to_name(err));
    }
    if (job->user_initiated || update_available || cancelled) {
        app_services_show_ota_lcd(&snapshot);
    }
    memset(job, 0, sizeof(*job));
    free(job);
    vTaskDelete(NULL);
}

esp_err_t app_services_check_for_update(bool user_initiated)
{
    char manifest_url[APP_OTA_MANIFEST_URL_MAX] = {0};
    if (!wifi_controller_is_connected()) {
        if (user_initiated) {
            const bool enabled = app_services_wifi_enabled();
            lcd_flash_warning_to(enabled ? "Wi-Fi ON" : "Wi-Fi OFF",
                                 enabled ? "Connect first" : "Turn Wi-Fi ON",
                                 1800U, LCD_SCREEN_MENU);
        }
        ESP_LOGW(APP_SERVICES_TAG, "Update check skipped: station is not connected");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_monitor_is_online()) {
        if (user_initiated) {
            lcd_flash_warning_to("Wi-Fi Connected", "Internet OFF",
                                 1800U, LCD_SCREEN_MENU);
        }
        ESP_LOGW(APP_SERVICES_TAG, "Internet Not Available; OTA check skipped");
        return ESP_ERR_INVALID_STATE;
    }
    if (app_services_get_ota_manifest_url(manifest_url, sizeof(manifest_url)) != ESP_OK ||
        strncmp(manifest_url, "https://", 8U) != 0 ||
        strlen(manifest_url) >= sizeof(manifest_url)) {
        if (user_initiated) {
            lcd_flash_warning_to("OTA URL Invalid", "HTTPS required",
                                 1500U, LCD_SCREEN_MENU);
        }
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool busy = s_ota_manifest_check_active ||
                      s_ota_status.state == APP_OTA_PREPARING ||
                      s_ota_status.state == APP_OTA_DOWNLOADING ||
                      s_ota_status.state == APP_OTA_VERIFYING ||
                      s_ota_status.state == APP_OTA_CHECKING;
    if (busy) {
        xSemaphoreGive(s_services_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    ota_manifest_check_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        xSemaphoreGive(s_services_mutex);
        return ESP_ERR_NO_MEM;
    }
    strncpy(job->manifest_url, manifest_url, sizeof(job->manifest_url) - 1U);
    job->user_initiated = user_initiated;
    s_ota_check_cancel_requested = false;
    s_ota_status.state = APP_OTA_CHECKING;
    s_ota_status.progress_percent = 0;
    s_ota_manifest_check_active = true;
    xSemaphoreGive(s_services_mutex);

    if (xTaskCreate(ota_manifest_check_task, "ota_manifest_check",
                    APP_OTA_CHECK_TASK_STACK_SIZE, job,
                    APP_OTA_CHECK_TASK_PRIORITY, &s_ota_manifest_check_task) != pdPASS) {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_ota_manifest_check_task = NULL;
        s_ota_manifest_check_active = false;
        s_ota_status.state = APP_OTA_ERROR;
        xSemaphoreGive(s_services_mutex);
        memset(job, 0, sizeof(*job));
        free(job);
        return ESP_ERR_NO_MEM;
    }
    app_ota_status_t snapshot;
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    snapshot = s_ota_status;
    xSemaphoreGive(s_services_mutex);
    if (user_initiated) {
        app_services_show_ota_lcd(&snapshot);
    }
    return ESP_OK;
}

esp_err_t app_services_request_update_confirmation(void)
{
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool available = s_ota_status.update_available;
    const bool busy = s_ota_status.state == APP_OTA_PREPARING ||
                      s_ota_status.state == APP_OTA_DOWNLOADING ||
                      s_ota_status.state == APP_OTA_VERIFYING ||
                      s_ota_status.state == APP_OTA_CHECKING;
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
    app_ota_status_t snapshot = {0};
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool confirmed = s_ota_status.confirmation_pending;
    if (confirmed)
    {
        s_ota_status.confirmation_pending = false;
        s_ota_status.cancel_confirmation_pending = false;
        s_ota_status.state = APP_OTA_PREPARING;
        s_ota_status.progress_percent = 0;
    }
    strncpy(manifest_url, s_manifest_url, sizeof(manifest_url) - 1U);
    snapshot = s_ota_status;
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
        lcd_flash_warning_to("Wi-Fi Connected", "Internet OFF",
                             1800U, LCD_SCREEN_MENU);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = ota_service_start_from_csv(manifest_url);
    if (err != ESP_OK)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_ota_status.state = APP_OTA_ERROR;
        snprintf(s_ota_status.error_detail, sizeof(s_ota_status.error_detail),
                 "ERR:%s", esp_err_to_name(err));
        snapshot = s_ota_status;
        xSemaphoreGive(s_services_mutex);
        app_services_show_ota_lcd(&snapshot);
        return err;
    }
    lcd_show_ota_status(LCD_OTA_VIEW_PREPARING, 0U,
                        snapshot.installed_version, snapshot.available_version,
                        "", false);
    return ESP_OK;
}

esp_err_t app_services_request_cancel_update(void)
{
    const bool service_busy = ota_service_in_progress();
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool check_busy = s_ota_manifest_check_active;
    if (service_busy || check_busy) {
        s_ota_cancel_confirmation_pending = true;
    }
    xSemaphoreGive(s_services_mutex);
    if (!service_busy && !check_busy) {
        lcd_flash_message("No Update Running", "Current kept", 1200U);
        return ESP_ERR_INVALID_STATE;
    }
    lcd_show_confirm("Cancel update?", "ENTER=Yes BACK=No");
    return ESP_OK;
}

esp_err_t app_services_confirm_cancel_update(void)
{
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool pending = s_ota_cancel_confirmation_pending;
    s_ota_cancel_confirmation_pending = false;
    xSemaphoreGive(s_services_mutex);
    if (!pending) {
        return ESP_ERR_INVALID_STATE;
    }
    return app_services_cancel_update();
}

void app_services_cancel_cancel_update(void)
{
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_ota_cancel_confirmation_pending = false;
    xSemaphoreGive(s_services_mutex);
}

bool app_services_ota_cancel_confirmation_pending(void)
{
    bool pending = false;
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    pending = s_ota_cancel_confirmation_pending;
    xSemaphoreGive(s_services_mutex);
    return pending;
}

esp_err_t app_services_cancel_update(void)
{
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool pending = s_ota_status.confirmation_pending;
    const bool checking = s_ota_manifest_check_active;
    s_ota_status.confirmation_pending = false;
    s_ota_cancel_confirmation_pending = false;
    if (pending) {
        s_ota_status.state = APP_OTA_AVAILABLE;
    }
    if (checking) {
        s_ota_check_cancel_requested = true;
        s_ota_status.state = APP_OTA_CANCELLED;
    }
    xSemaphoreGive(s_services_mutex);

    if (ota_service_in_progress()) {
        lcd_show_ota_status(LCD_OTA_VIEW_CANCELLING, 0U, "", "",
                            "Please wait", false);
        return ota_service_cancel();
    }
    if (pending) {
        lcd_flash_message("Update Deferred", "Current kept", 1200U);
    } else if (checking) {
        lcd_show_ota_status(LCD_OTA_VIEW_CANCELLED, 0U, "", "",
                            "Current kept", false);
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
