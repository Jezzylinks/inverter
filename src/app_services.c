#include "app_services.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd_writer.h"
#include "nvs.h"
#include "system_state.h"
#include "wifi/wifi_controller.h"
#include "wifi/wifi_scan.h"
#include "wifi/wifi_security.h"
#include "wifi/wifi_storage.h"

#define APP_SERVICES_TAG "APP_SERVICES"
#define APP_SERVICES_NVS_NAMESPACE NVS_NS_SYSTEM
#define APP_WIFI_ENABLED_KEY "wifi_enabled"
#define APP_OTA_MANIFEST_KEY "ota_manifest"
#define APP_OTA_AUTOCHECK_KEY "ota_auto"
#define APP_OTA_CHECK_INTERVAL_MS (6U * 60U * 60U * 1000U)
#define APP_OTA_TASK_STACK_SIZE 4096U
#define APP_OTA_TASK_PRIORITY 3U

extern system_state_t sys_state;
extern SemaphoreHandle_t sys_state_mutex;

static SemaphoreHandle_t s_services_mutex;
static TaskHandle_t s_ota_check_task;
static char s_manifest_url[APP_OTA_MANIFEST_URL_MAX];
static app_ota_status_t s_ota_status;

static esp_err_t persist_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_SERVICES_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t persist_manifest_url(const char *url)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_SERVICES_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if (url[0] == '\0') {
        err = nvs_erase_key(handle, APP_OTA_MANIFEST_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_str(handle, APP_OTA_MANIFEST_KEY, url);
    }
    if (err == ESP_OK) {
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
    if (err == ESP_OK) {
        (void)nvs_get_u8(handle, APP_WIFI_ENABLED_KEY, &wifi_enabled);
        (void)nvs_get_u8(handle, APP_OTA_AUTOCHECK_KEY, &auto_check);
        size_t length = sizeof(s_manifest_url);
        err = nvs_get_str(handle, APP_OTA_MANIFEST_KEY, s_manifest_url, &length);
        if (err != ESP_OK) {
            s_manifest_url[0] = '\0';
        }
        nvs_close(handle);
    } else {
        s_manifest_url[0] = '\0';
    }

    sys_state.wifi.enabled = wifi_enabled != 0U;
    sys_state.inverter.wifi_enabled = sys_state.wifi.enabled;
    s_ota_status.auto_check_enabled = auto_check != 0U;
}

static const char *wifi_state_text(wifi_controller_state_t state)
{
    switch (state) {
    case WIFI_CONTROLLER_CONNECTED:
        return "Connected";
    case WIFI_CONTROLLER_CONNECTING:
    case WIFI_CONTROLLER_STARTING:
        return "Connecting";
    case WIFI_CONTROLLER_PROVISIONING:
        return "Setup AP Active";
    case WIFI_CONTROLLER_ERROR:
        return "Controller Error";
    case WIFI_CONTROLLER_IDLE:
    default:
        return "Disabled";
    }
}

static void ota_progress_callback(int percent)
{
    if (!s_services_mutex) {
        return;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_ota_status.state = APP_OTA_DOWNLOADING;
    s_ota_status.progress_percent = percent;
    xSemaphoreGive(s_services_mutex);
}

static void ota_status_callback(ota_status_t status, int percent)
{
    if (!s_services_mutex) {
        return;
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_ota_status.progress_percent = percent;
    switch (status) {
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

    if (status == OTA_STATUS_VERIFYING) {
        lcd_flash_message("Verifying Update", "Please wait", 1200U);
    } else if (status == OTA_STATUS_FAILED) {
        lcd_flash_message("Update Failed", "Current kept", 1800U);
    } else if (status == OTA_STATUS_CANCELLED) {
        lcd_flash_message("Update Cancelled", "Current kept", 1200U);
    } else if (status == OTA_STATUS_SUCCESS) {
        lcd_flash_message("Update Complete", "Restarting", 800U);
    }
}

static void ota_auto_check_task(void *parameter)
{
    (void)parameter;
    vTaskDelay(pdMS_TO_TICKS(30000U));
    while (true) {
        app_ota_status_t status;
        app_services_get_ota_status(&status);
        if (status.auto_check_enabled &&
            status.state != APP_OTA_DOWNLOADING &&
            status.state != APP_OTA_CHECKING &&
            wifi_controller_is_connected()) {
            (void)app_services_check_for_update(false);
        }
        vTaskDelay(pdMS_TO_TICKS(APP_OTA_CHECK_INTERVAL_MS));
    }
}

esp_err_t app_services_init(void)
{
    if (!s_services_mutex) {
        s_services_mutex = xSemaphoreCreateMutex();
        if (!s_services_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    memset(&s_ota_status, 0, sizeof(s_ota_status));
    s_ota_status.state = APP_OTA_IDLE;
    xSemaphoreGive(s_services_mutex);

    load_persisted_config();

    esp_err_t err = wifi_security_init();
    if (err != ESP_OK) {
        ESP_LOGW(APP_SERVICES_TAG, "Wi-Fi security storage unavailable: %s", esp_err_to_name(err));
    }
    err = wifi_controller_init();
    if (err != ESP_OK) {
        ESP_LOGW(APP_SERVICES_TAG, "Wi-Fi controller unavailable: %s", esp_err_to_name(err));
    } else if (sys_state.wifi.enabled) {
        err = wifi_controller_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(APP_SERVICES_TAG, "Could not restore Wi-Fi state: %s", esp_err_to_name(err));
        }
    }

    const esp_err_t ota_err = ota_service_init();
    if (ota_err != ESP_OK) {
        ESP_LOGW(APP_SERVICES_TAG, "OTA service unavailable: %s", esp_err_to_name(ota_err));
    } else {
        (void)ota_service_register_progress_callback(ota_progress_callback);
        (void)ota_service_register_status_callback(ota_status_callback);
        (void)ota_service_get_current_version(s_ota_status.installed_version,
                                              sizeof(s_ota_status.installed_version));
    }

    if (!s_ota_check_task) {
        if (xTaskCreate(ota_auto_check_task, "ota_check", APP_OTA_TASK_STACK_SIZE,
                        NULL, APP_OTA_TASK_PRIORITY, &s_ota_check_task) != pdPASS) {
            s_ota_check_task = NULL;
            ESP_LOGW(APP_SERVICES_TAG, "Could not create OTA availability task");
        }
    }
    return err == ESP_OK ? ota_err : err;
}

esp_err_t app_services_set_wifi_enabled(bool enabled)
{
    sys_state.wifi.enabled = enabled;
    sys_state.inverter.wifi_enabled = enabled;

    esp_err_t controller_err = ESP_OK;
    if (enabled) {
        controller_err = wifi_controller_start();
        if (controller_err == ESP_ERR_INVALID_STATE) {
            controller_err = wifi_controller_reconnect();
        }
    } else {
        controller_err = wifi_controller_stop();
        if (controller_err == ESP_ERR_INVALID_STATE) {
            controller_err = ESP_OK;
        }
    }

    const esp_err_t nvs_err = persist_u8(APP_WIFI_ENABLED_KEY, enabled ? 1U : 0U);
    if (nvs_err != ESP_OK) {
        ESP_LOGE(APP_SERVICES_TAG, "Could not persist Wi-Fi intent: %s", esp_err_to_name(nvs_err));
    }

    if (controller_err == ESP_OK) {
        lcd_flash_message(enabled ? "Wi-Fi Enabled" : "Wi-Fi Disabled",
                          enabled ? "Connecting..." : "Saved to NVS", 1200U);
    } else {
        lcd_flash_message(enabled ? "Wi-Fi Start Failed" : "Wi-Fi Stop Failed",
                          "Setting saved", 1500U);
    }
    return controller_err != ESP_OK ? controller_err : nvs_err;
}

bool app_services_wifi_enabled(void)
{
    return sys_state.wifi.enabled;
}

esp_err_t app_services_wifi_scan(void)
{
    if (!sys_state.wifi.enabled) {
        lcd_flash_message("Wi-Fi Disabled", "Enable first", 1400U);
        return ESP_ERR_INVALID_STATE;
    }

    wifi_ap_record_t records[WIFI_MAX_SCAN_RESULTS] = {0};
    uint16_t count = WIFI_MAX_SCAN_RESULTS;
    const esp_err_t err = wifi_scan_start_records(records, &count);
    if (err != ESP_OK) {
        lcd_flash_message("Scan Unavailable", "Try again", 1400U);
        return err;
    }
    if (count == 0U) {
        lcd_flash_message("No Networks", "Found", 1200U);
        return ESP_OK;
    }

    const uint8_t display_count = count < LCD_WIFI_MAX_AP ? (uint8_t)count : LCD_WIFI_MAX_AP;
    char ssids[LCD_WIFI_MAX_AP][9] = {{0}};
    int8_t rssi[LCD_WIFI_MAX_AP] = {0};
    for (uint8_t i = 0U; i < display_count; ++i) {
        strncpy(ssids[i], (const char *)records[i].ssid, sizeof(ssids[i]) - 1U);
        rssi[i] = records[i].rssi;
    }
    /* This call only publishes render state; it never waits for button events. */
    lcd_show_wifi_scan(display_count, (const char (*)[9])ssids, rssi, 0U, 0U);
    return ESP_OK;
}

esp_err_t app_services_wifi_connect_saved(void)
{
    if (!sys_state.wifi.enabled) {
        lcd_flash_message("Wi-Fi Disabled", "Enable first", 1400U);
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_storage_has_credentials()) {
        lcd_flash_message("No Saved Wi-Fi", "Start Setup AP", 1500U);
        return ESP_ERR_NOT_FOUND;
    }

    lcd_show_wifi_connecting("Saved network");
    esp_err_t err = wifi_controller_start();
    if (err != ESP_OK) {
        err = wifi_controller_reconnect();
    }
    lcd_show_wifi_result(false, err != ESP_OK, false,
                         err == ESP_OK ? "Connecting..." : "Connect failed");
    return err;
}

esp_err_t app_services_wifi_disconnect(void)
{
    const esp_err_t err = wifi_controller_disconnect();
    lcd_show_wifi_result(false, err != ESP_OK, false,
                         err == ESP_OK ? "Disconnected" : "Disconnect failed");
    return err;
}

esp_err_t app_services_wifi_start_provisioning(void)
{
    if (!sys_state.wifi.enabled) {
        return app_services_set_wifi_enabled(true);
    }
    const esp_err_t err = wifi_controller_start_provisioning();
    if (err == ESP_OK) {
        lcd_flash_message("Setup AP Active", "Use installer", 1500U);
    } else {
        lcd_flash_message("Setup AP Failed", "Try again", 1500U);
    }
    return err;
}

void app_services_show_wifi_status(void)
{
    const wifi_controller_state_t state = wifi_controller_get_state();
    lcd_flash_message("Wi-Fi Status", wifi_state_text(state), 1500U);
}

esp_err_t app_services_set_ota_manifest_url(const char *url)
{
    if (!url || strlen(url) >= sizeof(s_manifest_url) ||
        (url[0] != '\0' && strncmp(url, "https://", 8U) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = persist_manifest_url(url);
    if (err == ESP_OK) {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        strncpy(s_manifest_url, url, sizeof(s_manifest_url) - 1U);
        s_manifest_url[sizeof(s_manifest_url) - 1U] = '\0';
        xSemaphoreGive(s_services_mutex);
    }
    return err;
}

esp_err_t app_services_get_ota_manifest_url(char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    if (strlen(s_manifest_url) >= buffer_len) {
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
    if (!wifi_controller_is_connected()) {
        if (user_initiated) {
            lcd_flash_message("Wi-Fi Required", "Connect first", 1500U);
        }
        return ESP_ERR_INVALID_STATE;
    }

    (void)app_services_get_ota_manifest_url(manifest_url, sizeof(manifest_url));
    if (manifest_url[0] == '\0') {
        if (user_initiated) {
            lcd_flash_message("OTA Not Set", "Configure URL", 1500U);
        }
        return ESP_ERR_NOT_FOUND;
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    if (s_ota_status.state == APP_OTA_DOWNLOADING || s_ota_status.state == APP_OTA_CHECKING) {
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
    if (err == ESP_OK && update_available) {
        s_ota_status.state = APP_OTA_AVAILABLE;
        s_ota_status.update_available = true;
        strncpy(s_ota_status.available_version, entry.version,
                sizeof(s_ota_status.available_version) - 1U);
    } else {
        s_ota_status.state = err == ESP_OK ? APP_OTA_IDLE : APP_OTA_ERROR;
        s_ota_status.update_available = false;
        s_ota_status.available_version[0] = '\0';
    }
    xSemaphoreGive(s_services_mutex);

    if (err != ESP_OK) {
        ESP_LOGW(APP_SERVICES_TAG, "OTA check failed: %s", esp_err_to_name(err));
        if (user_initiated) {
            lcd_flash_message("OTA Check Failed", "Try again", 1500U);
        }
    } else if (update_available) {
        char line[17];
        snprintf(line, sizeof(line), "Version %.8s", entry.version);
        lcd_flash_message("Update Available", line, 1800U);
    } else if (user_initiated) {
        lcd_flash_message("Firmware Current", "No update", 1400U);
    }
    return err;
}

esp_err_t app_services_request_update_confirmation(void)
{
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool available = s_ota_status.update_available;
    const bool busy = s_ota_status.state == APP_OTA_DOWNLOADING;
    if (available && !busy) {
        s_ota_status.state = APP_OTA_CONFIRMING;
        s_ota_status.confirmation_pending = true;
    }
    xSemaphoreGive(s_services_mutex);

    if (!available || busy) {
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
    if (confirmed) {
        s_ota_status.confirmation_pending = false;
        s_ota_status.state = APP_OTA_DOWNLOADING;
        s_ota_status.progress_percent = 0;
    }
    strncpy(manifest_url, s_manifest_url, sizeof(manifest_url) - 1U);
    xSemaphoreGive(s_services_mutex);

    if (!confirmed || manifest_url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = ota_service_start_from_csv(manifest_url);
    if (err != ESP_OK) {
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
    if (pending) {
        s_ota_status.state = APP_OTA_AVAILABLE;
    }
    xSemaphoreGive(s_services_mutex);

    if (ota_service_in_progress()) {
        return ota_service_cancel();
    }
    if (pending) {
        lcd_flash_message("Update Deferred", "Current kept", 1200U);
    }
    return ESP_OK;
}

void app_services_get_ota_status(app_ota_status_t *status)
{
    if (!status) {
        return;
    }
    if (!s_services_mutex) {
        memset(status, 0, sizeof(*status));
        return;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    *status = s_ota_status;
    xSemaphoreGive(s_services_mutex);
}
