#include "storage/nvs_manager.h"
#include "app/app_services.h"
#include "app/app_menu.h"
#include "diagnostics/stack_report.h"
#include "system/task_watchdog.h"
#include "system/core_affinity.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
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

#define APP_SERVICES_TAG "APP_SERVICES"
#ifdef CONFIG_INVERTER_OTA_MANIFEST_URL
#define APP_OTA_DEFAULT_MANIFEST_URL CONFIG_INVERTER_OTA_MANIFEST_URL
#else
/* Do not assume a production release asset exists. Configure a real HTTPS
 * CSV manifest in menuconfig for deployments that enable OTA. */
#define APP_OTA_DEFAULT_MANIFEST_URL "https://github.com/Jezzylinks/inverter/releases/latest/download/inverter.csv"
#endif
#define APP_SERVICES_NVS_NAMESPACE NVS_NS_SYSTEM
#define APP_WIFI_ENABLED_KEY "wifi_enabled"
#define APP_OTA_MANIFEST_KEY "ota_manifest"
#define APP_OTA_AUTOCHECK_KEY "ota_auto"
#define APP_OTA_CHECK_INTERVAL_MS (6U * 60U * 60U * 1000U)
#define APP_OTA_APPLY_PROMPT_TIMEOUT_MS 60000U
#define APP_OTA_TASK_STACK_SIZE 4096U
#define APP_OTA_CHECK_TASK_STACK_SIZE 6144U
#define APP_OTA_CHECK_TASK_PRIORITY 5U
#define APP_WIFI_OPERATION_WATCH_STACK_SIZE 3072U
#define APP_WIFI_OPERATION_WATCH_PRIORITY 5U
/* This task's call chain runs the full Wi-Fi driver start/stop path
 * (wifi_controller_start/stop -> wifi_manager -> esp_wifi_*) and, on
 * disable, network_services_stop() (HTTP/WebSocket/mDNS/NTP/MQTT teardown),
 * plus an NVS commit to persist the enabled/disabled intent. 4096 bytes was
 * not observed to overflow in this review, but it is a tight margin for
 * that combined call depth and could not be confirmed against hardware
 * stack high-water-mark data (uxTaskGetStackHighWaterMark) in this
 * environment. Sized up defensively; verify on-device and tune down if
 * headroom data supports it. */
#define APP_WIFI_TOGGLE_TASK_STACK_SIZE 5120U
#define APP_WIFI_TOGGLE_TASK_PRIORITY 5U
#define APP_WIFI_TOGGLE_QUEUE_LENGTH 1U
#define APP_WIFI_TOGGLE_WATCHDOG_POLL_MS 500U
#define APP_WIFI_OPERATION_TIMEOUT_MS 30000U
#define APP_WIFI_PREREQUISITE_POLL_MS 500U
#define APP_WIFI_PREREQUISITE_STACK_SIZE 3072U
#define APP_WIFI_PREREQUISITE_PRIORITY 5U
#define APP_WIFI_SCAN_DURATION_MS 40000U
#define APP_WIFI_SCAN_POLL_MS 250U
#define APP_WIFI_SCAN_INTERVAL_MS 2500U
#define APP_WIFI_SCAN_TASK_STACK_SIZE 6144U
#define APP_WIFI_SCAN_TASK_PRIORITY 5U
/* One-shot post-boot saved-network connect. Runs below the normal Wi-Fi
 * workers so it can never delay a user-driven operation. */
#define APP_WIFI_BACKGROUND_CONNECT_STACK_SIZE 6144U
#define APP_WIFI_BACKGROUND_CONNECT_PRIORITY 3U

extern system_state_t sys_state;
extern SemaphoreHandle_t sys_state_mutex;

static SemaphoreHandle_t s_services_mutex;
static TaskHandle_t s_ota_check_task;
static TaskHandle_t s_ota_apply_prompt_task;
static volatile bool s_ota_prompt_from_boot_reminder;
static volatile uint32_t s_ota_prompt_last_shown_ms;
static TaskHandle_t s_ota_manifest_check_task;
static bool s_ota_manifest_check_active;
static TaskHandle_t s_pending_manifest_check_task;
static bool s_pending_manifest_check_active;
static bool s_pending_manifest_checked;
static void app_services_schedule_pending_manifest_check(void);
static TaskHandle_t s_wifi_operation_watch_task;
static TaskHandle_t s_wifi_prerequisite_task;
static TaskHandle_t s_wifi_toggle_task;
static QueueHandle_t s_wifi_toggle_queue;
/* Set to 1 while app_wifi_toggle_task is inside execute_wifi_toggle().
 * Prevents a new toggle from entering the radio stack while a previous
 * start/stop sequence is still in progress.  esp_wifi_start/stop are
 * not re-entrant; calling one while the other is running causes an
 * assert/panic in the ESP-IDF Wi-Fi stack. */
/* Atomic admission flag: 0 = free, 1 = a toggle is pending or executing.
 * Set by the caller BEFORE queuing the request; cleared by the worker AFTER
 * execute_wifi_toggle() returns.  Only one toggle can be live at any time. */
static volatile uint32_t s_wifi_toggle_admitted = 0U;
static TaskHandle_t s_wifi_scan_task;
static volatile TaskHandle_t s_wifi_background_connect_task;
static bool s_wifi_forget_pending;
static bool s_wifi_disconnect_pending;
static bool s_ota_check_cancel_requested;
static bool s_ota_cancel_confirmation_pending;
static bool s_wifi_scan_active;
static bool s_wifi_scan_cancel_requested;
static char s_manifest_url[APP_OTA_MANIFEST_URL_MAX];
static app_ota_status_t s_ota_status;

typedef enum
{
    APP_WIFI_OPERATION_NONE = 0,
    APP_WIFI_OPERATION_ENABLE,
    APP_WIFI_OPERATION_CONNECT_SAVED,
    APP_WIFI_OPERATION_DISABLE,
    APP_WIFI_OPERATION_DISCONNECT,
    APP_WIFI_OPERATION_PROVISION,
    APP_WIFI_OPERATION_MODE_CHANGE,
} app_wifi_operation_t;

static app_wifi_operation_t s_wifi_operation;
static char s_wifi_operation_ssid[WIFI_MAX_SSID_LEN + 1U];
static uint32_t s_wifi_operation_generation;
static TickType_t s_wifi_operation_started_tick;
static int8_t s_wifi_operation_rssi = -127;
static bool s_wifi_connection_uses_auth = false;

typedef struct
{
    bool enabled;
    bool previous_enabled;
    bool mode_change;
    wifi_mode_t mode;
    uint64_t requested_ms;
    bool silent; /* boot-time Auto Wi-Fi On: no LCD flashes or error popups */
} wifi_toggle_request_t;

/*
 * The user's last manual Wi-Fi choice, mirrored in NVS key "wifi_enabled":
 * ON only when the user switched Wi-Fi on, OFF otherwise (also the default
 * when nothing was ever saved). Only manual toggles change it; the automatic
 * boot activation never does. It does not start Wi-Fi by itself: the
 * "Wi-Fi Auto On" setting is the master and this choice only takes effect
 * at boot while that setting is ON (see app_wifi_boot_enable_wanted()).
 */
static bool s_wifi_user_choice_on = false;

static bool s_wifi_mode_edit_active = false;
static wifi_mode_t s_wifi_mode_edit_value = WIFI_COMPILED_OPERATION_MODE;

static esp_err_t persist_u8(const char *key, uint8_t value);
static void app_wifi_toggle_task(void *parameter);
static void app_wifi_prerequisite_task(void *parameter);
static esp_err_t app_services_execute_wifi_mode_change(wifi_mode_t mode);
static bool app_wifi_end_operation_if_generation(uint32_t generation);
static void app_wifi_schedule_menu_refresh(uint32_t delay_ms);
static void app_wifi_report_connected(const char *ssid, int8_t rssi);
static void ota_apply_prompt_timeout_task(void *parameter);

typedef enum
{
    APP_WIFI_PREREQUISITE_NONE = 0,
    APP_WIFI_PREREQUISITE_OTA_CHECK,
    APP_WIFI_PREREQUISITE_OTA_INSTALL
} app_wifi_prerequisite_operation_t;

static app_wifi_prerequisite_operation_t s_wifi_prerequisite_operation;
static bool s_wifi_prerequisite_pending;
static bool s_wifi_prerequisite_waiting;
static bool s_wifi_prerequisite_ready;
static TickType_t s_wifi_prerequisite_started_tick;

static bool app_manifest_url_is_valid(const char *url)
{
    /* URI schemes are case-insensitive (RFC 3986). Keep this policy in
     * step with ota_manifest_is_https_url(), which already accepts HTTPS://. */
    return url != NULL && url[0] != '\0' &&
           ota_manifest_is_https_url(url) &&
           strlen(url) < APP_OTA_MANIFEST_URL_MAX;
}

/* A station DHCP lease is the authoritative local-connectivity prerequisite.
 * WIFI_STATE may be AP_ACTIVE in APSTA mode even while STA has a valid IP. */
static bool app_wifi_station_has_ip(void)
{
    wifi_status_t status = {0};
    return wifi_events_get_status_copy(&status) == ESP_OK && status.got_ip;
}

/* Single live-state test for "the station is connected to the requested SSID
 * and has an IP". Used by the status callback, the watcher poll and the
 * timeout reconciliation so they cannot disagree.
 *
 *  - got_ip comes from the station event snapshot and is required, so an
 *    active AP (APSTA) alone is never mistaken for a station connection.
 *  - When the driver can return the AP record, its SSID is authoritative
 *    (a DHCP lease on a previous AP does not satisfy a new request).
 *  - When the AP record is transiently unavailable, fall back to other live
 *    state: the controller reports connected, the STA netif holds a non-zero
 *    address, and the target the driver was configured with is the requested
 *    SSID. A connection to an unrelated network cannot satisfy all three
 *    because switching networks disconnects first and rewrites the target. */
static bool app_wifi_station_connected_to(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0')
    {
        return false;
    }

    wifi_status_t status = {0};
    if (wifi_events_get_status_copy(&status) != ESP_OK || !status.got_ip)
    {
        return false;
    }

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        return strncmp((const char *)ap.ssid, ssid, sizeof(ap.ssid)) == 0;
    }

    if (!wifi_controller_is_connected())
    {
        return false;
    }
    esp_netif_ip_info_t ip = {0};
    if (wifi_manager_get_ip(&ip) != ESP_OK || ip.ip.addr == 0U)
    {
        return false;
    }
    wifi_manager_config_t target = {0};
    if (wifi_manager_get_config(&target) != ESP_OK)
    {
        return false;
    }
    return strcmp(target.ssid, ssid) == 0;
}

static bool app_wifi_scan_cancel_requested(void)
{
    bool cancel = true;
    if (s_services_mutex != NULL)
    {
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
           (xTaskGetTickCount() - started) < duration)
    {
        uint16_t count = WIFI_MAX_SCAN_RESULTS;
        const esp_err_t err = wifi_scan_start_records(records, &count);
        last_scan_error = err;
        if (err == ESP_OK)
        {
            const uint8_t visible = count > LCD_WIFI_MAX_AP ? LCD_WIFI_MAX_AP : (uint8_t)count;
            last_count = visible;
            had_successful_scan = true;
            memset(ssids, 0, sizeof(ssids));
            memset(rssi, 0, sizeof(rssi));
            for (uint8_t i = 0U; i < visible; ++i)
            {
                strncpy(ssids[i], (const char *)records[i].ssid, LCD_WIFI_SSID_MAX_LEN);
                ssids[i][LCD_WIFI_SSID_MAX_LEN] = '\0';
                rssi[i] = records[i].rssi;
                channel[i] = records[i].primary;
                authmode[i] = (uint8_t)records[i].authmode;
            }
            lcd_update_wifi_scan_results(visible, ssids, rssi, channel, authmode, spinner++);
        }
        else if (!app_wifi_scan_cancel_requested())
        {
            lcd_update_wifi_scan_spinner(spinner++);
        }

        const TickType_t wait_until = xTaskGetTickCount() +
                                      pdMS_TO_TICKS(APP_WIFI_SCAN_INTERVAL_MS);
        while (!app_wifi_scan_cancel_requested() &&
               (int32_t)(wait_until - xTaskGetTickCount()) > 0 &&
               (xTaskGetTickCount() - started) < duration)
        {
            vTaskDelay(pdMS_TO_TICKS(APP_WIFI_SCAN_POLL_MS));
        }
    }

    bool keep_scan_screen = false;
    LCD_LOCK();
    keep_scan_screen = sys_lcd.screen == LCD_SCREEN_WIFI_SCAN;
    LCD_UNLOCK();
    if (keep_scan_screen)
    {
        if (!had_successful_scan && !app_wifi_scan_cancel_requested() &&
            last_scan_error != ESP_OK)
        {
            lcd_show_wifi_scan_failed();
        }
        else
        {
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

static void app_wifi_prerequisite_task(void *parameter)
{
    (void)parameter;

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(APP_WIFI_PREREQUISITE_POLL_MS));

        app_wifi_prerequisite_operation_t operation = APP_WIFI_PREREQUISITE_NONE;
        bool waiting = false;

        if (s_services_mutex != NULL)
        {
            xSemaphoreTake(s_services_mutex, portMAX_DELAY);
            operation = s_wifi_prerequisite_operation;
            waiting = s_wifi_prerequisite_waiting;
            xSemaphoreGive(s_services_mutex);
        }
        (void)task_watchdog_health_feed();

        if (!waiting || operation == APP_WIFI_PREREQUISITE_NONE)
        {
            continue;
        }

        if ((xTaskGetTickCount() - s_wifi_prerequisite_started_tick) >=
            pdMS_TO_TICKS(APP_WIFI_OPERATION_TIMEOUT_MS))
        {
            xSemaphoreTake(s_services_mutex, portMAX_DELAY);
            s_wifi_prerequisite_operation = APP_WIFI_PREREQUISITE_NONE;
            s_wifi_prerequisite_pending = false;
            s_wifi_prerequisite_waiting = false;
            s_wifi_prerequisite_ready = false;
            xSemaphoreGive(s_services_mutex);

            ESP_LOGW(APP_SERVICES_TAG,
                     "Wi-Fi prerequisite timed out after %ums",
                     (unsigned)APP_WIFI_OPERATION_TIMEOUT_MS);
            lcd_flash_message("Wi-Fi Unavailable", "Try again", 1500U);
            show_menu_screen(MENU_OTA, operation == APP_WIFI_PREREQUISITE_OTA_INSTALL ? 1 : 0);
            continue;
        }

        if (!app_wifi_station_has_ip() || !wifi_monitor_is_online())
        {
            continue;
        }

        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        if (s_wifi_prerequisite_operation == operation &&
            s_wifi_prerequisite_waiting)
        {
            s_wifi_prerequisite_waiting = false;
            s_wifi_prerequisite_ready = true;
        }
        else
        {
            operation = APP_WIFI_PREREQUISITE_NONE;
        }
        xSemaphoreGive(s_services_mutex);

        if (operation == APP_WIFI_PREREQUISITE_NONE)
        {
            continue;
        }

        ESP_LOGI(APP_SERVICES_TAG,
                 "Wi-Fi prerequisite ready for operation=%d", (int)operation);

        if (operation == APP_WIFI_PREREQUISITE_OTA_CHECK)
        {
            xSemaphoreTake(s_services_mutex, portMAX_DELAY);
            s_wifi_prerequisite_ready = false;
            s_wifi_prerequisite_operation = APP_WIFI_PREREQUISITE_NONE;
            xSemaphoreGive(s_services_mutex);

            (void)app_services_check_for_update(true);
        }
        else
        {
            /*
             * Keep the existing OTA authentication boundary in app_input.c.
             * Return to the OTA menu with Install selected; ENTER resumes the
             * normal PIN/confirmation path now that networking is ready.
             */
            xSemaphoreTake(s_services_mutex, portMAX_DELAY);
            s_wifi_prerequisite_ready = true;
            s_wifi_prerequisite_operation = APP_WIFI_PREREQUISITE_NONE;
            xSemaphoreGive(s_services_mutex);

            show_menu_screen(MENU_OTA, 1);
        }
    }
}

static bool app_wifi_ui_animation_active(void);
static void app_wifi_sync_menu_if_visible(void);

/* Single authoritative UI view of the Wi-Fi action label. It is derived from
 * the live operation/controller/event/driver state in app_wifi_ui_state();
 * nothing else decides what the Connect/Disconnect item shows. */
typedef enum
{
    APP_WIFI_UI_CONNECT = 0,
    APP_WIFI_UI_CONNECTING,
    APP_WIFI_UI_DISCONNECT,
    APP_WIFI_UI_DISCONNECTING,
} app_wifi_ui_state_t;

#define APP_WIFI_ANIM_STEP_MS 400U      /* one dot phase                  */
#define APP_WIFI_ANIM_PHASES 3U         /* ".", "..", "..."               */
#define APP_WIFI_DISCONNECT_MIN_MS 900U /* keep "Disconnecting" visible   */

/* Animation phase owned by the watcher task; read by label renderers. */
static uint8_t s_wifi_anim_phase;

static app_wifi_ui_state_t app_wifi_ui_state(void);
static bool app_wifi_menu_snapshot_stale(const char *want);
static void app_wifi_notify_watcher(void);
static esp_err_t app_wifi_ensure_watcher(void);

static void app_wifi_operation_watch_task(void *parameter)
{
    (void)parameter;

    char last_connect_label[24] = {0};
    bool last_wifi_enabled = false;
    bool have_menu_snapshot = false;
    TickType_t last_rssi_poll = 0U;
    TickType_t last_phase_tick = xTaskGetTickCount();
    uint8_t last_rendered_anim_phase = UINT8_MAX;
    app_wifi_ui_state_t last_ui_state = APP_WIFI_UI_CONNECT;

    while (true)
    {
        /*
         * The menu label also animates during controller-driven auto-reconnect,
         * when s_wifi_operation is NONE. Polling only once per second in that
         * case skips animation phases and makes dots appear to jump whenever
         * a button causes the menu to be redrawn. Poll at 100 ms while either
         * the explicit operation or the live station state is transitioning.
         */
        /* Wake early when an operation starts/ends; otherwise the timeout is
         * only the animation/reconciliation cadence. */
        (void)ulTaskNotifyTake(pdTRUE,
                               pdMS_TO_TICKS(app_wifi_ui_animation_active() ? 100U : 250U));

        /* Poll the live station/DHCP handshake while an operation is active.
         * Do not depend exclusively on an event callback to finish the
         * Connecting/Disconnecting state. Generation checks arbitrate with a
         * callback that may complete concurrently. */
        app_wifi_operation_t polled_operation = APP_WIFI_OPERATION_NONE;
        uint32_t polled_generation = 0U;
        char polled_ssid[sizeof(s_wifi_operation_ssid)] = {0};
        int8_t polled_rssi = -127;
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        polled_operation = s_wifi_operation;
        polled_generation = s_wifi_operation_generation;
        polled_rssi = s_wifi_operation_rssi;
        strncpy(polled_ssid, s_wifi_operation_ssid, sizeof(polled_ssid) - 1U);
        xSemaphoreGive(s_services_mutex);

        if (polled_operation == APP_WIFI_OPERATION_CONNECT_SAVED &&
            polled_ssid[0] != '\0')
        {
            if (app_wifi_station_connected_to(polled_ssid) &&
                app_wifi_end_operation_if_generation(polled_generation))
            {
                ESP_LOGI(APP_SERVICES_TAG,
                         "Live Wi-Fi poll confirmed connection to '%s'",
                         polled_ssid);
                app_wifi_report_connected(polled_ssid, polled_rssi);
            }
        }
        else if (polled_operation == APP_WIFI_OPERATION_DISCONNECT)
        {
            wifi_ap_record_t live_ap = {0};
            /* The driver's station association is authoritative here.
             * In APSTA the AP side may remain active, and the event snapshot
             * can lag the station disconnect by a tick. */
            const bool station_gone =
                esp_wifi_sta_get_ap_info(&live_ap) != ESP_OK;
            if (station_gone &&
                app_wifi_end_operation_if_generation(polled_generation))
            {
                ESP_LOGI(APP_SERVICES_TAG,
                         "Live Wi-Fi poll confirmed station disconnection");
                app_wifi_sync_menu_if_visible();
            }
        }

        bool timed_out = false;
        app_wifi_operation_t timed_out_operation = APP_WIFI_OPERATION_NONE;
        char timed_out_ssid[sizeof(s_wifi_operation_ssid)] = {0};
        int8_t timed_out_rssi = -127;
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        if (s_wifi_operation != APP_WIFI_OPERATION_NONE &&
            (uint32_t)(xTaskGetTickCount() - s_wifi_operation_started_tick) >=
                pdMS_TO_TICKS(APP_WIFI_OPERATION_TIMEOUT_MS))
        {
            timed_out_operation = s_wifi_operation;
            timed_out_rssi = s_wifi_operation_rssi;
            strncpy(timed_out_ssid, s_wifi_operation_ssid,
                    sizeof(timed_out_ssid) - 1U);
            s_wifi_operation = APP_WIFI_OPERATION_NONE;
            s_wifi_operation_ssid[0] = '\0';
            s_wifi_operation_rssi = -127;
            ++s_wifi_operation_generation;
            timed_out = true;
        }
        xSemaphoreGive(s_services_mutex);

        if (timed_out)
        {
            /* A delayed/missed monitor callback must not report failure if
             * the live station state already proves completion. */
            bool connection_really_succeeded = false;
            bool disconnection_really_succeeded = false;
            if (timed_out_operation == APP_WIFI_OPERATION_CONNECT_SAVED)
            {
                connection_really_succeeded =
                    app_wifi_station_connected_to(timed_out_ssid);
            }
            else if (timed_out_operation == APP_WIFI_OPERATION_DISCONNECT)
            {
                wifi_ap_record_t active_ap = {0};
                disconnection_really_succeeded =
                    esp_wifi_sta_get_ap_info(&active_ap) != ESP_OK;
            }

            if (connection_really_succeeded)
            {
                ESP_LOGI(APP_SERVICES_TAG,
                         "Wi-Fi callback was delayed; confirmed connected to '%s'",
                         timed_out_ssid);
                app_wifi_report_connected(timed_out_ssid, timed_out_rssi);
            }
            else if (disconnection_really_succeeded)
            {
                ESP_LOGI(APP_SERVICES_TAG,
                         "Wi-Fi callback was delayed; station is disconnected");
                app_wifi_sync_menu_if_visible();
            }
            else
            {
                ESP_LOGW(APP_SERVICES_TAG, "Wi-Fi operation timed out after %ums",
                         (unsigned)APP_WIFI_OPERATION_TIMEOUT_MS);
                lcd_flash_warning_to("Connection Timeout",
                                     timed_out_ssid[0] ? timed_out_ssid : "Try again",
                                     1500U, LCD_SCREEN_MENU);
                app_wifi_schedule_menu_refresh(1700U);
            }
        }

        /* Advance the animation phase from real state: restart only when the
         * UI state changes, step every APP_WIFI_ANIM_STEP_MS while animating. */
        const app_wifi_ui_state_t ui_state = app_wifi_ui_state();
        const bool animating = ui_state == APP_WIFI_UI_CONNECTING ||
                               ui_state == APP_WIFI_UI_DISCONNECTING;
        const TickType_t phase_now = xTaskGetTickCount();
        if (ui_state != last_ui_state)
        {
            last_ui_state = ui_state;
            last_phase_tick = phase_now;
            __atomic_store_n(&s_wifi_anim_phase, 0U, __ATOMIC_RELAXED);
        }
        else if (animating &&
                 (TickType_t)(phase_now - last_phase_tick) >=
                     pdMS_TO_TICKS(APP_WIFI_ANIM_STEP_MS))
        {
            last_phase_tick = phase_now;
            const uint8_t phase =
                __atomic_load_n(&s_wifi_anim_phase, __ATOMIC_RELAXED);
            __atomic_store_n(&s_wifi_anim_phase,
                             (uint8_t)((phase + 1U) % APP_WIFI_ANIM_PHASES),
                             __ATOMIC_RELAXED);
        }

        lcd_screen_id_t screen;
        LCD_LOCK();
        screen = sys_lcd.screen;
        LCD_UNLOCK();

        menu_state_t menu_state;
        uint8_t selection;
        if (sys_state_mutex != NULL)
        {
            xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
        }
        menu_state = sys_state.menu_state;
        selection = sys_state.menu_selection;
        if (sys_state_mutex != NULL)
        {
            xSemaphoreGive(sys_state_mutex);
        }

        /*
         * Only redraw while the menu itself owns the LCD. Full-screen Wi-Fi
         * result/connecting screens and flash messages own the display until
         * they expire; overwriting them here was a competing writer. When the
         * LCD returns to LCD_SCREEN_MENU its snapshot may be stale, so compare
         * the snapshot itself (not a private cached string) with the label
         * that should be shown.
         */
        if (screen == LCD_SCREEN_MENU && menu_state == MENU_WIFI_CONFIG)
        {
            const char *connect_label = app_services_wifi_connect_action_label();
            const bool wifi_enabled = app_services_wifi_enabled();
            const uint8_t anim_phase =
                __atomic_load_n(&s_wifi_anim_phase, __ATOMIC_RELAXED);
            /* Do not rely on text diffing the LCD render buffer to drive
             * animation. Some LCD writers update a shadow/menu snapshot
             * asynchronously; a phase transition must explicitly request a
             * redraw while the Wi-Fi menu owns the display. */
            if (!have_menu_snapshot ||
                strcmp(last_connect_label, connect_label) != 0 ||
                last_wifi_enabled != wifi_enabled ||
                (animating && last_rendered_anim_phase != anim_phase) ||
                app_wifi_menu_snapshot_stale(connect_label))
            {
                strncpy(last_connect_label, connect_label,
                        sizeof(last_connect_label) - 1U);
                last_connect_label[sizeof(last_connect_label) - 1U] = '\0';
                last_wifi_enabled = wifi_enabled;
                last_rendered_anim_phase = anim_phase;
                have_menu_snapshot = true;
                show_menu_screen(MENU_WIFI_CONFIG, selection);
            }
        }
        else
        {
            have_menu_snapshot = false;
        }

        /* RSSI can change without a state transition; keep home telemetry live. */
        const TickType_t now = xTaskGetTickCount();
        if (last_rssi_poll == 0U ||
            (TickType_t)(now - last_rssi_poll) >= pdMS_TO_TICKS(5000U))
        {
            wifi_status_t status = {0};
            const bool connected =
                wifi_events_get_status_copy(&status) == ESP_OK &&
                status.got_ip;
            int8_t rssi = 0;
            if (connected)
            {
                rssi = wifi_manager_get_rssi();
                if (rssi == -127)
                {
                    rssi = wifi_monitor_get_rssi();
                }
            }
            lcd_update_wifi_status(connected, connected ? rssi : 0);
            last_rssi_poll = now;
        }
    }
}

static void app_wifi_begin_operation(app_wifi_operation_t operation,
                                     const char *ssid, int8_t rssi)
{
    if (s_services_mutex == NULL)
    {
        return;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_operation = operation;
    ++s_wifi_operation_generation;
    s_wifi_operation_started_tick = xTaskGetTickCount();
    s_wifi_operation_rssi = rssi;
    s_wifi_operation_ssid[0] = '\0';
    if (ssid != NULL)
    {
        strncpy(s_wifi_operation_ssid, ssid, sizeof(s_wifi_operation_ssid) - 1U);
        s_wifi_operation_ssid[sizeof(s_wifi_operation_ssid) - 1U] = '\0';
    }
    xSemaphoreGive(s_services_mutex);
    app_wifi_notify_watcher();
}

static void app_wifi_end_operation(void)
{
    if (s_services_mutex == NULL)
    {
        return;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_operation = APP_WIFI_OPERATION_NONE;
    s_wifi_operation_ssid[0] = '\0';
    s_wifi_operation_rssi = -127;
    xSemaphoreGive(s_services_mutex);
    app_wifi_notify_watcher();
}

/* End a monitor-reported operation only if it is still the operation whose
 * status snapshot was evaluated. This prevents a delayed callback from
 * clearing a newer user request. */
static bool app_wifi_end_operation_if_generation(uint32_t generation)
{
    if (s_services_mutex == NULL)
    {
        return false;
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    bool matches = (s_wifi_operation != APP_WIFI_OPERATION_NONE) &&
                   (s_wifi_operation_generation == generation);
    /* Keep "Disconnecting" on screen long enough to be seen. The watcher
     * keeps polling the live station state and ends it afterwards. */
    if (matches && s_wifi_operation == APP_WIFI_OPERATION_DISCONNECT &&
        (uint32_t)(xTaskGetTickCount() - s_wifi_operation_started_tick) <
            pdMS_TO_TICKS(APP_WIFI_DISCONNECT_MIN_MS))
    {
        matches = false;
    }
    if (matches)
    {
        s_wifi_operation = APP_WIFI_OPERATION_NONE;
        s_wifi_operation_ssid[0] = '\0';
        s_wifi_operation_rssi = -127;
    }
    xSemaphoreGive(s_services_mutex);
    if (matches)
    {
        app_wifi_notify_watcher();
    }
    return matches;
}

static app_wifi_ui_state_t app_wifi_ui_state(void)
{
    app_wifi_operation_t operation = APP_WIFI_OPERATION_NONE;
    if (s_services_mutex != NULL)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        operation = s_wifi_operation;
        xSemaphoreGive(s_services_mutex);
    }
    if (operation == APP_WIFI_OPERATION_DISCONNECT)
    {
        return APP_WIFI_UI_DISCONNECTING;
    }
    if (operation == APP_WIFI_OPERATION_CONNECT_SAVED)
    {
        return APP_WIFI_UI_CONNECTING;
    }

    wifi_status_t status = {0};
    const bool have_status = wifi_events_get_status_copy(&status) == ESP_OK;
    const wifi_controller_state_t controller_state = wifi_controller_get_state();
    wifi_ap_record_t active_ap = {0};
    const bool station_associated =
        esp_wifi_sta_get_ap_info(&active_ap) == ESP_OK;
    const bool got_ip = have_status && status.got_ip;

    /* Connected requires IP + driver association + controller agreement. */
    if (got_ip && station_associated && wifi_controller_is_connected())
    {
        return APP_WIFI_UI_DISCONNECT;
    }
    /* In progress: controller/event transition, or associated but DHCP not
     * finished. Association alone with an IP is not "connecting". */
    if (controller_state == WIFI_CONTROLLER_CONNECTING ||
        controller_state == WIFI_CONTROLLER_STARTING ||
        (have_status && (status.state == WIFI_STATE_CONNECTING ||
                         status.state == WIFI_STATE_RECONNECTING)) ||
        (station_associated && !got_ip))
    {
        return APP_WIFI_UI_CONNECTING;
    }
    return APP_WIFI_UI_CONNECT;
}

static bool app_wifi_ui_animation_active(void)
{
    const app_wifi_ui_state_t state = app_wifi_ui_state();
    return state == APP_WIFI_UI_CONNECTING ||
           state == APP_WIFI_UI_DISCONNECTING;
}

/* True when the menu snapshot shows a Connect-family label that differs
 * from `want`. If no such label is on a visible row (item scrolled out of
 * view) there is nothing to correct. Tokens are matched exactly so
 * "Connecting." is not mistaken for "Connecting..". */
static bool app_wifi_menu_snapshot_stale(const char *want)
{
    static const char *const labels[] = {
        "Connect",
        "Disconnect",
        "Connecting.",
        "Connecting..",
        "Connecting...",
        "Disconnecting.",
        "Disconnecting..",
        "Disconnecting...",
    };
    const char *found = NULL;

    LCD_LOCK();
    for (uint8_t row = 0U; row < LCD_ROWS && found == NULL; ++row)
    {
        const char *text = sys_lcd.menu.rows[row];
        for (size_t i = 0U; i < sizeof(labels) / sizeof(labels[0]); ++i)
        {
            const size_t len = strlen(labels[i]);
            for (const char *at = strstr(text, labels[i]); at != NULL;
                 at = strstr(at + 1, labels[i]))
            {
                const char before = (at == text) ? ' ' : at[-1];
                const char after = at[len];
                if (!isalnum((unsigned char)before) &&
                    !isalnum((unsigned char)after) && after != '.')
                {
                    found = labels[i];
                    break;
                }
            }
            if (found != NULL)
            {
                break;
            }
        }
    }
    const bool stale = (found != NULL) && (strcmp(found, want) != 0);
    LCD_UNLOCK();
    return stale;
}

static void app_wifi_notify_watcher(void)
{
    if (s_wifi_operation_watch_task != NULL)
    {
        xTaskNotifyGive(s_wifi_operation_watch_task);
    }
}

static void app_wifi_sync_menu_if_visible(void)
{
    menu_state_t menu_state;
    uint8_t selection;
    bool visible;

    LCD_LOCK();
    /* LCD_SCREEN_WIFI_CONNECTING owns the display (connect/result view) and
     * returns to the menu itself; the watcher repairs the stale snapshot then. */
    visible = (sys_lcd.screen == LCD_SCREEN_MENU) ||
              (sys_lcd.screen == LCD_SCREEN_FLASH_MSG &&
               lcd_flash_is_active());
    LCD_UNLOCK();
    if (!visible)
    {
        return;
    }

    if (sys_state_mutex != NULL)
    {
        xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    }
    menu_state = sys_state.menu_state;
    selection = sys_state.menu_selection;
    if (sys_state_mutex != NULL)
    {
        xSemaphoreGive(sys_state_mutex);
    }

    if (menu_state == MENU_WIFI_CONFIG)
    {
        show_menu_screen(menu_state, selection);
        ESP_LOGI(APP_SERVICES_TAG, "Wi-Fi menu synchronized from runtime state");
    }
}

/* Connection result messages use a timed LCD overlay. Refresh after that
 * overlay has expired so the underlying Connect/Disconnect label is current. */
static void app_wifi_menu_refresh_task(void *parameter)
{
    const uint32_t delay_ms = (uint32_t)(uintptr_t)parameter;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    /* A queued LCD flash can last slightly longer than its nominal duration.
     * Never redraw the menu over an active flash/result screen. */
    for (unsigned attempt = 0U; attempt < 12U; ++attempt)
    {
        lcd_screen_id_t screen;
        LCD_LOCK();
        screen = sys_lcd.screen;
        LCD_UNLOCK();

        if (screen == LCD_SCREEN_MENU)
        {
            app_wifi_sync_menu_if_visible();
            break;
        }
        if (screen == LCD_SCREEN_WIFI_CONNECTING)
        {
            /* A confirmed connection leaves the connect screen on its own.
             * Failure/timeout results stay up until the user presses Back. */
            bool connected_result;
            LCD_LOCK();
            connected_result = sys_lcd.wifi_connect.connected;
            LCD_UNLOCK();
            if (connected_result)
            {
                menu_state_t menu_state;
                uint8_t selection;
                if (sys_state_mutex != NULL)
                {
                    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
                }
                menu_state = sys_state.menu_state;
                selection = sys_state.menu_selection;
                if (sys_state_mutex != NULL)
                {
                    xSemaphoreGive(sys_state_mutex);
                }
                if (menu_state == MENU_WIFI_CONFIG)
                {
                    show_menu_screen(MENU_WIFI_CONFIG, selection);
                }
                break;
            }
        }
        if (screen != LCD_SCREEN_FLASH_MSG &&
            screen != LCD_SCREEN_WIFI_CONNECTING)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250U));
    }
    vTaskDelete(NULL);
}

#define APP_WIFI_CONNECTED_RESULT_MS 1500U

/* Report a confirmed station connection. The scan/password flow shows a
 * full-screen "connecting" view that app_wifi_sync_menu_if_visible() never
 * redraws, so without this the LCD stayed on CONNECTING after a successful
 * association. Convert that view to the success result and return to the
 * Wi-Fi menu. Menu-driven reconnects (no full-screen view) just re-sync. */
static void app_wifi_report_connected(const char *ssid, int8_t rssi)
{
    bool newer_operation = false;
    if (s_services_mutex != NULL)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        newer_operation = s_wifi_operation != APP_WIFI_OPERATION_NONE;
        xSemaphoreGive(s_services_mutex);
    }
    if (newer_operation)
    {
        /* A newer request owns the display; a late result must not
         * overwrite its connecting view. */
        return;
    }

    bool owns_connect_view;
    LCD_LOCK();
    owns_connect_view = sys_lcd.screen == LCD_SCREEN_WIFI_CONNECTING &&
                        !sys_lcd.wifi_connect.connected &&
                        !sys_lcd.wifi_connect.failed &&
                        !sys_lcd.wifi_connect.timed_out &&
                        /* The LCD copy is truncated to LCD_WIFI_SSID_MAX_LEN. */
                        strncmp(sys_lcd.wifi_connect.ssid,
                                ssid != NULL ? ssid : "",
                                LCD_WIFI_SSID_MAX_LEN) == 0;
    LCD_UNLOCK();

    if (!owns_connect_view)
    {
        app_wifi_sync_menu_if_visible();
        return;
    }

    const int8_t live_rssi = wifi_manager_get_rssi();
    if (live_rssi != -127 && live_rssi != 0)
    {
        rssi = live_rssi;
    }
    lcd_show_wifi_result(true, false, false, ssid, rssi, "LINK ESTABLISHED");
    app_wifi_schedule_menu_refresh(APP_WIFI_CONNECTED_RESULT_MS);
}

static void app_wifi_schedule_menu_refresh(uint32_t delay_ms)
{
    TaskHandle_t task = NULL;
    if (xTaskCreatePinnedToCore(app_wifi_menu_refresh_task, "wifi_menu_refresh",
                                2048U, (void *)(uintptr_t)delay_ms,
                                APP_WIFI_OPERATION_WATCH_PRIORITY,
                                &task, APP_CORE_SYSTEM) != pdPASS)
    {
        ESP_LOGW(APP_SERVICES_TAG, "Could not schedule Wi-Fi menu refresh");
    }
}

static esp_err_t app_services_execute_wifi_toggle(bool enabled,
                                                  bool previous_enabled,
                                                  bool silent)
{
    const uint64_t started_ms = (uint64_t)(esp_timer_get_time() / 1000LL);

    if (!enabled)
    {
        /* Service shutdown can include MQTT, mDNS, HTTP, and WebSocket
         * teardown. Keep it off the button and panel-input task. */
        (void)network_services_stop();
    }
    else
    {
        /* Clear the teardown flag so network_services_start() (called from
         * network_services_sync_task when Wi-Fi connects and gets an IP)
         * is allowed to run again after a previous disable cycle set it. */
        (void)network_services_allow_start();
    }

    ESP_LOGI(APP_SERVICES_TAG, "Wi-Fi toggle worker calling %s",
             enabled ? "wifi_controller_start()" : "wifi_controller_stop()");
    esp_err_t controller_err = enabled ? wifi_controller_start()
                                       : wifi_controller_stop();
    ESP_LOGI(APP_SERVICES_TAG, "Wi-Fi controller %s result: %s",
             enabled ? "start" : "stop", esp_err_to_name(controller_err));
    if (!enabled && (controller_err == ESP_ERR_INVALID_STATE ||
                     controller_err == ESP_ERR_WIFI_NOT_INIT ||
                     controller_err == ESP_ERR_WIFI_NOT_STARTED))
    {
        /* OFF is idempotent when the radio is already stopped.  This
         * normalization is deliberately limited to OFF: no non-OK start
         * result is treated as a successful ON transition. */
        controller_err = ESP_OK;
    }

    esp_err_t nvs_err = ESP_OK;
    if (controller_err == ESP_OK)
    {
        /* The controller result is the sole authority for the actual radio
         * transition.  Event callbacks only report status and never commit
         * this user setting. */
        sys_state.wifi.enabled = enabled;
        sys_state.inverter.wifi_enabled = enabled;
        lcd_update_wifi_enabled(enabled);
        ESP_LOGI(APP_SERVICES_TAG, "Committing Wi-Fi runtime state: %s",
                 enabled ? "ON" : "OFF");
        if (!silent)
        {
            /* A manual toggle is the user's choice: remember it (ON when the
             * user turned Wi-Fi on, OFF otherwise). The silent boot
             * activation is derived from that choice, so it must not rewrite
             * it. */
            nvs_err = persist_u8(APP_WIFI_ENABLED_KEY, enabled ? 1U : 0U);
            if (nvs_err == ESP_OK)
            {
                /* Read the key back through the same path the boot loader
                 * uses: "commit OK" alone does not prove boot will see it. */
                uint8_t readback = 0xFFU;
                nvs_handle_t rb_handle;
                esp_err_t rb_err = storage_nvs_open(APP_SERVICES_NVS_NAMESPACE,
                                                    NVS_READONLY, &rb_handle);
                if (rb_err == ESP_OK)
                {
                    rb_err = nvs_get_u8(rb_handle, APP_WIFI_ENABLED_KEY, &readback);
                    storage_nvs_close(rb_handle);
                }
                if (rb_err != ESP_OK || readback != (enabled ? 1U : 0U))
                {
                    ESP_LOGE(APP_SERVICES_TAG,
                             "Wi-Fi NVS read-back MISMATCH: wrote=%u read=%u err=%s",
                             enabled ? 1U : 0U, (unsigned)readback,
                             esp_err_to_name(rb_err));
                    nvs_err = (rb_err != ESP_OK) ? rb_err : ESP_ERR_INVALID_STATE;
                }
                else
                {
                    /* Record the choice only once it is verifiably stored. */
                    s_wifi_user_choice_on = enabled;
                }
            }
            if (nvs_err != ESP_OK)
            {
                ESP_LOGE(APP_SERVICES_TAG,
                         "Wi-Fi NVS persistence failed (%s); runtime state remains actual",
                         esp_err_to_name(nvs_err));
            }
            else
            {
                ESP_LOGI(APP_SERVICES_TAG,
                         "Wi-Fi user choice saved: %s=%u",
                         APP_WIFI_ENABLED_KEY, enabled ? 1U : 0U);
            }
        }
    }

    if (controller_err == ESP_OK)
    {
        app_wifi_end_operation();
        app_wifi_sync_menu_if_visible();
        /* Successful ON/OFF transitions are reflected by the live menu and
         * home-screen status; do not replace them with a redundant success
         * flash. Persistence failures remain visible to the user. */
        if (nvs_err != ESP_OK)
        {
            lcd_flash_warning_to(enabled ? "Wi-Fi ON" : "Wi-Fi OFF",
                                 "Save failed", 1500U, LCD_SCREEN_MENU);
        }
    }
    else
    {
        sys_state.wifi.enabled = previous_enabled;
        sys_state.inverter.wifi_enabled = previous_enabled;
        lcd_update_wifi_enabled(previous_enabled);
        ESP_LOGE(APP_SERVICES_TAG, "Wi-Fi %s failed: %s",
                 enabled ? "ON" : "OFF", esp_err_to_name(controller_err));
        app_wifi_end_operation();
        app_wifi_sync_menu_if_visible();
        if (silent)
        {
            /* Automatic boot activation: no saved SSID or a failed start is
             * logged, never shown. Manual ON/OFF still reports to the user. */
            ESP_LOGW(APP_SERVICES_TAG,
                     "Auto Wi-Fi On did not complete (%s); staying OFF silently",
                     esp_err_to_name(controller_err));
        }
        else if (enabled && controller_err == ESP_ERR_NOT_FOUND)
        {
            lcd_flash_message("Wi-Fi Not Config", "Use menuconfig", 1800U);
        }
        else
        {
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

    wifi_toggle_request_t request;
    while (true)
    {
        if (xQueueReceive(s_wifi_toggle_queue, &request,
                          pdMS_TO_TICKS(APP_WIFI_TOGGLE_WATCHDOG_POLL_MS)) != pdTRUE)
        {
            continue;
        }

        const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        if (request.mode_change)
        {
            ESP_LOGI(APP_SERVICES_TAG,
                     "Wi-Fi mode worker dispatch after %llums",
                     (unsigned long long)(now_ms >= request.requested_ms
                                              ? now_ms - request.requested_ms
                                              : 0U));
            const esp_err_t mode_err =
                app_services_execute_wifi_mode_change(request.mode);
            if (mode_err != ESP_OK)
            {
                ESP_LOGE(APP_SERVICES_TAG, "Wi-Fi mode change failed: %s",
                         esp_err_to_name(mode_err));
                lcd_flash_message("Mode Change Failed", "Previous kept", 1400U);
            }
            else
            {
                show_menu_screen(MENU_WIFI_SETTINGS, sys_state.menu_selection);
            }
            __atomic_store_n(&s_wifi_toggle_admitted, 0U, __ATOMIC_SEQ_CST);
            continue;
        }

        ESP_LOGI(APP_SERVICES_TAG,
                 "Wi-Fi %s worker dispatch after %llums",
                 request.enabled ? "ON" : "OFF",
                 (unsigned long long)(now_ms >= request.requested_ms
                                          ? now_ms - request.requested_ms
                                          : 0U));
        /* s_wifi_toggle_admitted was set by the caller; it remains set
         * throughout execution and is released here -- after the complete
         * start/stop sequence -- so no new toggle can enter while the
         * radio driver is running. */
        ESP_LOGI(APP_SERVICES_TAG, "Wi-Fi toggle worker started: %s",
                 request.enabled ? "ON" : "OFF");
        (void)app_services_execute_wifi_toggle(request.enabled,
                                               request.previous_enabled,
                                               request.silent);
        ESP_LOGI(APP_SERVICES_TAG, "Wi-Fi toggle worker completed: %s",
                 request.enabled ? "ON" : "OFF");
        __atomic_store_n(&s_wifi_toggle_admitted, 0U, __ATOMIC_SEQ_CST);
        ESP_LOGI(APP_SERVICES_TAG, "Wi-Fi toggle admission released");

        /* Stack high-water telemetry: log remaining headroom so that any
         * future call-chain growth that risks overflow is caught in logs
         * before it becomes a crash.  A value below ~512 words is a warning. */
        const UBaseType_t stack_words = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(APP_SERVICES_TAG,
                 "wifi_toggle stack high-water: %u words remaining (task stack=%u)",
                 (unsigned)stack_words,
                 (unsigned)(APP_WIFI_TOGGLE_TASK_STACK_SIZE / sizeof(StackType_t)));
        if (stack_words < 128U)
        {
            ESP_LOGE(APP_SERVICES_TAG,
                     "wifi_toggle stack critically low (%u words) — "
                     "increase APP_WIFI_TOGGLE_TASK_STACK_SIZE",
                     (unsigned)stack_words);
        }
    }
}

static esp_err_t app_services_execute_wifi_mode_change(wifi_mode_t mode)
{
    if (mode < WIFI_MODE_STA || mode > WIFI_MODE_APSTA)
    {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_manager_config_t old_config = {0};
    esp_err_t err = wifi_manager_get_config(&old_config);
    if (err != ESP_OK)
    {
        return err;
    }

    if (old_config.mode == mode)
    {
        return ESP_OK;
    }

    const bool was_enabled = sys_state.wifi.enabled;

    if (was_enabled)
    {
        (void)network_services_stop();
        err = wifi_controller_stop();
        if (err != ESP_OK)
        {
            return err;
        }
        /* network_services_stop() latches teardown. Clear that latch before
         * starting the newly selected architecture so AP/STA events can
         * legitimately bring the correct service set back online. */
        (void)network_services_allow_start();
    }

    wifi_manager_config_t new_config = old_config;
    new_config.mode = mode;

    err = wifi_manager_set_config(&new_config);
    if (err != ESP_OK)
    {
        if (was_enabled)
        {
            (void)wifi_controller_start();
        }
        return err;
    }

    wifi_network_config_t stored = {0};
    if (wifi_storage_load_network_config(&stored) != ESP_OK)
    {
        wifi_storage_set_default_network_config(&stored);
    }
    const wifi_mode_t previous_mode = stored.mode;
    stored.mode = mode;

    err = wifi_storage_save_network_config(&stored);
    if (err != ESP_OK)
    {
        (void)wifi_manager_set_config(&old_config);
        if (was_enabled)
        {
            (void)wifi_controller_start();
        }
        return err;
    }

    if (was_enabled)
    {
        err = wifi_controller_start();
        if (err != ESP_OK)
        {
            /* Roll back both runtime and persistent mode if the selected
             * architecture cannot be started. */
            stored.mode = previous_mode;
            (void)wifi_storage_save_network_config(&stored);
            (void)wifi_manager_set_config(&old_config);
            const esp_err_t restore_err = wifi_controller_start();
            if (restore_err != ESP_OK)
            {
                ESP_LOGE(APP_SERVICES_TAG,
                         "Wi-Fi mode rollback could not restart previous mode: %s",
                         esp_err_to_name(restore_err));
            }
            return err;
        }
    }

    ESP_LOGI(APP_SERVICES_TAG, "Wi-Fi mode changed to %s",
             mode == WIFI_MODE_STA ? "STA" : mode == WIFI_MODE_AP ? "AP"
                                                                  : "APSTA");
    app_wifi_sync_menu_if_visible();
    if (sys_state.menu_state == MENU_WIFI_SETTINGS)
    {
        sys_state.menu_selection = (uint8_t)app_menu_wifi_settings_mode_index();
        show_menu_screen(MENU_WIFI_SETTINGS, sys_state.menu_selection);
    }
    return ESP_OK;
}

/* Called by wifi_monitor_task whenever Wi-Fi state changes.  Pushes the
 * new connected/RSSI state to the LCD immediately — no polling required.
 * Must not block for long: it runs in wifi_monitor_task context and must
 * not take any mutex that could be held for an extended period. */
static void app_wifi_lcd_status_callback(const wifi_monitor_status_t *status)
{
    if (status == NULL)
    {
        return;
    }

    /* This indicator represents station connectivity, not internet access.
     * Internet availability is a separate status shown on the Wi-Fi details
     * screen and used to gate remote services such as OTA. */
    const bool connected = status->got_ip;
    lcd_update_wifi_status(connected, connected ? status->rssi : 0);

    /* The callback is invoked only when the monitor snapshot changes. If the
     * Wi-Fi menu is currently visible, redraw it immediately; never redraw
     * over a connecting/result overlay. */
    bool menu_visible = false;
    LCD_LOCK();
    menu_visible = sys_lcd.screen == LCD_SCREEN_MENU;
    LCD_UNLOCK();
    if (menu_visible)
    {
        app_wifi_sync_menu_if_visible();
    }
}

static void app_wifi_status_callback(const wifi_status_t *status)
{
    if (status == NULL || s_services_mutex == NULL)
    {
        return;
    }

    app_wifi_operation_t operation;
    uint32_t operation_generation;
    char ssid[sizeof(s_wifi_operation_ssid)] = {0};
    bool connection_uses_auth = false;
    bool terminal = false;
    bool connected = false;
    bool failed = false;
    int8_t operation_rssi = -127;
    const char *message = NULL;

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    operation = s_wifi_operation;
    operation_generation = s_wifi_operation_generation;
    strncpy(ssid, s_wifi_operation_ssid, sizeof(ssid) - 1U);
    connection_uses_auth = s_wifi_connection_uses_auth;
    operation_rssi = s_wifi_operation_rssi;
    xSemaphoreGive(s_services_mutex);

    if (operation == APP_WIFI_OPERATION_NONE)
    {
        return;
    }

    switch (operation)
    {
    case APP_WIFI_OPERATION_ENABLE:
        /* ON/OFF completion belongs to app_services_execute_wifi_toggle(),
         * after wifi_controller_start() returns.  AP/STA status events are
         * informational and must not race the runtime/NVS commit. */
        break;
    case APP_WIFI_OPERATION_CONNECT_SAVED:
        /* A DHCP lease on the previously connected AP is not proof that the
         * requested connection succeeded. Confirm the actual associated SSID
         * before completing the pending operation (important for same-SSID
         * password changes and delayed monitor callbacks). */
        if (status->got_ip && ssid[0] != '\0')
        {
            if (app_wifi_station_connected_to(ssid))
            {
                terminal = true;
                connected = true;
                message = ssid;
            }
        }
        else if (status->state == WIFI_STATE_FAILED ||
                 (!status->connected &&
                  status->retry_count >= wifi_manager_get_retry_limit()))
        {
            /* APSTA intentionally keeps publishing AP_ACTIVE while the AP is
             * serving clients. Exhausted STA retries are still a terminal
             * failure for this requested connection. */
            terminal = true;
            failed = true;
            message = connection_uses_auth ? "Auth failed" : "Network unavailable";
        }
        break;
    case APP_WIFI_OPERATION_DISABLE:
        /* The controller return value, not WIFI_STATE_IDLE, proves STOP. */
        break;
    case APP_WIFI_OPERATION_DISCONNECT:
        if (!status->connected && !status->got_ip &&
            (status->state == WIFI_STATE_IDLE ||
             status->state == WIFI_STATE_DISCONNECTED ||
             status->state == WIFI_STATE_FAILED ||
             status->state == WIFI_STATE_AP_ACTIVE))
        {
            terminal = true;
            message = "Disconnected";
        }
        break;
    case APP_WIFI_OPERATION_PROVISION:
        if (status->state == WIFI_STATE_PROVISIONING)
        {
            terminal = true;
            message = "Setup AP Active";
        }
        else if (status->state == WIFI_STATE_FAILED)
        {
            terminal = true;
            failed = true;
            message = "Setup AP failed";
        }
        break;
    case APP_WIFI_OPERATION_NONE:
    default:
        break;
    }

    if (!terminal)
    {
        return;
    }

    if (!app_wifi_end_operation_if_generation(operation_generation))
    {
        /* The request changed while this monitor snapshot was being handled. */
        return;
    }
    if (operation == APP_WIFI_OPERATION_CONNECT_SAVED)
    {
        if (connected)
        {
            /* Confirmed station association + DHCP: acknowledge briefly and
             * return quickly to the live Wi-Fi menu. */
            app_wifi_report_connected(ssid, operation_rssi);
        }
        else
        {
            lcd_show_wifi_result(false, failed, false, ssid, operation_rssi,
                                 message != NULL ? message : "Connect failed");
            app_wifi_schedule_menu_refresh(1800U);
        }
    }
    else if (operation == APP_WIFI_OPERATION_DISABLE ||
             operation == APP_WIFI_OPERATION_DISCONNECT)
    {
        app_wifi_sync_menu_if_visible();
    }
    else if (failed)
    {
        lcd_flash_warning_to(message != NULL ? message : "Operation failed",
                             "Try again", 1500U, LCD_SCREEN_MENU);
        app_wifi_schedule_menu_refresh(1800U);
    }
    else
    {
        /* Successful state transitions are shown by the owning menu. */
        app_wifi_sync_menu_if_visible();
    }
}

static esp_err_t persist_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = storage_nvs_open(APP_SERVICES_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    storage_nvs_close(handle);
    return err;
}

static esp_err_t persist_manifest_url(const char *url)
{
    nvs_handle_t handle;
    esp_err_t err = storage_nvs_open(APP_SERVICES_NVS_NAMESPACE, NVS_READWRITE, &handle);
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
        if (err == ESP_ERR_NVS_TYPE_MISMATCH)
        {
            /* Older firmware may have used this key with a different NVS
             * type. Migrate this key only; never erase the whole namespace. */
            const esp_err_t erase_err = nvs_erase_key(handle, APP_OTA_MANIFEST_KEY);
            if (erase_err == ESP_OK || erase_err == ESP_ERR_NVS_NOT_FOUND)
            {
                err = nvs_set_str(handle, APP_OTA_MANIFEST_KEY, url);
            }
            else
            {
                err = erase_err;
            }
        }
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    storage_nvs_close(handle);
    return err;
}

/*
 * Load the user's saved Wi-Fi choice (NVS key "wifi_enabled") into
 * s_wifi_user_choice_on. This is deliberately separate from
 * load_persisted_config(): that function only runs from app_services_init(),
 * which app_main skips when startup is unhealthy, while the automatic Wi-Fi
 * start is NOT gated on startup health. Without this, an unhealthy boot would
 * evaluate the boot policy against the static default (OFF) even though NVS
 * holds ON. Missing key/namespace means the user never switched Wi-Fi on: OFF.
 * Idempotent and safe to call more than once.
 */
static void load_wifi_user_choice(void)
{
    uint8_t wifi_enabled = 0U;
    nvs_handle_t handle;
    const esp_err_t err = storage_nvs_open(APP_SERVICES_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK)
    {
        const esp_err_t get_err = nvs_get_u8(handle, APP_WIFI_ENABLED_KEY, &wifi_enabled);
        storage_nvs_close(handle);
        if (get_err != ESP_OK)
        {
            wifi_enabled = 0U;
            ESP_LOGI(APP_SERVICES_TAG, "No saved Wi-Fi choice (%s): OFF",
                     esp_err_to_name(get_err));
        }
    }
    else
    {
        ESP_LOGW(APP_SERVICES_TAG, "Wi-Fi choice not readable (%s): OFF",
                 esp_err_to_name(err));
    }
    s_wifi_user_choice_on = (wifi_enabled != 0U);
    ESP_LOGI(APP_SERVICES_TAG, "Saved Wi-Fi user choice loaded: %s=%u",
             APP_WIFI_ENABLED_KEY, (unsigned)wifi_enabled);
}

static void load_persisted_config(void)
{
    uint8_t auto_check = 1U;
    nvs_handle_t handle;
    esp_err_t err = storage_nvs_open(APP_SERVICES_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK)
    {
        (void)nvs_get_u8(handle, APP_OTA_AUTOCHECK_KEY, &auto_check);
        size_t length = sizeof(s_manifest_url);
        err = nvs_get_str(handle, APP_OTA_MANIFEST_KEY, s_manifest_url, &length);
        if (err != ESP_OK)
        {
            s_manifest_url[0] = '\0';
        }
        storage_nvs_close(handle);
    }
    else
    {
        s_manifest_url[0] = '\0';
    }
    if (s_manifest_url[0] != '\0' && !app_manifest_url_is_valid(s_manifest_url))
    {
        ESP_LOGW(APP_SERVICES_TAG, "Persisted OTA manifest URL rejected; using configured default");
        s_manifest_url[0] = '\0';
    }

    /*
     * wifi_enabled is the user's last manual Wi-Fi choice. It is loaded as
     * that choice only; it is not the boot policy by itself. The radio is
     * physically OFF after reset, so do not claim it is enabled (or start
     * it) just because the previous session left this key set.
     * app_wifi_background_start_task applies the policy after normal startup
     * is complete: Wi-Fi comes up only when "Wi-Fi Auto On" is ON and the
     * saved choice is ON.
     */
    load_wifi_user_choice();
    sys_state.wifi.enabled = false;
    sys_state.inverter.wifi_enabled = false;
    lcd_update_wifi_enabled(false);
    ESP_LOGI(APP_SERVICES_TAG,
             "Boot Wi-Fi state: OFF (user choice=%u, auto-on=%u, auto-reconnect=%u)",
             s_wifi_user_choice_on ? 1U : 0U,
             sys_state.wifi.auto_enable ? 1U : 0U,
             sys_state.wifi.auto_reconnect ? 1U : 0U);
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

/* Authoritative "an OTA operation is really running": the OTA service's own
 * lifecycle flag, or an active manifest check. The published status/progress
 * value alone can be stale after a failure, cancellation or completion. */
bool app_services_ota_lifecycle_active(void)
{
    if (ota_service_in_progress())
    {
        return true;
    }
    if (s_services_mutex == NULL)
    {
        return false;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool check_active = s_ota_manifest_check_active;
    xSemaphoreGive(s_services_mutex);
    return check_active;
}

/* True when the OTA menu itself is what the user is looking at. */
static bool app_ota_menu_is_visible(void)
{
    lcd_screen_id_t screen;
    LCD_LOCK();
    screen = sys_lcd.screen;
    const menu_state_t menu_state = sys_state.menu_state;
    LCD_UNLOCK();
    return screen == LCD_SCREEN_MENU && menu_state == MENU_OTA;
}

/* respect_menu: background updates must not pull the user from the OTA menu
 * back to the progress screen after they pressed Back. Explicit entry through
 * Firmware Update passes false and always shows the progress screen. */
static void app_services_show_ota_lcd_impl(const app_ota_status_t *status,
                                           bool respect_menu)
{
    if (status == NULL)
    {
        return;
    }

    if (respect_menu &&
        (status->state == APP_OTA_PREPARING ||
         status->state == APP_OTA_DOWNLOADING ||
         status->state == APP_OTA_VERIFYING) &&
        app_ota_menu_is_visible())
    {
        return;
    }

    /* A failed check/install is a transient notification, not a dead-end
     * OTA page. Restore the actionable OTA menu first so the flash returns to
     * Check for Update / Retry / Back instead of the previous screen. */
    if (status->state == APP_OTA_ERROR)
    {
        const char *detail = status->error_detail[0] != '\0'
                                 ? status->error_detail
                                 : "Check Wi-Fi and retry";
        LCD_LOCK();
        sys_state.menu_state = MENU_OTA;
        sys_state.menu_selection = 0U;
        LCD_UNLOCK();
        show_menu_screen(MENU_OTA, 0);
        lcd_flash_warning_to("Update Failed", detail, 1800U, LCD_SCREEN_MENU);
        return;
    }

    /* Up to date is a short notification, not a dead-end page: flash the
     * message, then the running version, and return to the OTA menu. */
    if (status->state == APP_OTA_IDLE)
    {
        char version_line[LCD_LINE_SIZE];
        snprintf(version_line, sizeof(version_line), "VERSION %.12s",
                 status->installed_version);
        LCD_LOCK();
        sys_state.menu_state = MENU_OTA;
        sys_state.menu_selection = 0U;
        LCD_UNLOCK();
        show_menu_screen(MENU_OTA, 0);
        lcd_flash_info_to("SOFTWARE IS", "UP TO DATE", 2000U, LCD_SCREEN_MENU);
        lcd_flash_info_to("SOFTWARE VERSION", version_line, 2500U,
                          LCD_SCREEN_MENU);
        return;
    }

    lcd_ota_view_state_t view = LCD_OTA_VIEW_ERROR;
    switch (status->state)
    {
    case APP_OTA_CHECKING:
        view = LCD_OTA_VIEW_CHECKING;
        break;
    case APP_OTA_PREPARING:
        view = LCD_OTA_VIEW_PREPARING;
        break;
    case APP_OTA_DOWNLOADING:
        view = LCD_OTA_VIEW_DOWNLOADING;
        break;
    case APP_OTA_VERIFYING:
        view = LCD_OTA_VIEW_VERIFYING;
        break;
    case APP_OTA_AVAILABLE:
        view = LCD_OTA_VIEW_AVAILABLE;
        break;
    case APP_OTA_COMPLETE:
        view = LCD_OTA_VIEW_COMPLETE;
        break;
    case APP_OTA_PENDING_APPLY:
        view = LCD_OTA_VIEW_PENDING_APPLY;
        break;
    case APP_OTA_DELETE_CONFIRM:
        view = LCD_OTA_VIEW_DELETE_CONFIRM;
        break;
    case APP_OTA_CANCELLED:
        view = LCD_OTA_VIEW_CANCELLED;
        break;
    case APP_OTA_IDLE:
        view = LCD_OTA_VIEW_CURRENT;
        break;
    case APP_OTA_ERROR:
    case APP_OTA_CONFIRMING:
    default:
        return;
    }
    const char *detail = status->state == APP_OTA_CANCELLED ? "Current kept" : status->state == APP_OTA_IDLE ? "No update"
                                                                                                             : "";
    lcd_show_ota_status(view, (uint8_t)(status->progress_percent < 0 ? 0 : status->progress_percent),
                        status->installed_version, status->available_version,
                        detail, status->state == APP_OTA_CANCELLED);
}

static void app_services_show_ota_lcd(const app_ota_status_t *status)
{
    app_services_show_ota_lcd_impl(status, true);
}

static void app_services_record_ota_error(const char *detail,
                                          bool user_initiated)
{
    if (s_services_mutex == NULL)
    {
        return;
    }

    app_ota_status_t snapshot;
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_ota_status.state = APP_OTA_ERROR;
    s_ota_status.progress_percent = 0;
    s_ota_status.confirmation_pending = false;
    (void)snprintf(s_ota_status.error_detail,
                   sizeof(s_ota_status.error_detail), "%s",
                   (detail != NULL && detail[0] != '\0')
                       ? detail
                       : "OTA operation failed");
    snapshot = s_ota_status;
    xSemaphoreGive(s_services_mutex);

    if (user_initiated)
    {
        app_services_show_ota_lcd(&snapshot);
    }
}

/* Re-enter the OTA progress/status page when the user opens Firmware Update
 * while a check or firmware transfer is already active. */
void app_services_show_ota_status_screen(void)
{
    app_ota_status_t status;
    app_services_get_ota_status(&status);

    /* The persistent staged image takes precedence over the transient
     * in-memory state, including after the 60-second prompt timed out. */
    char pending_version[OTA_MAX_VERSION_LENGTH] = {0};
    if (ota_service_get_pending_version(pending_version,
                                        sizeof(pending_version)) == ESP_OK)
    {
        if (s_services_mutex != NULL)
        {
            xSemaphoreTake(s_services_mutex, portMAX_DELAY);
            s_ota_status.state = APP_OTA_PENDING_APPLY;
            s_ota_status.update_available = true;
            s_ota_status.progress_percent = 100;
            snprintf(s_ota_status.available_version,
                     sizeof(s_ota_status.available_version), "%s",
                     pending_version);
            status = s_ota_status;
            xSemaphoreGive(s_services_mutex);
        }
        app_services_show_ota_lcd_impl(&status, false);
        app_services_schedule_pending_manifest_check();
        s_ota_prompt_last_shown_ms =
            (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        s_ota_prompt_from_boot_reminder = false;
        if (s_ota_apply_prompt_task == NULL &&
            xTaskCreate(ota_apply_prompt_timeout_task, "ota_apply_prompt",
                        3072U, NULL, 3U, &s_ota_apply_prompt_task) != pdPASS)
        {
            s_ota_apply_prompt_task = NULL;
            ESP_LOGW(APP_SERVICES_TAG, "Could not start OTA prompt timeout");
        }
    }
    else if (status.state == APP_OTA_PENDING_APPLY ||
             status.state == APP_OTA_DELETE_CONFIRM)
    {
        if (status.state == APP_OTA_PENDING_APPLY &&
            !ota_service_update_pending())
        {
            /* The marker may have been cleared because the staged image was
             * stale or lacked the new integrity metadata. Do not resurrect its
             * old in-memory prompt after it has been discarded. */
            if (s_services_mutex != NULL)
            {
                xSemaphoreTake(s_services_mutex, portMAX_DELAY);
                s_ota_status.state = APP_OTA_IDLE;
                s_ota_status.update_available = false;
                s_ota_status.progress_percent = 0;
                s_ota_status.available_version[0] = '\0';
                status = s_ota_status;
                xSemaphoreGive(s_services_mutex);
            }
            LCD_LOCK();
            sys_state.menu_state = MENU_OTA;
            if (sys_state.menu_selection >= 3U)
                sys_state.menu_selection = 0U;
            const int selection = sys_state.menu_selection;
            LCD_UNLOCK();
            show_menu_screen(MENU_OTA, selection);
        }
        else
        {
            app_services_show_ota_lcd_impl(&status, false);
        }
    }
    else if ((status.state == APP_OTA_CHECKING ||
              status.state == APP_OTA_PREPARING ||
              status.state == APP_OTA_DOWNLOADING ||
              status.state == APP_OTA_VERIFYING) &&
             app_services_ota_lifecycle_active())
    {
        app_services_show_ota_lcd_impl(&status, false);
    }
    else
    {
        /*
         * ERROR is a completed/failed operation, not an operation in
         * progress. Re-entering Firmware Update after an error must open the
         * actionable OTA menu instead of replaying the OTA Failed screen.
         * The original failure flash remains owned by the failed operation;
         * this branch handles subsequent navigation back into OTA.
         */
        LCD_LOCK();
        sys_state.menu_state = MENU_OTA;
        if (sys_state.menu_selection >= 3U)
        {
            sys_state.menu_selection = 0U;
        }
        const int selection = sys_state.menu_selection;
        LCD_UNLOCK();
        show_menu_screen(MENU_OTA, selection);
    }
}

static const char *app_ota_state_text(app_ota_state_t state)
{
    switch (state)
    {
    case APP_OTA_IDLE:
        return "idle";
    case APP_OTA_CHECKING:
        return "checking";
    case APP_OTA_AVAILABLE:
        return "available";
    case APP_OTA_CONFIRMING:
        return "confirming";
    case APP_OTA_PREPARING:
        return "preparing";
    case APP_OTA_DOWNLOADING:
        return "downloading";
    case APP_OTA_VERIFYING:
        return "verifying";
    case APP_OTA_COMPLETE:
        return "complete";
    case APP_OTA_PENDING_APPLY:
        return "pending_apply";
    case APP_OTA_DELETE_CONFIRM:
        return "delete_confirm";
    case APP_OTA_ERROR:
        return "error";
    case APP_OTA_CANCELLED:
        return "cancelled";
    default:
        return "unknown";
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
    s_ota_status.progress_percent = percent < 0 ? 0 : percent > 100 ? 100
                                                                    : percent;
    app_ota_status_t snapshot = s_ota_status;
    xSemaphoreGive(s_services_mutex);
    websocket_broadcast_ota_status(app_ota_state_text(snapshot.state),
                                   snapshot.progress_percent,
                                   snapshot.available_version,
                                   snapshot.error_detail);
    app_services_show_ota_lcd(&snapshot);
}

static void ota_apply_prompt_timeout_task(void *parameter)
{
    (void)parameter;
    /* Count the timeout from the most recent time the prompt was shown, not
     * from the first time it was created. Reopening the prompt grants a full
     * response window without spawning duplicate timer tasks. */
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
        const uint32_t now_ms =
            (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if ((uint32_t)(now_ms - s_ota_prompt_last_shown_ms) >=
            APP_OTA_APPLY_PROMPT_TIMEOUT_MS)
        {
            break;
        }
    }

    bool timed_out = false;
    if (s_services_mutex != NULL)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        if (s_ota_status.state == APP_OTA_PENDING_APPLY)
        {
            /* Timeout is equivalent to "Later". The staged image remains
             * persisted; importantly, the boot partition is still unchanged. */
            s_ota_status.state = APP_OTA_AVAILABLE;
            s_ota_status.update_available = true;
            s_ota_status.progress_percent = 100;
            timed_out = true;
        }
        s_ota_apply_prompt_task = NULL;
        xSemaphoreGive(s_services_mutex);
    }

    if (timed_out)
    {
        ESP_LOGI(APP_SERVICES_TAG, "OTA apply prompt timed out; current firmware retained");
        bool prompt_still_visible = false;
        LCD_LOCK();
        prompt_still_visible = (sys_lcd.screen == LCD_SCREEN_OTA &&
                                sys_lcd.ota.state == LCD_OTA_VIEW_PENDING_APPLY);
        LCD_UNLOCK();
        if (prompt_still_visible)
        {
            if (s_ota_prompt_from_boot_reminder)
            {
                /* No response to the boot reminder means "later": return to
                 * the normal home screen without changing the boot target. */
                LCD_LOCK();
                sys_state.menu_state = MENU_NONE;
                sys_state.menu_selection = 0U;
                sys_state.in_detail_view = false;
                LCD_UNLOCK();
                lcd_show_main();
            }
            else
            {
                show_menu_screen(MENU_OTA, 1U);
            }
        }
    }
    vTaskDelete(NULL);
}

static void ota_pending_boot_reminder_task(void *parameter)
{
    (void)parameter;
    /* Do not replace the startup sequence or an active screen. Wait until the
     * inverter has released the LCD and returned to its normal home screen. */
    for (;;)
    {
        if (!ota_service_update_pending())
        {
            vTaskDelete(NULL);
            return;
        }
        bool ready = false;
        LCD_LOCK();
        ready = sys_state.system_ready &&
                sys_state.menu_state == MENU_NONE &&
                sys_lcd.screen == LCD_SCREEN_MAIN;
        LCD_UNLOCK();
        if (ready && !lcd_is_startup_active())
            break;
        vTaskDelay(pdMS_TO_TICKS(500U));
    }

    app_ota_status_t snapshot;
    app_services_get_ota_status(&snapshot);
    if (snapshot.state == APP_OTA_PENDING_APPLY &&
        ota_service_update_pending())
    {
        ESP_LOGI(APP_SERVICES_TAG,
                 "Showing boot reminder for staged firmware %s",
                 snapshot.available_version);
        app_services_show_ota_lcd_impl(&snapshot, false);
        app_services_schedule_pending_manifest_check();
        s_ota_prompt_last_shown_ms =
            (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        s_ota_prompt_from_boot_reminder = true;
        if (s_ota_apply_prompt_task == NULL &&
            xTaskCreate(ota_apply_prompt_timeout_task, "ota_apply_prompt",
                        3072U, NULL, 3U, &s_ota_apply_prompt_task) != pdPASS)
        {
            s_ota_apply_prompt_task = NULL;
            ESP_LOGW(APP_SERVICES_TAG, "Could not start OTA reminder timeout");
        }
    }
    vTaskDelete(NULL);
}

static void ota_status_callback(ota_status_t status, int percent)
{
    if (!s_services_mutex)
    {
        return;
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_ota_status.progress_percent = percent < 0 ? 0 : percent > 100 ? 100
                                                                    : percent;
    switch (status)
    {
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
        s_ota_status.state = APP_OTA_PENDING_APPLY;
        s_pending_manifest_checked = false;
        s_ota_status.error_detail[0] = '\0';
        s_ota_status.confirmation_pending = false;
        s_ota_status.cancel_confirmation_pending = false;
        s_ota_cancel_confirmation_pending = false;
        break;
    case OTA_STATUS_CANCELLED:
        s_ota_status.state = APP_OTA_CANCELLED;
        s_ota_status.confirmation_pending = false;
        s_ota_status.cancel_confirmation_pending = false;
        s_ota_cancel_confirmation_pending = false;
        break;
    case OTA_STATUS_FAILED:
    {
        const esp_err_t cause = ota_service_get_last_error();
        s_ota_status.confirmation_pending = false;
        s_ota_status.cancel_confirmation_pending = false;
        s_ota_cancel_confirmation_pending = false;
        if (cause == ESP_ERR_INVALID_VERSION)
        {
            /* The manifest is not newer than the running firmware: this is
             * "up to date", not a failed update. */
            s_ota_status.state = APP_OTA_IDLE;
            s_ota_status.update_available = false;
            s_ota_status.available_version[0] = '\0';
            s_ota_status.error_detail[0] = '\0';
        }
        else
        {
            const char *name = esp_err_to_name(cause);
            if (strncmp(name, "ESP_", 4) == 0)
            {
                name += 4;
            }
            s_ota_status.state = APP_OTA_ERROR;
            snprintf(s_ota_status.error_detail,
                     sizeof(s_ota_status.error_detail), "ERR:%s",
                     cause == ESP_OK ? "IMAGE" : name);
        }
        break;
    }
    case OTA_STATUS_IDLE:
    default:
        break;
    }
    app_ota_status_t snapshot = s_ota_status;
    const bool start_apply_prompt_timer =
        status == OTA_STATUS_SUCCESS && s_ota_apply_prompt_task == NULL;
    xSemaphoreGive(s_services_mutex);

    if (start_apply_prompt_timer)
    {
        s_ota_prompt_last_shown_ms =
            (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        s_ota_prompt_from_boot_reminder = false;
        if (xTaskCreate(ota_apply_prompt_timeout_task, "ota_apply_prompt",
                        3072U, NULL, 3U, &s_ota_apply_prompt_task) != pdPASS)
        {
            ESP_LOGW(APP_SERVICES_TAG,
                     "Could not create OTA apply-prompt timer; user can still defer manually");
            s_ota_apply_prompt_task = NULL;
        }
    }
    if (status == OTA_STATUS_FAILED || status == OTA_STATUS_CANCELLED ||
        status == OTA_STATUS_SUCCESS)
    {
        /* OTA no longer reboots automatically. Restore HTTP/WebSocket and
         * other local services for both failure and the pending-apply state. */
        network_services_resume_after_ota();
    }
    websocket_broadcast_ota_status(app_ota_state_text(snapshot.state),
                                   snapshot.progress_percent,
                                   snapshot.available_version,
                                   snapshot.error_detail);
    app_services_show_ota_lcd(&snapshot);
    if (status == OTA_STATUS_SUCCESS)
    {
        app_services_schedule_pending_manifest_check();
    }
}

static void ota_auto_check_task(void *parameter)
{
    (void)parameter;
    uint32_t wait_ms = 30000U;
    while (wait_ms > 0U)
    {
        const uint32_t slice = wait_ms > 1000U ? 1000U : wait_ms;
        vTaskDelay(pdMS_TO_TICKS(slice));
        wait_ms -= slice;
    }
    while (true)
    {
        app_ota_status_t status;
        app_services_get_ota_status(&status);
        if (status.auto_check_enabled &&
            status.state != APP_OTA_PREPARING &&
            status.state != APP_OTA_DOWNLOADING &&
            status.state != APP_OTA_VERIFYING &&
            status.state != APP_OTA_CHECKING &&
            status.state != APP_OTA_PENDING_APPLY &&
            status.state != APP_OTA_DELETE_CONFIRM &&
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
    s_wifi_scan_active = false;
    s_wifi_scan_cancel_requested = false;
    s_wifi_scan_task = NULL;
    s_wifi_forget_pending = false;
    s_wifi_disconnect_pending = false;
    s_wifi_prerequisite_operation = APP_WIFI_PREREQUISITE_NONE;
    s_wifi_prerequisite_pending = false;
    s_wifi_prerequisite_waiting = false;
    s_wifi_prerequisite_ready = false;
    s_wifi_prerequisite_started_tick = 0;
    xSemaphoreGive(s_services_mutex);

    load_persisted_config();

    if (s_manifest_url[0] == '\0' &&
        APP_OTA_DEFAULT_MANIFEST_URL[0] != '\0')
    {
        const esp_err_t manifest_err =
            app_services_set_ota_manifest_url(APP_OTA_DEFAULT_MANIFEST_URL);
        if (manifest_err != ESP_OK)
        {
            ESP_LOGW(APP_SERVICES_TAG,
                     "Could not provision configured OTA manifest URL: %s",
                     esp_err_to_name(manifest_err));
        }
        else
        {
            ESP_LOGI(APP_SERVICES_TAG, "Configured OTA manifest URL provisioned");
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
        wifi_controller_set_auto_connect(sys_state.wifi.auto_reconnect);
        const esp_err_t callback_err =
            wifi_events_register_status_callback(app_wifi_status_callback);
        if (callback_err != ESP_OK)
        {
            ESP_LOGW(APP_SERVICES_TAG,
                     "Could not register Wi-Fi status callback: %s",
                     esp_err_to_name(callback_err));
        }
        /* Register a wifi_monitor callback to push the Wi-Fi connected/RSSI
         * state to the LCD immediately on every state change.
         *
         * This replaces the previous approach (polling from the ADC manager
         * task every cycle) with an event-driven push that fires only when
         * state actually changes.  The callback runs from wifi_monitor_task
         * context, which is safe for lcd_update_wifi_status() — that function
         * only takes the LCD mutex for a short memcpy, never for network I/O.
         *
         * The lcd_task itself should not poll wifi_monitor_is_online() or
         * wifi_monitor_get_rssi() while rendering. The callback supplies a
         * snapshot asynchronously and keeps LCD rendering independent of
         * network-probe timing and monitor mutex contention. */
        (void)wifi_monitor_register_callback(app_wifi_lcd_status_callback);
        const esp_err_t network_err = network_services_init();
        if (network_err != ESP_OK)
        {
            ESP_LOGW(APP_SERVICES_TAG,
                     "Network services unavailable: %s",
                     esp_err_to_name(network_err));
            if (err == ESP_OK)
            {
                err = network_err;
            }
        }
        /*
         * Do not start the radio from the persisted wifi_enabled runtime key.
         * Auto Wi-Fi policy is applied once, after startup readiness, through
         * app_services_set_wifi_enabled() and the normal toggle worker.
         */
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
        char pending_version[OTA_MAX_VERSION_LENGTH] = {0};
        if (ota_service_get_pending_version(pending_version,
                                            sizeof(pending_version)) == ESP_OK)
        {
            s_ota_status.state = APP_OTA_PENDING_APPLY;
            s_ota_status.update_available = true;
            s_ota_status.progress_percent = 100;
            snprintf(s_ota_status.available_version,
                     sizeof(s_ota_status.available_version), "%s",
                     pending_version);
            ESP_LOGI(APP_SERVICES_TAG,
                     "Verified OTA image is pending explicit activation: %s",
                     pending_version);
            if (xTaskCreate(ota_pending_boot_reminder_task, "ota_pending_reminder",
                            3072U, NULL, 3U, NULL) != pdPASS)
            {
                ESP_LOGW(APP_SERVICES_TAG, "Could not create pending OTA boot reminder");
            }
        }
    }

    if (!s_ota_check_task)
    {
        if (xTaskCreatePinnedToCore(ota_auto_check_task, "ota_check", APP_OTA_TASK_STACK_SIZE,
                                    NULL, APP_OTA_CHECK_TASK_PRIORITY, &s_ota_check_task, APP_CORE_SYSTEM) != pdPASS)
        {
            s_ota_check_task = NULL;
            ESP_LOGW(APP_SERVICES_TAG, "Could not create OTA availability task");
        }
    }
    /* Wi-Fi toggle control is initialized independently of the optional
     * network/OTA services.  This keeps the panel ON/OFF action available
     * whenever Wi-Fi itself can be initialized, including a startup-fault
     * state where the broader background-service startup is suppressed. */
    if (!s_wifi_toggle_queue)
    {
        s_wifi_toggle_queue = xQueueCreate(APP_WIFI_TOGGLE_QUEUE_LENGTH,
                                           sizeof(wifi_toggle_request_t));
        if (!s_wifi_toggle_queue)
        {
            ESP_LOGE(APP_SERVICES_TAG, "Could not create Wi-Fi toggle queue");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_wifi_toggle_task)
    {
        if (xTaskCreatePinnedToCore(app_wifi_toggle_task,
                                    "wifi_toggle",
                                    APP_WIFI_TOGGLE_TASK_STACK_SIZE,
                                    NULL,
                                    APP_WIFI_TOGGLE_TASK_PRIORITY,
                                    &s_wifi_toggle_task, APP_CORE_SYSTEM) != pdPASS)
        {
            vQueueDelete(s_wifi_toggle_queue);
            s_wifi_toggle_queue = NULL;
            s_wifi_toggle_task = NULL;
            ESP_LOGW(APP_SERVICES_TAG, "Could not create Wi-Fi toggle worker");
            return ESP_ERR_NO_MEM;
        }
        task_watchdog_register_health_only("wifi_toggle");
    }
    (void)app_wifi_ensure_watcher();
    if (!s_wifi_prerequisite_task)
    {
        if (xTaskCreatePinnedToCore(app_wifi_prerequisite_task,
                                    "wifi_prereq",
                                    APP_WIFI_PREREQUISITE_STACK_SIZE,
                                    NULL,
                                    APP_WIFI_PREREQUISITE_PRIORITY,
                                    &s_wifi_prerequisite_task, APP_CORE_SYSTEM) != pdPASS)
        {
            s_wifi_prerequisite_task = NULL;
            ESP_LOGW(APP_SERVICES_TAG, "Could not create Wi-Fi prerequisite task");
        }
        else
        {
            task_watchdog_register_health_only("wifi_prereq");
        }
    }
    return err == ESP_OK ? ota_err : err;
}

static esp_err_t app_wifi_ensure_watcher(void)
{
    if (s_wifi_operation_watch_task != NULL)
    {
        return ESP_OK;
    }
    if (xTaskCreatePinnedToCore(app_wifi_operation_watch_task,
                                "wifi_op_watch",
                                APP_WIFI_OPERATION_WATCH_STACK_SIZE,
                                NULL,
                                APP_WIFI_OPERATION_WATCH_PRIORITY,
                                &s_wifi_operation_watch_task,
                                APP_CORE_SYSTEM) != pdPASS)
    {
        s_wifi_operation_watch_task = NULL;
        ESP_LOGW(APP_SERVICES_TAG, "Could not create Wi-Fi operation watcher");
        return ESP_ERR_NO_MEM;
    }
    task_watchdog_register_health_only("wifi_op_watch");
    return ESP_OK;
}

esp_err_t app_services_wifi_toggle_init(void)
{
    if (s_services_mutex == NULL)
    {
        s_services_mutex = xSemaphoreCreateMutex();
        if (s_services_mutex == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Runs on every boot, healthy or not, before the automatic Wi-Fi start. */
    load_wifi_user_choice();

    if (s_wifi_toggle_queue == NULL)
    {
        s_wifi_toggle_queue = xQueueCreate(APP_WIFI_TOGGLE_QUEUE_LENGTH,
                                           sizeof(wifi_toggle_request_t));
        if (s_wifi_toggle_queue == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_wifi_toggle_task == NULL)
    {
        if (xTaskCreatePinnedToCore(app_wifi_toggle_task,
                                    "wifi_toggle",
                                    APP_WIFI_TOGGLE_TASK_STACK_SIZE,
                                    NULL,
                                    APP_WIFI_TOGGLE_TASK_PRIORITY,
                                    &s_wifi_toggle_task, APP_CORE_SYSTEM) != pdPASS)
        {
            vQueueDelete(s_wifi_toggle_queue);
            s_wifi_toggle_queue = NULL;
            s_wifi_toggle_task = NULL;
            return ESP_ERR_NO_MEM;
        }
        task_watchdog_register_health_only("wifi_toggle");
    }

    const esp_err_t controller_err = wifi_controller_init();
    if (controller_err != ESP_OK)
    {
        ESP_LOGW(APP_SERVICES_TAG,
                 "Wi-Fi controller initialization deferred: %s",
                 esp_err_to_name(controller_err));
    }
    else
    {
        wifi_controller_set_auto_connect(sys_state.wifi.auto_reconnect);
    }
    /* The label watcher must exist even when broader services are suppressed
     * after a failed POST; it only reads state and redraws the Wi-Fi menu. */
    (void)app_wifi_ensure_watcher();
    return controller_err;
}

static esp_err_t app_wifi_request_enabled(bool enabled, bool silent)
{
    /* The Wi-Fi ON/OFF control path must not depend on the broader
     * app_services startup having completed.  In particular, main.c
     * deliberately suppresses background services when POST/startup is
     * unhealthy, while the button/event tasks remain alive for safe
     * diagnostics and recovery.  Lazily establish only the mutex, toggle
     * queue/worker, and Wi-Fi controller required by this action. */
    if (s_services_mutex == NULL)
    {
        s_services_mutex = xSemaphoreCreateMutex();
        if (s_services_mutex == NULL)
        {
            ESP_LOGE(APP_SERVICES_TAG,
                     "Wi-Fi toggle rejected: could not create services mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_wifi_toggle_queue == NULL)
    {
        s_wifi_toggle_queue = xQueueCreate(APP_WIFI_TOGGLE_QUEUE_LENGTH,
                                           sizeof(wifi_toggle_request_t));
        if (s_wifi_toggle_queue == NULL)
        {
            ESP_LOGE(APP_SERVICES_TAG,
                     "Wi-Fi toggle rejected: could not create toggle queue");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_wifi_toggle_task == NULL)
    {
        if (xTaskCreatePinnedToCore(app_wifi_toggle_task,
                                    "wifi_toggle",
                                    APP_WIFI_TOGGLE_TASK_STACK_SIZE,
                                    NULL,
                                    APP_WIFI_TOGGLE_TASK_PRIORITY,
                                    &s_wifi_toggle_task, APP_CORE_SYSTEM) != pdPASS)
        {
            vQueueDelete(s_wifi_toggle_queue);
            s_wifi_toggle_queue = NULL;
            s_wifi_toggle_task = NULL;
            ESP_LOGE(APP_SERVICES_TAG,
                     "Wi-Fi toggle rejected: could not create toggle worker");
            return ESP_ERR_NO_MEM;
        }
        task_watchdog_register_health_only("wifi_toggle");
    }

    const esp_err_t controller_init_err = wifi_controller_init();
    if (controller_init_err != ESP_OK)
    {
        ESP_LOGE(APP_SERVICES_TAG,
                 "Wi-Fi toggle rejected: controller initialization failed: %s",
                 esp_err_to_name(controller_init_err));
        return controller_init_err;
    }
    wifi_controller_set_auto_connect(sys_state.wifi.auto_reconnect);

    ESP_LOGI(APP_SERVICES_TAG, "Wi-Fi toggle request: %s",
             enabled ? "ON" : "OFF");

    /* Atomic admission: only one toggle may be pending or executing at a
     * time.  The compare-and-swap atomically checks that no toggle is live
     * and marks one as admitted in a single indivisible operation, so a
     * second concurrent caller cannot slip in between the check and the
     * mark.  The flag is released by the worker AFTER execute_wifi_toggle
     * returns -- never before. */
    uint32_t expected = 0U;
    if (!__atomic_compare_exchange_n(&s_wifi_toggle_admitted,
                                     &expected, 1U,
                                     false,
                                     __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST))
    {
        ESP_LOGW(APP_SERVICES_TAG, "Wi-Fi toggle rejected: operation already pending");
        if (!silent)
        {
            lcd_flash_message("Wi-Fi Busy", "Please wait", 900U);
        }
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(APP_SERVICES_TAG, "Wi-Fi toggle admitted");

    /* Build request and operation state now that admission is guaranteed. */
    char ssid[WIFI_MAX_SSID_LEN + 1U] = {0};
    if (enabled)
    {
        strncpy(ssid, WIFI_COMPILED_STA_SSID, sizeof(ssid) - 1U);
    }

    wifi_toggle_request_t request = {
        .enabled = enabled,
        .previous_enabled = sys_state.wifi.enabled,
        .mode_change = false,
        .mode = WIFI_MODE_NULL,
        .requested_ms = (uint64_t)(esp_timer_get_time() / 1000ULL),
        .silent = silent,
    };

    app_wifi_begin_operation(enabled ? APP_WIFI_OPERATION_ENABLE
                                     : APP_WIFI_OPERATION_DISABLE,
                             ssid, -127);

    if (xQueueSend(s_wifi_toggle_queue, &request, 0) != pdTRUE)
    {
        /* Queue full -- should not happen (queue length 1 and admitted flag
         * prevents concurrent senders), but handle defensively. */
        app_wifi_end_operation();
        __atomic_store_n(&s_wifi_toggle_admitted, 0U, __ATOMIC_SEQ_CST);
        if (!silent)
        {
            lcd_flash_message("Wi-Fi Busy", "Please wait", 900U);
        }
        ESP_LOGE(APP_SERVICES_TAG, "Wi-Fi toggle queue send failed (full?)");
        return ESP_ERR_TIMEOUT;
    }

    /* Immediate progress feedback to the LCD. */
    if (!silent)
    {
        lcd_flash_message(enabled ? "Wi-Fi STARTING" : "Wi-Fi STOPPING",
                          "Please wait", 900U);
    }
    ESP_LOGI(APP_SERVICES_TAG, "Wi-Fi %s request queued%s",
             enabled ? "ON" : "OFF", silent ? " (silent, automatic)" : "");
    return ESP_OK;
}

esp_err_t app_services_set_wifi_enabled(bool enabled)
{
    return app_wifi_request_enabled(enabled, false);
}

bool app_services_wifi_enabled(void)
{
    return sys_state.wifi.enabled;
}

const char *app_services_wifi_mode_name(void)
{
    if (s_wifi_mode_edit_active)
    {
        switch (s_wifi_mode_edit_value)
        {
        case WIFI_MODE_STA:
            return "STA";
        case WIFI_MODE_AP:
            return "AP";
        case WIFI_MODE_APSTA:
            return "APSTA";
        default:
            break;
        }
    }

    wifi_mode_t mode = wifi_manager_get_mode();
    if (mode == WIFI_MODE_NULL)
    {
        wifi_network_config_t stored = {0};
        if (wifi_storage_load_network_config(&stored) == ESP_OK)
        {
            mode = stored.mode;
        }
    }

    switch (mode)
    {
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

esp_err_t app_services_wifi_mode_edit_begin(void)
{
    esp_err_t err = wifi_controller_init();
    if (err != ESP_OK)
    {
        return err;
    }
    wifi_controller_set_auto_connect(sys_state.wifi.auto_reconnect);

    wifi_manager_config_t config = {0};
    err = wifi_manager_get_config(&config);
    if (err != ESP_OK)
    {
        return err;
    }

    if (config.mode < WIFI_MODE_STA || config.mode > WIFI_MODE_APSTA)
    {
        config.mode = WIFI_COMPILED_OPERATION_MODE;
    }

    s_wifi_mode_edit_value = config.mode;
    s_wifi_mode_edit_active = true;
    return ESP_OK;
}

esp_err_t app_services_wifi_mode_edit_step(int direction)
{
    if (!s_wifi_mode_edit_active || direction == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    static const wifi_mode_t modes[] = {
        WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA};
    size_t index = 0U;
    for (size_t i = 0U; i < sizeof(modes) / sizeof(modes[0]); ++i)
    {
        if (modes[i] == s_wifi_mode_edit_value)
        {
            index = i;
            break;
        }
    }

    const size_t count = sizeof(modes) / sizeof(modes[0]);
    if (direction > 0)
    {
        index = (index + 1U) % count;
    }
    else
    {
        index = (index + count - 1U) % count;
    }
    s_wifi_mode_edit_value = modes[index];
    return ESP_OK;
}

esp_err_t app_services_wifi_mode_edit_confirm(void)
{
    if (!s_wifi_mode_edit_active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const wifi_mode_t mode = s_wifi_mode_edit_value;
    s_wifi_mode_edit_active = false;

    /* The selector must remain usable even when the broader application
     * services were suppressed during startup recovery. Establish only the
     * existing Wi-Fi mode-change worker path; do not start unrelated services. */
    if (s_wifi_toggle_queue == NULL)
    {
        s_wifi_toggle_queue = xQueueCreate(APP_WIFI_TOGGLE_QUEUE_LENGTH,
                                           sizeof(wifi_toggle_request_t));
        if (s_wifi_toggle_queue == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_wifi_toggle_task == NULL)
    {
        if (xTaskCreatePinnedToCore(app_wifi_toggle_task,
                                    "wifi_toggle",
                                    APP_WIFI_TOGGLE_TASK_STACK_SIZE,
                                    NULL,
                                    APP_WIFI_TOGGLE_TASK_PRIORITY,
                                    &s_wifi_toggle_task, APP_CORE_SYSTEM) != pdPASS)
        {
            vQueueDelete(s_wifi_toggle_queue);
            s_wifi_toggle_queue = NULL;
            return ESP_ERR_NO_MEM;
        }
        task_watchdog_register_health_only("wifi_toggle");
    }

    uint32_t expected = 0U;
    if (!__atomic_compare_exchange_n(&s_wifi_toggle_admitted, &expected, 1U,
                                     false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        return ESP_ERR_TIMEOUT;
    }

    wifi_toggle_request_t request = {
        .enabled = false,
        .previous_enabled = sys_state.wifi.enabled,
        .mode_change = true,
        .mode = mode,
        .requested_ms = (uint64_t)(esp_timer_get_time() / 1000ULL),
    };

    if (xQueueSend(s_wifi_toggle_queue, &request, 0) != pdTRUE)
    {
        __atomic_store_n(&s_wifi_toggle_admitted, 0U, __ATOMIC_SEQ_CST);
        return ESP_ERR_TIMEOUT;
    }

    lcd_flash_message("Wi-Fi MODE", "Saving...", 900U);
    return ESP_OK;
}

void app_services_wifi_mode_edit_cancel(void)
{
    s_wifi_mode_edit_active = false;
}

bool app_services_wifi_mode_edit_active(void)
{
    return s_wifi_mode_edit_active;
}

const char *app_services_wifi_connect_action_label(void)
{
    static const char *const k_connecting[APP_WIFI_ANIM_PHASES] = {
        "Connecting.", "Connecting..", "Connecting..."};
    static const char *const k_disconnecting[APP_WIFI_ANIM_PHASES] = {
        "Disconnecting.", "Disconnecting..", "Disconnecting..."};

    if (wifi_manager_get_mode() == WIFI_MODE_AP)
    {
        return "AP Clients";
    }

    /* All labels are string literals: no shared mutable buffer, so concurrent
     * callers (watcher, menu renderer, input task) cannot tear each other. */
    const uint8_t phase =
        __atomic_load_n(&s_wifi_anim_phase, __ATOMIC_RELAXED) %
        APP_WIFI_ANIM_PHASES;
    switch (app_wifi_ui_state())
    {
    case APP_WIFI_UI_DISCONNECTING:
        return k_disconnecting[phase];
    case APP_WIFI_UI_CONNECTING:
        return k_connecting[phase];
    case APP_WIFI_UI_DISCONNECT:
        return "Disconnect";
    case APP_WIFI_UI_CONNECT:
    default:
        return "Connect";
    }
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
    if (!sys_state.wifi.enabled)
    {
        lcd_flash_message("Wi-Fi Disabled", "Enable first", 1400U);
        return ESP_ERR_INVALID_STATE;
    }
    if (wifi_manager_get_mode() == WIFI_MODE_AP)
    {
        lcd_flash_message("Scan Unavailable", "AP mode only", 1400U);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_services_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    if (s_wifi_scan_active)
    {
        xSemaphoreGive(s_services_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_wifi_scan_active = true;
    s_wifi_scan_cancel_requested = false;
    xSemaphoreGive(s_services_mutex);

    lcd_show_wifi_scan_start();
    if (xTaskCreatePinnedToCore(app_wifi_scan_task, "wifi_scan_ui",
                                APP_WIFI_SCAN_TASK_STACK_SIZE, NULL,
                                APP_WIFI_SCAN_TASK_PRIORITY, &s_wifi_scan_task, APP_CORE_SYSTEM) != pdPASS)
    {
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
    if (s_services_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool active = s_wifi_scan_active;
    if (active)
    {
        s_wifi_scan_cancel_requested = true;
    }
    xSemaphoreGive(s_services_mutex);

    if (!active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* Stop the lower-level ESP-IDF scan immediately when one is in progress;
     * the worker task will then publish any results the scan API retained. */
    const esp_err_t err = wifi_scan_cancel();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }
    return ESP_OK;
}

bool app_services_wifi_scan_is_active(void)
{
    bool active = false;
    if (s_services_mutex != NULL)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        active = s_wifi_scan_active;
        xSemaphoreGive(s_services_mutex);
    }
    return active;
}

bool app_services_wifi_operation_in_progress(void)
{
    bool operation_pending = false;
    if (s_services_mutex != NULL)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        operation_pending = (s_wifi_operation != APP_WIFI_OPERATION_NONE) ||
                            s_wifi_scan_active;
        xSemaphoreGive(s_services_mutex);
    }
    /* Toggle admission remains set until radio transition and NVS commit end. */
    return operation_pending || s_wifi_toggle_admitted != 0U;
}

void app_services_show_wifi_network_details(uint8_t selected_index)
{
    /* A network selection is an action, not a separate details page:
     * open networks connect immediately and secured networks go straight to
     * password entry. This avoids making the user select the same SSID twice. */
    (void)app_services_wifi_connect_selected(selected_index);
}

static esp_err_t app_services_wifi_connect_network_with_authmode(
    const char *ssid, const char *password, int8_t rssi,
    wifi_auth_mode_t authmode)
{
    if (ssid == NULL || ssid[0] == '\0' || password == NULL ||
        strlen(ssid) > WIFI_MAX_SSID_LEN || strlen(password) > 63U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    /* wifi_manager_config_valid() rejects non-empty station passwords under
     * WPA's 8-character minimum. Report that exact problem instead of the
     * misleading generic "Network Invalid" message. */
    if (password[0] != '\0' && strlen(password) < 8U)
    {
        lcd_flash_message("Password Too Short", "Use 8+ characters", 1400U);
        return ESP_ERR_INVALID_ARG;
    }
    if (!sys_state.wifi.enabled)
    {
        lcd_flash_message("Wi-Fi Disabled", "Enable first", 1400U);
        return ESP_ERR_INVALID_STATE;
    }
    if (wifi_manager_get_mode() == WIFI_MODE_AP)
    {
        lcd_flash_message("AP Mode", "No STA Connect", 1400U);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Compare the currently associated SSID before changing credentials.
     * An already-working station must not be needlessly disconnected and
     * reconnected, even if the scan result used a different auth-mode label. */
    wifi_status_t live_status = {0};
    const bool station_ready =
        wifi_events_get_status_copy(&live_status) == ESP_OK &&
        live_status.got_ip;
    if (station_ready && wifi_controller_is_connected())
    {
        wifi_ap_record_t current_ap = {0};
        if (esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK &&
            strncmp((const char *)current_ap.ssid, ssid,
                    sizeof(current_ap.ssid)) == 0)
        {
            show_menu_screen(MENU_WIFI_CONFIG, sys_state.menu_selection);
            return ESP_OK;
        }
    }

    /* The manager config represents the saved target, not necessarily the
     * currently associated AP. Avoid duplicate in-flight requests to that
     * same target, but permit a fresh attempt after a failed authentication. */
    wifi_manager_config_t config = {0};
    esp_err_t err = wifi_manager_get_config(&config);
    if (err != ESP_OK)
    {
        return err;
    }

    const bool same_active_config =
        strcmp(config.ssid, ssid) == 0 &&
        strcmp(config.password, password) == 0;
    if (same_active_config &&
        wifi_controller_get_state() == WIFI_CONTROLLER_CONNECTING)
    {
        return ESP_OK;
    }

    /* ESP32 has one station association. Explicitly drop a different active
     * or in-flight station before replacing the target credentials. */
    if (station_ready || wifi_controller_get_state() == WIFI_CONTROLLER_CONNECTING)
    {
        const esp_err_t disconnect_err = wifi_controller_disconnect();
        if (disconnect_err != ESP_OK &&
            disconnect_err != ESP_ERR_WIFI_NOT_CONNECT &&
            disconnect_err != ESP_ERR_WIFI_NOT_STARTED)
        {
            ESP_LOGW(APP_SERVICES_TAG,
                     "Could not disconnect previous station before switching: %s",
                     esp_err_to_name(disconnect_err));
            return disconnect_err;
        }
    }

    strncpy(config.ssid, ssid, sizeof(config.ssid) - 1U);
    config.ssid[sizeof(config.ssid) - 1U] = '\0';
    strncpy(config.password, password, sizeof(config.password) - 1U);
    config.password[sizeof(config.password) - 1U] = '\0';
    config.authmode = authmode;
    err = wifi_manager_set_config(&config);
    if (err != ESP_OK)
    {
        ESP_LOGE(APP_SERVICES_TAG,
                 "Rejected scanned-network configuration for SSID='%s': %s (0x%x)",
                 ssid, esp_err_to_name(err), err);
        lcd_flash_message("Wi-Fi Config Error", "Check credentials", 1400U);
        return err;
    }

    wifi_credentials_t credentials = {0};
    strncpy(credentials.ssid, ssid, sizeof(credentials.ssid) - 1U);
    strncpy(credentials.password, password, sizeof(credentials.password) - 1U);
    (void)wifi_storage_save_credentials(&credentials);

    if (s_services_mutex != NULL)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_wifi_connection_uses_auth = password[0] != '\0';
        xSemaphoreGive(s_services_mutex);
    }
    app_wifi_begin_operation(APP_WIFI_OPERATION_CONNECT_SAVED, ssid, rssi);
    lcd_show_wifi_connecting(ssid, rssi);
    err = wifi_controller_reconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
    {
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
    if (sys_lcd.screen == LCD_SCREEN_WIFI_NETWORK_DETAILS)
    {
        strncpy(ssid, sys_lcd.wifi_network_detail.ssid, sizeof(ssid) - 1U);
        selected_rssi = sys_lcd.wifi_network_detail.rssi;
        authmode = sys_lcd.wifi_network_detail.authmode;
    }
    else if (sys_lcd.screen == LCD_SCREEN_WIFI_SCAN &&
             sys_lcd.wifi_scan.stage == LCD_WIFI_SCAN_COMPLETE &&
             selected_index < sys_lcd.wifi_scan.count)
    {
        strncpy(ssid, sys_lcd.wifi_scan.ssid[selected_index], sizeof(ssid) - 1U);
        selected_rssi = sys_lcd.wifi_scan.rssi[selected_index];
        authmode = sys_lcd.wifi_scan.authmode[selected_index];
    }
    else
    {
        LCD_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    LCD_UNLOCK();

    if ((wifi_auth_mode_t)authmode == WIFI_AUTH_OPEN)
    {
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
    if (sys_lcd.screen != LCD_SCREEN_WIFI_PASSWORD)
    {
        LCD_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    strncpy(ssid, sys_lcd.wifi_password.ssid, sizeof(ssid) - 1U);
    strncpy(password, sys_lcd.wifi_password.password, sizeof(password) - 1U);
    rssi = sys_lcd.wifi_password.rssi;
    authmode = sys_lcd.wifi_password.authmode;
    LCD_UNLOCK();

    if (password[0] == '\0')
    {
        lcd_flash_message("Password required", "Enter network key", 1200U);
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

/* Boot policy. "Wi-Fi Auto On" is the master: when it is OFF, Wi-Fi stays OFF
 * after every boot until the user switches it on. When it is ON, Wi-Fi comes
 * back only if the user left it ON (saved choice); if the user left it OFF, or
 * never switched it on, it stays OFF until the user turns it on. */
static bool app_wifi_boot_enable_wanted(void)
{
    return sys_state.wifi.auto_enable && s_wifi_user_choice_on;
}

/* True when the boot policy asks for the radio to be switched ON and it is
 * currently OFF. Re-evaluated inside the worker, so a manual OFF made while
 * the boot start is pending cancels it. Never touches the LCD. */
static bool app_wifi_background_enable_needed(void)
{
    return app_wifi_boot_enable_wanted() && !sys_state.wifi.enabled;
}

/* Decides whether a silent post-boot connect to the saved network is allowed.
 * Evaluated when the request is made and again inside the worker, since the
 * user may act in between. Never touches the LCD. */
static bool app_wifi_background_connect_allowed(void)
{
    if (!sys_state.wifi.auto_reconnect)
    {
        /* Auto-reconnect is the gate for connecting: when it is off the
         * station only connects on an explicit user request. */
        return false;
    }
    if (!sys_state.wifi.enabled || wifi_manager_get_mode() == WIFI_MODE_AP)
    {
        /* Wi-Fi must already be enabled; a connect cannot work otherwise. */
        return false;
    }
    if (wifi_controller_manual_disconnect_active())
    {
        return false;
    }
    if (app_services_wifi_operation_in_progress() ||
        app_services_wifi_scan_is_active())
    {
        return false;
    }

    switch (wifi_controller_get_state())
    {
    case WIFI_CONTROLLER_STARTING:
    case WIFI_CONTROLLER_CONNECTING:
    case WIFI_CONTROLLER_CONNECTED:
    case WIFI_CONTROLLER_PROVISIONING:
        return false;
    case WIFI_CONTROLLER_IDLE:
    case WIFI_CONTROLLER_AP_ACTIVE:
    case WIFI_CONTROLLER_ERROR:
    default:
        break;
    }

    wifi_status_t status = {0};
    if (wifi_events_get_status_copy(&status) == ESP_OK && status.got_ip)
    {
        return false;
    }

    wifi_manager_config_t config = {0};
    return wifi_manager_get_config(&config) == ESP_OK && config.ssid[0] != '\0';
}

/*
 * Use the same queued ON path as the physical Wi-Fi menu. This keeps driver
 * serialization, controller initialization, runtime state, NVS persistence,
 * and error handling owned by the existing toggle worker instead of
 * maintaining a second background-only radio-start implementation.
 */
static esp_err_t app_wifi_background_enable(void)
{
    if (!app_wifi_background_enable_needed())
    {
        return ESP_OK;
    }

    ESP_LOGI(APP_SERVICES_TAG,
             "Auto Wi-Fi On=1 (auto-reconnect=%u): requesting ON through existing toggle path",
             sys_state.wifi.auto_reconnect ? 1U : 0U);
    /* Same queued ON path as the menu; only the LCD popups are suppressed. A
     * busy admission (manual toggle in flight) is retried, not dropped. */
    const TickType_t retry_started = xTaskGetTickCount();
    esp_err_t err = app_wifi_request_enabled(true, true);
    while (err == ESP_ERR_TIMEOUT && app_wifi_background_enable_needed() &&
           (xTaskGetTickCount() - retry_started) <
               pdMS_TO_TICKS(APP_WIFI_OPERATION_TIMEOUT_MS))
    {
        ESP_LOGI(APP_SERVICES_TAG,
                 "Auto Wi-Fi On deferred: toggle path busy, retrying");
        vTaskDelay(pdMS_TO_TICKS(500U));
        (void)task_watchdog_health_feed();
        err = app_wifi_request_enabled(true, true);
    }
    return err;
}

static void app_wifi_background_start_task(void *parameter)
{
    (void)parameter;
    bool ready_for_connect = true;

    /*
     * The public ON path queues work and returns immediately. Wait here,
     * only in this low-priority background task, for the existing toggle
     * worker to finish. app_main and inverter safety/startup never wait for
     * Wi-Fi readiness.
     */
    if (app_wifi_background_enable_needed())
    {
        const esp_err_t enable_err = app_wifi_background_enable();
        if (enable_err != ESP_OK)
        {
            ESP_LOGW(APP_SERVICES_TAG,
                     "Auto Wi-Fi On request was not accepted: %s",
                     esp_err_to_name(enable_err));
            ready_for_connect = false;
        }

        if (ready_for_connect)
        {
            const TickType_t enable_wait_started = xTaskGetTickCount();
            while (__atomic_load_n(&s_wifi_toggle_admitted, __ATOMIC_SEQ_CST) != 0U &&
                   (xTaskGetTickCount() - enable_wait_started) <
                       pdMS_TO_TICKS(APP_WIFI_OPERATION_TIMEOUT_MS))
            {
                vTaskDelay(pdMS_TO_TICKS(100U));
                (void)task_watchdog_health_feed();
            }
            if (__atomic_load_n(&s_wifi_toggle_admitted, __ATOMIC_SEQ_CST) != 0U)
            {
                ESP_LOGW(APP_SERVICES_TAG,
                         "Auto Wi-Fi On timed out waiting for the existing toggle worker");
                ready_for_connect = false;
            }
        }

        if (ready_for_connect)
        {
            /*
             * wifi_controller_start() may return before its state machine
             * leaves STARTING. Do not issue a saved-network reconnect while
             * the driver is still starting. CONNECTING means an existing
             * connection attempt is already in progress and must be left alone.
             */
            const TickType_t controller_wait_started = xTaskGetTickCount();
            while (wifi_controller_get_state() == WIFI_CONTROLLER_STARTING &&
                   (xTaskGetTickCount() - controller_wait_started) <
                       pdMS_TO_TICKS(APP_WIFI_OPERATION_TIMEOUT_MS))
            {
                vTaskDelay(pdMS_TO_TICKS(100U));
                (void)task_watchdog_health_feed();
            }
            if (wifi_controller_get_state() == WIFI_CONTROLLER_STARTING)
            {
                ESP_LOGW(APP_SERVICES_TAG,
                         "Auto Wi-Fi On: controller did not leave STARTING before timeout");
                ready_for_connect = false;
            }
        }
    }

    if (ready_for_connect && app_wifi_background_connect_allowed())
    {
        /*
         * Reuse the controller's existing saved-network reconnect mechanism.
         * Do not register a foreground operation or display a connection
         * failure when the saved AP is unavailable; the controller's existing
         * auto-reconnect policy owns retry/backoff and status reporting.
         */
        const esp_err_t err = wifi_controller_reconnect();
        if (err == ESP_OK || err == ESP_ERR_WIFI_CONN)
        {
            ESP_LOGI(APP_SERVICES_TAG, "Background saved-network connect started");
        }
        else
        {
            ESP_LOGW(APP_SERVICES_TAG,
                     "Background saved-network connect request failed: %s",
                     esp_err_to_name(err));
        }
    }
    else if (ready_for_connect)
    {
        ESP_LOGI(APP_SERVICES_TAG,
                 "Background saved-network connect not needed or not allowed");
    }

    ESP_LOGI(APP_SERVICES_TAG,
             "wifi_bg_start done (stack high-water: %u words, controller=%s)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             wifi_state_text(wifi_controller_get_state()));
    s_wifi_background_connect_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t app_services_wifi_background_start(void)
{
    ESP_LOGI(APP_SERVICES_TAG,
             "Automatic Wi-Fi start: auto-on=%u user-choice=%u auto-reconnect=%u wifi-enabled=%u controller=%s",
             sys_state.wifi.auto_enable ? 1U : 0U,
             s_wifi_user_choice_on ? 1U : 0U,
             sys_state.wifi.auto_reconnect ? 1U : 0U,
             sys_state.wifi.enabled ? 1U : 0U,
             wifi_state_text(wifi_controller_get_state()));
    if (!app_wifi_background_enable_needed() &&
        !app_wifi_background_connect_allowed())
    {
        ESP_LOGI(APP_SERVICES_TAG, "Background Wi-Fi start not needed");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_wifi_background_connect_task != NULL)
    {
        return ESP_OK; /* already queued; idempotent */
    }

    TaskHandle_t handle = NULL;
    if (xTaskCreatePinnedToCore(app_wifi_background_start_task,
                                "wifi_bg_start",
                                APP_WIFI_BACKGROUND_CONNECT_STACK_SIZE,
                                NULL,
                                APP_WIFI_BACKGROUND_CONNECT_PRIORITY,
                                &handle, APP_CORE_SYSTEM) != pdPASS)
    {
        ESP_LOGW(APP_SERVICES_TAG,
                 "Could not create background Wi-Fi start task");
        return ESP_ERR_NO_MEM;
    }
    s_wifi_background_connect_task = handle;
    return ESP_OK;
}

esp_err_t app_services_wifi_reconnect(void)
{
    wifi_status_t current_status = {0};
    if (wifi_events_get_status_copy(&current_status) == ESP_OK &&
        current_status.got_ip &&
        wifi_controller_is_connected())
    {
        app_wifi_sync_menu_if_visible();
        return ESP_OK;
    }
    if (wifi_controller_get_state() == WIFI_CONTROLLER_CONNECTING)
    {
        /* A request already in flight is idempotent; a FAILED state remains
         * retryable so the user can correct a password and try again. */
        return ESP_OK;
    }
    if (wifi_manager_get_mode() == WIFI_MODE_AP)
    {
        lcd_flash_message("AP Mode", "No STA Connect", 1400U);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!sys_state.wifi.enabled)
    {
        lcd_flash_message("Wi-Fi Disabled", "Enable first", 1400U);
        return ESP_ERR_INVALID_STATE;
    }
    wifi_manager_config_t config = {0};
    if (wifi_manager_get_config(&config) != ESP_OK || config.ssid[0] == '\0')
    {
        lcd_flash_message("Wi-Fi Not Config", "Scan a network", 1800U);
        return ESP_ERR_NOT_FOUND;
    }
    /*
     * This path is invoked from the Wi-Fi menu's Connect item. Keep that menu
     * visible so its action label can animate in place (Connecting. / .. / ...)
     * and then change to Disconnect or Connect without a button press.
     * The separate full-screen Wi-Fi connecting view is reserved for scan/
     * password-driven connections where the user has left the menu.
     */
    app_wifi_begin_operation(APP_WIFI_OPERATION_CONNECT_SAVED, config.ssid, -127);
    esp_err_t err = wifi_controller_reconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
    {
        app_wifi_end_operation();
        app_wifi_sync_menu_if_visible();
        ESP_LOGW(APP_SERVICES_TAG, "Saved Wi-Fi reconnect request failed: %s",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_services_wifi_disconnect(void)
{
    /* A disconnect request is idempotent when STA is already disconnected. */
    if (!wifi_controller_is_connected())
    {
        app_wifi_end_operation();
        app_wifi_sync_menu_if_visible();
        return ESP_OK;
    }

    app_wifi_begin_operation(APP_WIFI_OPERATION_DISCONNECT, NULL, -127);
    const esp_err_t err = wifi_controller_disconnect();
    if (err == ESP_ERR_WIFI_NOT_CONNECT || err == ESP_ERR_WIFI_NOT_STARTED)
    {
        app_wifi_end_operation();
        app_wifi_sync_menu_if_visible();
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        app_wifi_end_operation();
        lcd_show_wifi_result(false, true, false, "", -127, "Disconnect failed");
    }
    /* Otherwise the watcher confirms the live station state and ends the
     * operation after the minimum visible time. */
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
    if (wifi_manager_get_mode() == WIFI_MODE_AP)
    {
        strncpy(ssid, config.ap_ssid, sizeof(ssid) - 1U);
    }
    else
    {
        strncpy(ssid, config.ssid, sizeof(ssid) - 1U);
    }
    if (ssid[0] == '\0')
    {
        strncpy(ssid, "Not configured", sizeof(ssid) - 1U);
    }

    char ip[16] = "0.0.0.0";
    char gateway[16] = "0.0.0.0";
    if (status.got_ip)
    {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&status.ip));
        snprintf(gateway, sizeof(gateway), IPSTR, IP2STR(&status.gateway));
    }

    char state_text[LCD_LINE_SIZE] = {0};
    if (!sys_state.wifi.enabled)
    {
        snprintf(state_text, sizeof(state_text), "Wi-Fi OFF");
    }
    else if (wifi_manager_get_mode() == WIFI_MODE_AP)
    {
        snprintf(state_text, sizeof(state_text), "AP active");
    }
    else
    {
        snprintf(state_text, sizeof(state_text), "%s",
                 wifi_state_text(wifi_controller_get_state()));
    }
    const int8_t rssi = status.got_ip ? wifi_manager_get_rssi() : status.rssi;
    lcd_show_wifi_status(state_text, ssid, ip, gateway, rssi,
                         status.connected, status.got_ip,
                         status.connected && status.internet_available);
}

const char *app_services_wifi_saved_network_label(void)
{
    static char label[LCD_LINE_SIZE];
    wifi_credentials_t credentials = {0};
    if (wifi_storage_load_credentials(&credentials) == ESP_OK &&
        credentials.ssid[0] != '\0')
    {
        snprintf(label, sizeof(label), "Saved: %.9s", credentials.ssid);
    }
    else
    {
        snprintf(label, sizeof(label), "Saved: None");
    }
    return label;
}

bool app_services_wifi_forget_confirmation_pending(void)
{
    bool pending = false;
    if (s_services_mutex != NULL)
    {
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
        credentials.ssid[0] == '\0')
    {
        lcd_flash_message("No Saved Network", "Nothing to forget", 1400U);
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_forget_pending = true;
    xSemaphoreGive(s_services_mutex);
    lcd_show_confirm("Forget network?", "Confirm removal?");
    return ESP_OK;
}

esp_err_t app_services_wifi_confirm_forget_saved(void)
{
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_forget_pending = false;
    xSemaphoreGive(s_services_mutex);

    esp_err_t err = wifi_storage_erase_credentials();
    if (err != ESP_OK)
    {
        lcd_flash_message("Forget failed", "Try again", 1400U);
        return err;
    }

    wifi_manager_config_t config = {0};
    if (wifi_manager_get_config(&config) == ESP_OK)
    {
        config.ssid[0] = '\0';
        config.password[0] = '\0';
        (void)wifi_manager_set_config(&config);
    }
    if (wifi_controller_is_connected())
    {
        (void)wifi_controller_disconnect();
    }
    lcd_flash_message("Network Forgotten", "Wi-Fi ready", 1200U);
    return ESP_OK;
}

void app_services_wifi_cancel_forget_saved(void)
{
    if (s_services_mutex != NULL)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_wifi_forget_pending = false;
        xSemaphoreGive(s_services_mutex);
    }
}

bool app_services_wifi_disconnect_confirmation_pending(void)
{
    bool pending = false;
    if (s_services_mutex != NULL)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        pending = s_wifi_disconnect_pending;
        xSemaphoreGive(s_services_mutex);
    }
    return pending;
}

esp_err_t app_services_wifi_request_disconnect(void)
{
    if (!wifi_controller_is_connected())
    {
        show_menu_screen(MENU_WIFI_CONFIG, sys_state.menu_selection);
        return ESP_OK;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_disconnect_pending = true;
    xSemaphoreGive(s_services_mutex);
    lcd_show_confirm("Disconnect Wi-Fi?", "Confirm disconnect?");
    return ESP_OK;
}

esp_err_t app_services_wifi_confirm_disconnect(void)
{
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_disconnect_pending = false;
    xSemaphoreGive(s_services_mutex);

    /* Dismiss the confirmation unconditionally. The radio may have already
     * dropped its association before the controller's completion callback,
     * so gating this navigation on operation_pending can strand the dialog. */
    const esp_err_t err = app_services_wifi_disconnect();
    show_menu_screen(MENU_WIFI_CONFIG, sys_state.menu_selection);
    return err;
}

void app_services_wifi_cancel_disconnect(void)
{
    if (s_services_mutex != NULL)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_wifi_disconnect_pending = false;
        xSemaphoreGive(s_services_mutex);
    }
}

bool app_services_wifi_dhcp_enabled(void)
{
    if (wifi_manager_get_mode() == WIFI_MODE_AP)
    {
        return wifi_manager_ap_dhcp_enabled();
    }

    wifi_manager_config_t config = {0};
    return wifi_manager_get_config(&config) == ESP_OK ? config.dhcp : true;
}

esp_err_t app_services_wifi_toggle_dhcp(void)
{
    wifi_manager_config_t config = {0};
    esp_err_t err = wifi_manager_get_config(&config);
    if (err != ESP_OK)
    {
        return err;
    }

    const bool ap_mode = config.mode == WIFI_MODE_AP;
    const bool new_value = ap_mode ? !config.ap_dhcp : !config.dhcp;

    if (ap_mode)
    {
        err = wifi_manager_set_ap_dhcp(new_value);
        if (err != ESP_OK)
        {
            return err;
        }
        config.ap_dhcp = new_value;
    }
    else
    {
        config.dhcp = new_value;
        err = wifi_manager_set_config(&config);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    wifi_network_config_t stored = {0};
    if (wifi_storage_load_network_config(&stored) != ESP_OK)
    {
        wifi_storage_set_default_network_config(&stored);
    }
    if (ap_mode)
    {
        stored.ap_dhcp = new_value;
    }
    else
    {
        stored.dhcp = new_value;
        stored.ip_info = config.ip_info;
        stored.dns = config.dns;
    }
    err = wifi_storage_save_network_config(&stored);
    if (err == ESP_OK)
    {
        lcd_flash_message(new_value ? "DHCP ON" : "DHCP OFF",
                          ap_mode ? "AP DHCP" : (wifi_controller_is_connected() ? "Reconnect needed" : "Saved"),
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
    if (err != ESP_OK && err != ESP_ERR_INVALID_SIZE)
    {
        return err;
    }
    if (index >= count)
    {
        return ESP_ERR_NOT_FOUND;
    }
    return app_services_disconnect_ap_client(clients[index].mac);
}

void app_services_show_ap_clients(void)
{
    if (!app_services_wifi_can_manage_clients())
    {
        lcd_flash_message("AP Clients", "AP mode required", 1400U);
        return;
    }
    if (!sys_state.wifi.enabled)
    {
        /* AP mode can be selected while the radio is OFF: this is not a
         * system fault and must not eject the user from the Wi-Fi menu. */
        lcd_flash_message("Wi-Fi is OFF", "Turn Wi-Fi ON", 1400U);
        return;
    }
    wifi_ap_client_info_t clients[WIFI_AP_MAX_CLIENTS] = {0};
    char macs[WIFI_AP_MAX_CLIENTS][18] = {{0}};
    size_t count = 0U;
    const esp_err_t err = app_services_get_ap_clients(clients, WIFI_AP_MAX_CLIENTS, &count);
    if (err != ESP_OK && err != ESP_ERR_INVALID_SIZE)
    {
        /* Radio startup/offline errors are recoverable UI conditions, not a
         * global system fault. Keep the user in the Wi-Fi menu. */
        ESP_LOGW(APP_SERVICES_TAG, "Could not read AP clients: %s",
                 esp_err_to_name(err));
        lcd_flash_message("AP Clients Unavailable", "Check Wi-Fi state", 1400U);
        return;
    }
    for (size_t i = 0U; i < count; ++i)
    {
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

/* Effective manifest URL: the stored runtime URL when it is valid, otherwise
 * the compile-time default. The default must not depend on app_services_init()
 * having run or on the NVS provisioning write having succeeded; previously a
 * failed/skipped write left s_manifest_url empty and the check reported
 * "OTA URL Invalid" although a valid default was configured. Caller must NOT
 * hold s_services_mutex. */
static esp_err_t app_ota_effective_manifest_url(char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_services_mutex != NULL)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    }
    const char *chosen = NULL;
    if (s_manifest_url[0] != '\0' && app_manifest_url_is_valid(s_manifest_url))
    {
        chosen = s_manifest_url;
    }
    else if (APP_OTA_DEFAULT_MANIFEST_URL[0] != '\0' &&
             app_manifest_url_is_valid(APP_OTA_DEFAULT_MANIFEST_URL))
    {
        chosen = APP_OTA_DEFAULT_MANIFEST_URL;
    }
    esp_err_t err = ESP_OK;
    if (chosen == NULL)
    {
        buffer[0] = '\0';
    }
    else if (strlen(chosen) >= buffer_len)
    {
        err = ESP_ERR_INVALID_SIZE;
    }
    else
    {
        strcpy(buffer, chosen);
    }
    if (s_services_mutex != NULL)
    {
        xSemaphoreGive(s_services_mutex);
    }
    return err;
}

esp_err_t app_services_get_ota_manifest_url(char *buffer, size_t buffer_len)
{
    return app_ota_effective_manifest_url(buffer, buffer_len);
}

typedef struct
{
    char manifest_url[APP_OTA_MANIFEST_URL_MAX];
    bool user_initiated;
} ota_manifest_check_job_t;

static void ota_manifest_check_task(void *parameter)
{
    ota_manifest_check_job_t *job = (ota_manifest_check_job_t *)parameter;
    if (!job)
    {
        vTaskDelete(NULL);
        return;
    }

    ota_manifest_entry_t entry = {0};
    bool update_available = false;
    /* The manifest fetch is a full TLS session (about 17 KB contiguous for one
     * record plus roughly 10 KB for the handshake / RSA bignums). With the
     * HTTP/WebSocket/dashboard/mDNS services up, only ~15 KB was free and the
     * handshake failed with "alloc failed" (mbedTLS -0x0010). Free that memory
     * for the few seconds the check takes, but only when the heap is actually
     * too tight so roomier builds are not interrupted. */
    const bool suspended_for_check =
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 40000U;
    if (suspended_for_check)
    {
        (void)network_services_suspend_for_ota();
    }
    const esp_err_t err = ota_service_check_csv_manifest(job->manifest_url,
                                                         &entry,
                                                         &update_available);
    if (suspended_for_check)
    {
        network_services_resume_after_ota();
    }
    bool cancelled = false;
    app_ota_status_t snapshot;
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    cancelled = s_ota_check_cancel_requested;
    s_ota_check_cancel_requested = false;
    if (cancelled)
    {
        s_ota_status.state = APP_OTA_CANCELLED;
        s_ota_status.progress_percent = 0;
    }
    else if (err == ESP_OK && update_available)
    {
        s_ota_status.state = APP_OTA_AVAILABLE;
        s_ota_status.error_detail[0] = '\0';
        s_ota_status.update_available = true;
        strncpy(s_ota_status.available_version, entry.version,
                sizeof(s_ota_status.available_version) - 1U);
    }
    else if (err == ESP_OK)
    {
        s_ota_status.state = APP_OTA_IDLE;
        s_ota_status.error_detail[0] = '\0';
        s_ota_status.update_available = false;
        s_ota_status.available_version[0] = '\0';
    }
    else
    {
        s_ota_status.state = APP_OTA_ERROR;
        snprintf(s_ota_status.error_detail, sizeof(s_ota_status.error_detail),
                 "ERR:%s", esp_err_to_name(err));
        /* Preserve a previously valid available manifest so Install/Retry can
         * still begin a clean, freshly downloaded OTA transaction. */
    }
    snapshot = s_ota_status;
    s_ota_manifest_check_task = NULL;
    s_ota_manifest_check_active = false;
    /* A "Cancel update?" prompt for this check is moot once it has ended;
     * a stale flag would swallow the next physical Back press. */
    s_ota_cancel_confirmation_pending = false;
    xSemaphoreGive(s_services_mutex);

    if (err != ESP_OK && !cancelled)
    {
        ESP_LOGW(APP_SERVICES_TAG, "OTA check failed: %s", esp_err_to_name(err));
    }
    if (job->user_initiated || update_available || cancelled)
    {
        app_services_show_ota_lcd(&snapshot);
    }
    memset(job, 0, sizeof(*job));
    free(job);
    vTaskDelete(NULL);
}

typedef struct
{
    char manifest_url[APP_OTA_MANIFEST_URL_MAX];
    char staged_version[OTA_MAX_VERSION_LENGTH];
} ota_pending_manifest_job_t;

static void ota_pending_manifest_check_task(void *parameter)
{
    ota_pending_manifest_job_t *job = (ota_pending_manifest_job_t *)parameter;
    if (job == NULL)
    {
        if (s_services_mutex != NULL)
        {
            xSemaphoreTake(s_services_mutex, portMAX_DELAY);
            s_pending_manifest_check_active = false;
            s_pending_manifest_check_task = NULL;
            xSemaphoreGive(s_services_mutex);
        }
        vTaskDelete(NULL);
        return;
    }

    /* Hash verification runs in this worker, never in the button/LCD path.
     * The button path is held until this preflight has completed. */
    const esp_err_t verify_err = ota_service_pending_image_verified()
                                     ? ESP_OK
                                     : ota_service_validate_pending_for_apply();
    ota_manifest_entry_t entry = {0};
    bool latest_newer_than_running = false;
    esp_err_t check_err = ESP_ERR_INVALID_STATE;
    if (verify_err == ESP_OK && job->manifest_url[0] != '\0')
    {
        const bool suspended_for_check =
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 40000U;
        if (suspended_for_check)
        {
            (void)network_services_suspend_for_ota();
        }
        check_err = ota_service_check_csv_manifest(
            job->manifest_url, &entry, &latest_newer_than_running);
        if (suspended_for_check)
        {
            network_services_resume_after_ota();
        }
    }

    bool staged_is_older = false;
    if (verify_err == ESP_OK && check_err == ESP_OK)
    {
        staged_is_older =
            ota_manifest_compare_versions(entry.version, job->staged_version) > 0;
    }

    esp_err_t discard_err = ESP_OK;
    if (staged_is_older)
    {
        ESP_LOGW(APP_SERVICES_TAG,
                 "Latest release %s is newer than staged %s; discarding stale staged image",
                 entry.version, job->staged_version);
        discard_err = ota_service_delete_pending();
    }

    app_ota_status_t snapshot = {0};
    bool show_status = false;
    if (s_services_mutex != NULL)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_pending_manifest_check_active = false;
        s_pending_manifest_check_task = NULL;
        /* One manifest attempt per staged image/session avoids repeated TLS
         * handshakes when the user authenticates and returns to the prompt. */
        s_pending_manifest_checked =
            verify_err == ESP_OK && job->manifest_url[0] != '\0';
        if (staged_is_older && discard_err == ESP_OK)
        {
            s_pending_manifest_checked = true;
        }
        if (verify_err != ESP_OK && !ota_service_update_pending())
        {
            s_ota_status.state = APP_OTA_IDLE;
            s_ota_status.update_available = false;
            s_ota_status.confirmation_pending = false;
            s_ota_status.progress_percent = 0;
            s_ota_status.available_version[0] = '\0';
            (void)snprintf(s_ota_status.error_detail,
                           sizeof(s_ota_status.error_detail), "%s",
                           "Staged image rejected");
            snapshot = s_ota_status;
            show_status = true;
        }
        else if (staged_is_older && discard_err == ESP_OK)
        {
            s_ota_status.state = APP_OTA_AVAILABLE;
            s_ota_status.update_available = latest_newer_than_running;
            s_ota_status.confirmation_pending = false;
            s_ota_status.progress_percent = 0;
            (void)snprintf(s_ota_status.available_version,
                           sizeof(s_ota_status.available_version), "%s",
                           entry.version);
            s_ota_status.error_detail[0] = '\0';
            snapshot = s_ota_status;
            show_status = true;
        }
        xSemaphoreGive(s_services_mutex);
    }

    if (verify_err != ESP_OK)
    {
        ESP_LOGE(APP_SERVICES_TAG,
                 "Staged OTA preflight failed for %s: %s; current firmware retained",
                 job->staged_version, esp_err_to_name(verify_err));
    }
    else if (job->manifest_url[0] == '\0')
    {
        ESP_LOGI(APP_SERVICES_TAG,
                 "Staged OTA %s passed SHA-256 preflight; manifest check skipped (offline)",
                 job->staged_version);
    }
    else if (check_err != ESP_OK)
    {
        ESP_LOGW(APP_SERVICES_TAG,
                 "Could not compare staged OTA %s with latest manifest: %s; verified staged image retained",
                 job->staged_version, esp_err_to_name(check_err));
    }
    else if (!staged_is_older)
    {
        ESP_LOGI(APP_SERVICES_TAG,
                 "Staged OTA %s passed preflight and is not older than manifest %s",
                 job->staged_version, entry.version);
    }
    else if (discard_err != ESP_OK)
    {
        ESP_LOGE(APP_SERVICES_TAG,
                 "Could not discard stale staged OTA %s: %s",
                 job->staged_version, esp_err_to_name(discard_err));
    }

    if (show_status)
    {
        app_services_show_ota_lcd_impl(&snapshot, false);
        if (staged_is_older && discard_err == ESP_OK)
        {
            ESP_LOGI(APP_SERVICES_TAG,
                     "Latest OTA %s is available; existing confirmation flow will download it",
                     entry.version);
        }
    }

    memset(job, 0, sizeof(*job));
    free(job);
    vTaskDelete(NULL);
}

static void app_services_schedule_pending_manifest_check(void)
{
    if (s_services_mutex == NULL)
    {
        return;
    }

    char staged_version[OTA_MAX_VERSION_LENGTH] = {0};
    if (ota_service_get_pending_version(staged_version,
                                        sizeof(staged_version)) != ESP_OK)
    {
        return;
    }

    const bool station_connected = app_wifi_station_has_ip();
    if (ota_service_pending_image_verified() &&
        (s_pending_manifest_checked || !station_connected))
    {
        return;
    }

    /* Integrity preflight always runs, even offline. The optional latest
     * manifest comparison only runs when station connectivity is available. */
    char manifest_url[APP_OTA_MANIFEST_URL_MAX] = {0};
    if (station_connected)
    {
        if (app_services_get_ota_manifest_url(manifest_url,
                                              sizeof(manifest_url)) != ESP_OK ||
            !ota_manifest_is_https_url(manifest_url))
        {
            manifest_url[0] = '\0';
        }
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    if (s_pending_manifest_check_active ||
        s_ota_status.state != APP_OTA_PENDING_APPLY)
    {
        xSemaphoreGive(s_services_mutex);
        return;
    }

    ota_pending_manifest_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL)
    {
        xSemaphoreGive(s_services_mutex);
        ESP_LOGW(APP_SERVICES_TAG,
                 "Could not allocate staged OTA preflight; image remains blocked from apply");
        return;
    }
    (void)snprintf(job->manifest_url, sizeof(job->manifest_url), "%s",
                   manifest_url);
    (void)snprintf(job->staged_version, sizeof(job->staged_version), "%s",
                   staged_version);
    s_pending_manifest_check_active = true;
    xSemaphoreGive(s_services_mutex);

    if (xTaskCreatePinnedToCore(ota_pending_manifest_check_task,
                                "ota_pending_check",
                                APP_OTA_CHECK_TASK_STACK_SIZE, job,
                                APP_OTA_CHECK_TASK_PRIORITY,
                                &s_pending_manifest_check_task,
                                APP_CORE_SYSTEM) != pdPASS)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_pending_manifest_check_active = false;
        s_pending_manifest_check_task = NULL;
        xSemaphoreGive(s_services_mutex);
        memset(job, 0, sizeof(*job));
        free(job);
        ESP_LOGW(APP_SERVICES_TAG,
                 "Could not start staged OTA preflight; image remains blocked from apply");
    }
}

esp_err_t app_services_check_for_update(bool user_initiated)
{
    char manifest_url[APP_OTA_MANIFEST_URL_MAX] = {0};
    if (s_services_mutex == NULL)
    {
        ESP_LOGW(APP_SERVICES_TAG,
                 "Update check requested before app services initialization");
        return ESP_ERR_INVALID_STATE;
    }
    if (!app_wifi_station_has_ip())
    {
        if (user_initiated && !app_services_wifi_enabled())
        {
            xSemaphoreTake(s_services_mutex, portMAX_DELAY);
            s_wifi_prerequisite_operation = APP_WIFI_PREREQUISITE_OTA_CHECK;
            s_wifi_prerequisite_pending = true;
            s_wifi_prerequisite_waiting = false;
            s_wifi_prerequisite_ready = false;
            xSemaphoreGive(s_services_mutex);
            lcd_show_confirm("Wi-Fi is OFF", "Enable to continue");
            ESP_LOGI(APP_SERVICES_TAG,
                     "OTA check paused: Wi-Fi is OFF; prerequisite prompt shown");
            return ESP_ERR_INVALID_STATE;
        }
        if (user_initiated)
        {
            lcd_flash_warning_to("Wi-Fi ON", "Connect first",
                                 1800U, LCD_SCREEN_MENU);
        }
        ESP_LOGW(APP_SERVICES_TAG, "Update check skipped: station is not connected");
        return ESP_ERR_INVALID_STATE;
    }
    /* The monitor's TCP reachability result can lag a newly acquired DHCP
     * lease. Let the actual HTTPS manifest request validate DNS/TLS/endpoint
     * reachability instead of rejecting a user check on a stale probe. */
    if (!wifi_monitor_is_online())
    {
        ESP_LOGW(APP_SERVICES_TAG,
                 "Internet probe is not yet positive; attempting HTTPS manifest check");
    }
    const esp_err_t url_err =
        app_services_get_ota_manifest_url(manifest_url, sizeof(manifest_url));
    if (url_err != ESP_OK || !ota_manifest_is_https_url(manifest_url))
    {
        ESP_LOGE(APP_SERVICES_TAG,
                 "OTA manifest URL unavailable or invalid (get=%s, https_valid=%d)",
                 esp_err_to_name(url_err),
                 ota_manifest_is_https_url(manifest_url) ? 1 : 0);
        app_services_record_ota_error(
            url_err == ESP_OK ? "OTA URL Invalid" : "Manifest URL unavailable",
            user_initiated);
        return url_err == ESP_OK ? ESP_ERR_INVALID_ARG : url_err;
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool busy = s_ota_manifest_check_active ||
                      s_ota_status.state == APP_OTA_PREPARING ||
                      s_ota_status.state == APP_OTA_DOWNLOADING ||
                      s_ota_status.state == APP_OTA_VERIFYING ||
                      s_ota_status.state == APP_OTA_CHECKING;
    if (busy)
    {
        xSemaphoreGive(s_services_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    ota_manifest_check_job_t *job = calloc(1, sizeof(*job));
    if (!job)
    {
        xSemaphoreGive(s_services_mutex);
        app_services_record_ota_error("ERR:NO_MEM", user_initiated);
        return ESP_ERR_NO_MEM;
    }
    strncpy(job->manifest_url, manifest_url, sizeof(job->manifest_url) - 1U);
    job->user_initiated = user_initiated;
    s_ota_check_cancel_requested = false;
    s_ota_status.state = APP_OTA_CHECKING;
    s_ota_status.progress_percent = 0;
    s_ota_manifest_check_active = true;
    xSemaphoreGive(s_services_mutex);

    if (xTaskCreatePinnedToCore(ota_manifest_check_task, "ota_manifest_check",
                                APP_OTA_CHECK_TASK_STACK_SIZE, job,
                                APP_OTA_CHECK_TASK_PRIORITY, &s_ota_manifest_check_task, APP_CORE_SYSTEM) != pdPASS)
    {
        app_ota_status_t failed_status;
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_ota_manifest_check_task = NULL;
        s_ota_manifest_check_active = false;
        s_ota_status.state = APP_OTA_ERROR;
        (void)snprintf(s_ota_status.error_detail,
                       sizeof(s_ota_status.error_detail), "ERR:NO_MEM");
        failed_status = s_ota_status;
        xSemaphoreGive(s_services_mutex);
        memset(job, 0, sizeof(*job));
        free(job);
        if (user_initiated)
        {
            app_services_show_ota_lcd(&failed_status);
        }
        return ESP_ERR_NO_MEM;
    }
    app_ota_status_t snapshot;
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    snapshot = s_ota_status;
    xSemaphoreGive(s_services_mutex);
    if (user_initiated)
    {
        app_services_show_ota_lcd(&snapshot);
    }
    return ESP_OK;
}

bool app_services_wifi_prerequisite_pending(void)
{
    if (s_services_mutex == NULL)
    {
        return false;
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool pending = s_wifi_prerequisite_pending;
    xSemaphoreGive(s_services_mutex);
    return pending;
}

esp_err_t app_services_confirm_wifi_prerequisite(void)
{
    if (s_services_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    app_wifi_prerequisite_operation_t operation;
    bool pending;

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    operation = s_wifi_prerequisite_operation;
    pending = s_wifi_prerequisite_pending;
    if (pending)
    {
        s_wifi_prerequisite_pending = false;
        s_wifi_prerequisite_waiting = true;
        s_wifi_prerequisite_ready = false;
        s_wifi_prerequisite_started_tick = xTaskGetTickCount();
    }
    xSemaphoreGive(s_services_mutex);

    if (!pending || operation == APP_WIFI_PREREQUISITE_NONE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = app_services_set_wifi_enabled(true);
    if (err != ESP_OK)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_wifi_prerequisite_operation = APP_WIFI_PREREQUISITE_NONE;
        s_wifi_prerequisite_pending = false;
        s_wifi_prerequisite_waiting = false;
        s_wifi_prerequisite_ready = false;
        xSemaphoreGive(s_services_mutex);

        lcd_flash_message("Wi-Fi Start Failed", "Try again", 1500U);
        return err;
    }

    lcd_flash_message("Wi-Fi STARTING", "Please wait", 1200U);
    ESP_LOGI(APP_SERVICES_TAG,
             "Wi-Fi prerequisite accepted for operation=%d; waiting for readiness",
             (int)operation);
    return ESP_OK;
}

void app_services_cancel_wifi_prerequisite(void)
{
    if (s_services_mutex == NULL)
    {
        return;
    }

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_wifi_prerequisite_operation = APP_WIFI_PREREQUISITE_NONE;
    s_wifi_prerequisite_pending = false;
    s_wifi_prerequisite_waiting = false;
    s_wifi_prerequisite_ready = false;
    xSemaphoreGive(s_services_mutex);

    lcd_flash_message("Operation Cancelled", "Wi-Fi unchanged", 1200U);
}

esp_err_t app_services_request_update_confirmation(void)
{
    if (s_services_mutex == NULL)
    {
        ESP_LOGW(APP_SERVICES_TAG,
                 "Update confirmation requested before app services initialization");
        return ESP_ERR_INVALID_STATE;
    }
    if (!app_wifi_station_has_ip() && !app_services_wifi_enabled())
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_wifi_prerequisite_operation = APP_WIFI_PREREQUISITE_OTA_INSTALL;
        s_wifi_prerequisite_pending = true;
        s_wifi_prerequisite_waiting = false;
        s_wifi_prerequisite_ready = false;
        xSemaphoreGive(s_services_mutex);
        lcd_show_confirm("Wi-Fi is OFF", "Enable to continue");
        ESP_LOGI(APP_SERVICES_TAG,
                 "OTA install paused: Wi-Fi is OFF; prerequisite prompt shown");
        return ESP_ERR_INVALID_STATE;
    }

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
    lcd_show_confirm("Install Update?", "Confirm install?");
    return ESP_OK;
}

esp_err_t app_services_confirm_update(void)
{
    /* If a previously verified image was deferred, the existing Install Update
     * menu item resumes that staged image instead of downloading it again. */
    char pending_version[OTA_MAX_VERSION_LENGTH] = {0};
    if (ota_service_get_pending_version(pending_version,
                                        sizeof(pending_version)) == ESP_OK)
    {
        if (s_services_mutex == NULL)
            return ESP_ERR_INVALID_STATE;
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_ota_status.state = APP_OTA_PENDING_APPLY;
        s_ota_status.update_available = true;
        s_ota_status.progress_percent = 100;
        snprintf(s_ota_status.available_version,
                 sizeof(s_ota_status.available_version), "%s", pending_version);
        app_ota_status_t pending_snapshot = s_ota_status;
        xSemaphoreGive(s_services_mutex);
        app_services_show_ota_lcd(&pending_snapshot);
        s_ota_prompt_last_shown_ms =
            (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        s_ota_prompt_from_boot_reminder = false;
        if (s_ota_apply_prompt_task == NULL &&
            xTaskCreate(ota_apply_prompt_timeout_task, "ota_apply_prompt",
                        3072U, NULL, 3U, &s_ota_apply_prompt_task) != pdPASS)
        {
            s_ota_apply_prompt_task = NULL;
            ESP_LOGW(APP_SERVICES_TAG, "Could not restart OTA apply-prompt timeout");
        }
        return ESP_OK;
    }

    char manifest_url[APP_OTA_MANIFEST_URL_MAX] = {0};
    app_ota_status_t snapshot = {0};
    if (s_services_mutex == NULL)
    {
        ESP_LOGW(APP_SERVICES_TAG,
                 "Update confirmation received before app services initialization");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool confirmed = s_ota_status.confirmation_pending;
    if (confirmed)
    {
        s_ota_status.confirmation_pending = false;
        s_ota_status.cancel_confirmation_pending = false;
        s_ota_status.state = APP_OTA_PREPARING;
        s_ota_status.progress_percent = 0;
    }
    snapshot = s_ota_status;
    xSemaphoreGive(s_services_mutex);

    if (confirmed)
    {
        (void)app_ota_effective_manifest_url(manifest_url, sizeof(manifest_url));
    }
    if (!confirmed)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (manifest_url[0] == '\0')
    {
        app_services_record_ota_error("OTA URL Invalid", true);
        return ESP_ERR_INVALID_ARG;
    }
    /* Require a station IP, as the update check does. The monitor's internet
     * probe can lag a fresh DHCP lease or fail while HTTPS still works, and
     * used to block a confirmed install; the HTTPS request itself validates
     * reachability and reports a real error. */
    if (!app_wifi_station_has_ip())
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_ota_status.confirmation_pending = false;
        s_ota_status.state = APP_OTA_AVAILABLE;
        xSemaphoreGive(s_services_mutex);
        lcd_flash_warning_to("Wi-Fi ON", "Connect first",
                             1800U, LCD_SCREEN_MENU);
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_monitor_is_online())
    {
        ESP_LOGW(APP_SERVICES_TAG,
                 "Internet probe is not yet positive; attempting HTTPS install");
    }
    /* TLS needs a sufficiently large contiguous internal-RAM block. Abort
     * before queueing OTA if the measured safety floor is not available. */
    const size_t ota_largest_internal =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(APP_SERVICES_TAG, "Pre-OTA largest internal block: %u bytes",
             (unsigned)ota_largest_internal);
    if (ota_largest_internal < 20000U)
    {
        xSemaphoreTake(s_services_mutex, portMAX_DELAY);
        s_ota_status.state = APP_OTA_AVAILABLE;
        s_ota_status.progress_percent = 0;
        xSemaphoreGive(s_services_mutex);
        lcd_flash_warning_to("Low memory", "Close apps; retry",
                             2200U, LCD_SCREEN_MENU);
        return ESP_ERR_NO_MEM;
    }
    /* Measured stack headroom of every live task, taken at the busiest
     * moment (Wi-Fi up, menus used, about to open a TLS session). */
    stack_report_log("before OTA install");
    /* The TLS download needs ~17 KB of contiguous heap. With the local
     * HTTP/WebSocket/mDNS/NTP services running only ~40 KB is free and
     * fragmented, and the download aborts with "alloc(16749) failed".
     * Suspend them for the transfer; they are resumed on failure/cancel
     * (a successful update reboots into the new image). */
    (void)network_services_suspend_for_ota();
    const esp_err_t err = ota_service_start_from_csv(manifest_url);
    if (err != ESP_OK)
    {
        network_services_resume_after_ota();
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
    if (s_services_mutex == NULL)
    {
        ESP_LOGW(APP_SERVICES_TAG,
                 "Cancel update requested before app services initialization");
        return ESP_ERR_INVALID_STATE;
    }
    const bool service_busy = ota_service_in_progress();
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool check_busy = s_ota_manifest_check_active;
    if (service_busy || check_busy)
    {
        s_ota_cancel_confirmation_pending = true;
    }
    xSemaphoreGive(s_services_mutex);
    if (!service_busy && !check_busy)
    {
        lcd_flash_message("No Update Running", "Current kept", 1200U);
        return ESP_ERR_INVALID_STATE;
    }
    lcd_show_confirm("Cancel update?", "Confirm cancel?");
    return ESP_OK;
}

esp_err_t app_services_confirm_cancel_update(void)
{
    if (s_services_mutex == NULL)
    {
        ESP_LOGW(APP_SERVICES_TAG,
                 "Cancel confirmation received before app services initialization");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool pending = s_ota_cancel_confirmation_pending;
    s_ota_cancel_confirmation_pending = false;
    xSemaphoreGive(s_services_mutex);
    if (!pending)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return app_services_cancel_update();
}

void app_services_cancel_cancel_update(void)
{
    if (s_services_mutex == NULL)
    {
        return;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_ota_cancel_confirmation_pending = false;
    xSemaphoreGive(s_services_mutex);
}

/* Forget a previous OTA failure so the next check starts clean: the error
 * state (which turns the menu item into "Retry") and its detail are dropped.
 * Only an ERROR state is touched; a running check/install, an available
 * update or a completed update is left exactly as it is. */
void app_services_ota_clear_error(void)
{
    if (s_services_mutex == NULL)
    {
        return;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    if (s_ota_status.state == APP_OTA_ERROR)
    {
        s_ota_status.state = APP_OTA_IDLE;
        s_ota_status.error_detail[0] = '\0';
        s_ota_status.progress_percent = 0;
    }
    xSemaphoreGive(s_services_mutex);
}

bool app_services_ota_cancel_confirmation_pending(void)
{
    if (s_services_mutex == NULL)
    {
        return false;
    }
    bool pending = false;
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    pending = s_ota_cancel_confirmation_pending;
    xSemaphoreGive(s_services_mutex);
    return pending;
}

bool app_services_update_pending(void)
{
    return ota_service_update_pending();
}

bool app_services_pending_manifest_check_active(void)
{
    if (s_services_mutex == NULL)
        return false;
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool active = s_pending_manifest_check_active;
    xSemaphoreGive(s_services_mutex);
    return active;
}

esp_err_t app_services_apply_pending_update(void)
{
    return ota_service_select_pending_for_boot();
}

esp_err_t app_services_defer_pending_update(void)
{
    if (!ota_service_update_pending())
        return ESP_ERR_NOT_FOUND;
    if (s_services_mutex == NULL)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_ota_status.state = APP_OTA_AVAILABLE;
    s_ota_status.update_available = true;
    s_ota_status.confirmation_pending = false;
    s_ota_status.cancel_confirmation_pending = false;
    s_ota_status.progress_percent = 100;
    xSemaphoreGive(s_services_mutex);
    return ESP_OK;
}

esp_err_t app_services_show_pending_delete_confirmation(void)
{
    if (!ota_service_update_pending())
        return ESP_ERR_NOT_FOUND;
    if (s_services_mutex == NULL)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_ota_status.state = APP_OTA_DELETE_CONFIRM;
    app_ota_status_t snapshot = s_ota_status;
    xSemaphoreGive(s_services_mutex);
    app_services_show_ota_lcd_impl(&snapshot, false);
    return ESP_OK;
}

esp_err_t app_services_delete_pending_update(void)
{
    if (s_services_mutex == NULL)
        return ESP_ERR_INVALID_STATE;
    const esp_err_t err = ota_service_delete_pending();
    /* If the marker is still present, deletion did not safely detach the
     * image; retain the prompt so the user can retry. If only physical erase
     * failed after the marker was cleared, forget the staged image anyway. */
    if (err != ESP_OK && ota_service_update_pending())
        return err;

    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    s_pending_manifest_checked = false;
    s_ota_status.state = APP_OTA_IDLE;
    s_ota_status.progress_percent = 0;
    s_ota_status.update_available = false;
    s_ota_status.confirmation_pending = false;
    s_ota_status.cancel_confirmation_pending = false;
    s_ota_status.available_version[0] = '\0';
    s_ota_status.error_detail[0] = '\0';
    xSemaphoreGive(s_services_mutex);
    return err;
}

esp_err_t app_services_cancel_update(void)
{
    if (s_services_mutex == NULL)
    {
        ESP_LOGW(APP_SERVICES_TAG,
                 "Cancel update requested before app services initialization");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_services_mutex, portMAX_DELAY);
    const bool pending = s_ota_status.confirmation_pending;
    const bool checking = s_ota_manifest_check_active;
    s_ota_status.confirmation_pending = false;
    s_ota_cancel_confirmation_pending = false;
    if (pending)
    {
        s_ota_status.state = APP_OTA_AVAILABLE;
    }
    if (checking)
    {
        s_ota_check_cancel_requested = true;
        s_ota_status.state = APP_OTA_CANCELLED;
    }
    xSemaphoreGive(s_services_mutex);

    if (ota_service_in_progress())
    {
        lcd_show_ota_status(LCD_OTA_VIEW_CANCELLING, 0U, "", "",
                            "Please wait", false);
        return ota_service_cancel();
    }
    if (pending)
    {
        lcd_flash_message("Update Deferred", "Current kept", 1200U);
    }
    else if (checking)
    {
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
