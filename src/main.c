#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"
#if CONFIG_BT_ENABLED
#include "esp_bt.h"
#endif
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app/app_buttons.h"
#include "app/app_runtime.h"
#include "adc/adc_manager.h"
#include "app/app_services.h"
#include "cloud/cloud_reporting.h"
#include "diagnostics/system_diagnostics.h"
#include "events/event_dispatcher.h"
#include "events/fault_log.h"
#include "events/protection_handler.h"
#include "events/system_events.h"
#include "lcd/lcd_event_receiver.h"
#include "lcd/lcd_flash_queue.h"
#include "lcd/lcd.h"
#include "lcd/lcd_watchdog.h"
#include "lcd/lcd_writer.h"
#include "lcd/lcd_startup_config.h"
#include "ota/ota_service.h"
#include "post/post_manager.h"
#include "security/change_pin_flow.h"
#include "security/security.h"
#include "system/task_watchdog.h"
#include "system/core_affinity.h"
#include "utility/buzzer.h"
#include "utility/led.h"

static const char *APP_TAG = "APP_INIT";

/* Startup supervisor: diagnostic stage tracking is deliberately separate from
 * readiness event bits. A stage can be logged as complete only after its
 * existing subsystem reports success; this supervisor never manufactures
 * ADC/LCD readiness and never enables inverter output. */
typedef struct
{
    const char *stage;
    TickType_t started_at;
    bool active;
} startup_supervisor_t;

static startup_supervisor_t s_startup_supervisor = {0};

static void startup_supervisor_begin(const char *stage)
{
    s_startup_supervisor.stage = stage;
    s_startup_supervisor.started_at = xTaskGetTickCount();
    s_startup_supervisor.active = true;
    ESP_LOGI("STARTUP_SUPERVISOR",
             "stage=%s state=IN_PROGRESS reset_reason=%d free_heap=%u largest_block=%u",
             stage, (int)esp_reset_reason(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void startup_supervisor_finish(bool passed, const char *detail)
{
    const uint32_t elapsed_ms = s_startup_supervisor.active
        ? (uint32_t)((xTaskGetTickCount() - s_startup_supervisor.started_at) *
                     portTICK_PERIOD_MS)
        : 0U;
    ESP_LOGI("STARTUP_SUPERVISOR",
             "stage=%s state=%s duration_ms=%u free_heap=%u largest_block=%u detail=%s",
             s_startup_supervisor.stage ? s_startup_supervisor.stage : "UNKNOWN",
             passed ? "PASSED" : "FAILED", (unsigned)elapsed_ms,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             detail ? detail : "none");
    s_startup_supervisor.active = false;
}

static void startup_show_stage(lcd_startup_stage_t stage,
                               bool post_complete,
                               bool post_passed,
                               bool lcd_ok,
                               bool adc_ok,
                               bool fan_ok)
{
    lcd_show_startup_status(stage,
                            post_complete,
                            post_passed,
                            lcd_ok,
                            adc_ok,
                            fan_ok);

    vTaskDelay(pdMS_TO_TICKS(LCD_STARTUP_STAGE_DURATION_MS));
}

static uint8_t startup_bounded_progress(uint8_t confirmed_pct,
                                        uint8_t next_milestone_pct,
                                        uint32_t elapsed_ms,
                                        uint32_t interpolation_window_ms)
{
    if (next_milestone_pct <= confirmed_pct + 1U ||
        interpolation_window_ms == 0U)
    {
        return confirmed_pct;
    }

    const uint8_t ceiling = (uint8_t)(next_milestone_pct - 1U);
    const uint8_t span = (uint8_t)(ceiling - confirmed_pct);
    if (elapsed_ms >= interpolation_window_ms)
    {
        return ceiling;
    }
    return (uint8_t)(confirmed_pct +
                     ((uint32_t)span * elapsed_ms) /
                         interpolation_window_ms);
}

static void post_show_result_and_notify(const post_result_t result)
{
    /* Publish the terminal POST state for both pass and fail outcomes. The
     * LCD state machine must never remain at post_complete=false after POST
     * has returned, even when the fault screen replaces the status screen. */
    lcd_show_startup_status(LCD_STARTUP_STAGE_HARDWARE, true,
                            result.all_passed, result.lcd_ok,
                            result.adc_ok, result.fan_ok);
    ESP_LOGI("POST", "POST result propagated: complete=1 passed=%d lcd=%d adc=%d fan=%d",
             result.all_passed, result.lcd_ok, result.adc_ok, result.fan_ok);

    if (result.all_passed)
    {
        return;
    }

    char summary[LCD_LINE_SIZE] = {0};
    int pos = 0;
    if (!result.lcd_ok)
        pos += snprintf(summary + pos, sizeof(summary) - pos, "LCD ");
    if (!result.adc_ok)
        pos += snprintf(summary + pos, sizeof(summary) - pos, "ADC ");
    if (!result.fan_ok)
        pos += snprintf(summary + pos, sizeof(summary) - pos, "FAN ");

    char line1[LCD_LINE_SIZE];
    snprintf(line1, sizeof(line1), "%-16.16s", summary);

    if (result.lcd_ok)
    {
        lcd_show_fault("** POST FAILED  ", line1);
    }
    else
    {
        ESP_LOGE("POST", "LCD failed POST -- cannot display fault screen. Failed: %s", summary);
    }
}
/* Nothing in the running firmware starts Bluetooth (ble_provision_start() has
 * no callers and the "Bluetooth" setting only stores a flag), yet the BLE
 * controller is compiled in, so ESP-IDF keeps its static RAM reserved out of
 * the heap. A TLS firmware download needs a contiguous ~17 KB block and the
 * heap was too small/fragmented without that memory. Return it at boot, before
 * Wi-Fi starts. This is permanent until reboot; set to 0 if BLE provisioning
 * is ever wired up. */
#define INVERTER_RELEASE_BT_MEMORY 1

void app_main(void)
{
#if INVERTER_RELEASE_BT_MEMORY && CONFIG_BT_ENABLED
    {
        const size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        const esp_err_t bt_err = esp_bt_mem_release(ESP_BT_MODE_BTDM);
        ESP_LOGI(APP_TAG,
                 "Bluetooth memory release: %s; free heap %u -> %u bytes "
                 "(largest block %u)",
                 esp_err_to_name(bt_err), (unsigned)heap_before,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
#endif
    startup_supervisor_begin("BOOTSTRAP");
    startup_supervisor_finish(true, "app_main entered");
    bool lcd_event_ready = true;
    bool post_completed = false;
    post_result_t startup_post = {0};
    startup_supervisor_begin("WATCHDOG");
    if (!init_watchdog(true, true))
    {
        startup_supervisor_finish(false, "watchdog initialization failed");
        ESP_LOGE(APP_TAG, "FATAL: watchdog initialization failed");
        return;
    }
    startup_supervisor_finish(true, "watchdog initialized");
    startup_supervisor_begin("CORE_SERVICES");

    if (!system_events_init())
    {
        ESP_LOGE(APP_TAG, "System event queue initialization failed");
    }
    if (!event_dispatcher_init())
    {
        ESP_LOGE(APP_TAG, "Event dispatcher initialization failed; sound/events degraded");
    }
    sys_event_group = xEventGroupCreate();
    configASSERT(sys_event_group);

    sys_state_mutex = xSemaphoreCreateMutex();
    if (!sys_state_mutex)
    {
        ESP_LOGE(APP_TAG, "FATAL: mutex");
        return;
    }

    change_pin_mutex = xSemaphoreCreateMutex();
    if (change_pin_mutex == NULL)
    {
        ESP_LOGE(APP_TAG, "Failed to create change_pin_mutex");
    }

    /* Initialize rendering before any subsystem publishes display state. */
    const esp_err_t lcd_writer_err = lcd_writer_init();
    if (lcd_writer_err != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "LCD writer initialization failed: %s",
                 esp_err_to_name(lcd_writer_err));
        return;
    }

    /* NVS and system defaults must be ready before loading profiles or security. */
    const esp_err_t nvs_err = nvs_init(false);
    if (nvs_err != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "NVS initialization failed: %s",
                 esp_err_to_name(nvs_err));
        sys_state.system_ready = false;
        return;
    }
    const esp_err_t diagnostics_err = system_diagnostics_init();
    if (diagnostics_err != ESP_OK)
    {
        ESP_LOGW(APP_TAG, "System diagnostics initialization failed: %s",
                 esp_err_to_name(diagnostics_err));
    }
    const esp_err_t state_err = init_system_state();
    if (state_err != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "System state initialization failed: %s",
                 esp_err_to_name(state_err));
        sys_state.system_ready = false;
        return;
    }
    const esp_err_t menu_err = init_menu_system();
    if (menu_err != ESP_OK)
    {
        ESP_LOGW(APP_TAG, "Menu initialization completed with recovered settings: %s",
                 esp_err_to_name(menu_err));
    }
    if (security_init() != ESP_OK)
    {

        ESP_LOGE(APP_TAG, "Security initialization failed — panel PIN "
                          "protection unavailable; inverter will continue "
                          "without access control");
        sys_state.security.enabled = false;
    }
    fault_log_init();
    nvs_print_stats();
    if (nvs_is_initialized())
    {
        ESP_LOGI("MAIN", "NVS ready");
    }

    startup_supervisor_finish(true, "core services initialized; individual degraded services logged above");
    startup_supervisor_begin("HARDWARE");
    /* Hardware-dependent battery/LCD peripherals use the validated profile. */
    ESP_LOGI("STARTUP", "BEFORE init_hardware()");
    const esp_err_t hardware_err = init_hardware();
    ESP_LOGI("STARTUP", "AFTER init_hardware(): %s",
             esp_err_to_name(hardware_err));
    if (hardware_err != ESP_OK)
    {
        ESP_LOGW(APP_TAG, "Hardware initialization completed with warnings: %s",
                 esp_err_to_name(hardware_err));
    }
    const esp_err_t restore_err = restore_from_deep_sleep();
    if (restore_err != ESP_OK)
    {
        ESP_LOGW(APP_TAG, "Wake-state restoration failed: %s",
                 esp_err_to_name(restore_err));
    }
    log_all_error_flags(sys_state.error.error_flags);
    startup_supervisor_finish(hardware_err == ESP_OK, esp_err_to_name(hardware_err));
    startup_supervisor_begin("LCD_CONTROLLER");
    const esp_err_t lcd_power_err = lcd_power_init();
    if (lcd_power_err != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "LCD power initialization failed: %s",
                 esp_err_to_name(lcd_power_err));
        return;
    }
    LCD_power(true);
    const esp_err_t lcd_init_result = lcd_controller_init();
    lcd_set_brightness(200);
    lcd_startup_timer_begin();
    startup_supervisor_finish(lcd_init_result == ESP_OK, esp_err_to_name(lcd_init_result));

    /* Buzzer owns its LEDC timer/channel. A buzzer failure is deliberately
     * non-fatal: physical button events must remain independent of sound. */
    const esp_err_t buzzer_init_result = buzzer_init();
    if (buzzer_init_result != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "Buzzer unavailable; continuing without sound: %s",
                 esp_err_to_name(buzzer_init_result));
    }

    xEventGroupClearBits(sys_event_group,
                         APP_EVENT_ADC_READY | APP_EVENT_ADC_FAILED |
                             APP_EVENT_LCD_READY | APP_EVENT_LCD_FAILED);
    if (lcd_init_result != ESP_OK)
    {
        xEventGroupSetBits(sys_event_group, APP_EVENT_LCD_FAILED);
    }
    /* LCD_READY is owned by lcd_task after it verifies the controller. Do not
     * publish it here: controller init failure must remain a hard gate. */
    const esp_err_t adc_start_result = adc_manager_start();
    if (adc_start_result != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "Failed to start ADC subsystem: %s",
                 esp_err_to_name(adc_start_result));
        xEventGroupSetBits(sys_event_group, APP_EVENT_ADC_FAILED);
    }

    if (lcd_event_receiver_start() != ESP_OK)
    {
        lcd_event_ready = false;
        ESP_LOGE(APP_TAG, "Failed to start LCD event receiver");
    }

    /* LCD task alone publishes LCD_READY after it checks the initialized
     * controller. Do not start a renderer against a failed controller. */
    BaseType_t lcd_task_status = pdFAIL;
    if (lcd_init_result == ESP_OK)
    {
        /* Runtime stack report showed 3828 B minimum-ever free from 8192 B.
         * Keep 6144 B and revalidate under LCD/menu stress. */
        lcd_task_status = xTaskCreatePinnedToCore(
            lcd_task, "lcd_task", 6144, NULL, 4, &lcd_task_handle,
            APP_CORE_SYSTEM);
    }
    if (lcd_task_status != pdPASS)
    {
        ESP_LOGE(APP_TAG, "Failed to start LCD task (controller=%s)",
                 esp_err_to_name(lcd_init_result));
        xEventGroupSetBits(sys_event_group, APP_EVENT_LCD_FAILED);
    }

    // Wait for the LCD and ADC subsystems to report readiness or failure before proceeding with POST. The LCD task must be created before POST so that the status screen can be drawn.
    startup_show_stage(LCD_STARTUP_STAGE_IDENTITY, false, false,
                       lcd_init_result == ESP_OK, false, false);

    startup_show_stage(LCD_STARTUP_STAGE_HARDWARE, false, false,
                       lcd_init_result == ESP_OK, false, false);

    startup_show_stage(LCD_STARTUP_STAGE_ADC_INIT, false, false,
                       lcd_init_result == ESP_OK, false, false);

    startup_show_stage(LCD_STARTUP_STAGE_LOADING, false, false,
                       lcd_init_result == ESP_OK, false, false);

    /* These consumers are required for normal operation. In particular,
     * failure to create the protection consumer must inhibit system_ready;
     * never silently continue as a healthy inverter without it. */
    startup_supervisor_begin("RUNTIME_TASKS");
    bool startup_runtime_tasks_ready = true;
    const BaseType_t dispatcher_task_status =
        xTaskCreatePinnedToCore(event_dispatcher_task, "dispatcher", 3584,
                                NULL, 10, NULL, APP_CORE_SYSTEM);
    if (dispatcher_task_status != pdPASS)
    {
        startup_runtime_tasks_ready = false;
        ESP_LOGE(APP_TAG, "Required event dispatcher task creation failed");
    }
    const BaseType_t buzzer_task_status =
        xTaskCreatePinnedToCore(buzzer_event_task, "buzzer_evt", 4096,
                                NULL, 7, NULL, APP_CORE_SYSTEM);
    if (buzzer_task_status != pdPASS)
    {
        /* Sound is optional; do not make the power-control path depend on it. */
        ESP_LOGW(APP_TAG, "Buzzer event task unavailable; continuing silently");
    }
    const BaseType_t led_task_status =
        xTaskCreatePinnedToCore(led_event_task, "led_evt", 2048,
                                NULL, 7, NULL, APP_CORE_SYSTEM);
    if (led_task_status != pdPASS)
    {
        startup_runtime_tasks_ready = false;
        ESP_LOGE(APP_TAG, "Required LED event task creation failed");
    }
    /* Runtime stack report showed 3340 B minimum-ever free from 4096 B.
     * Keep 3072 B and revalidate during fault-log bursts. */
    const BaseType_t fault_log_task_status =
        xTaskCreatePinnedToCore(fault_log_event_task, "logger_evt", 3072,
                                NULL, 5, NULL, APP_CORE_SYSTEM);
    if (fault_log_task_status != pdPASS)
    {
        startup_runtime_tasks_ready = false;
        ESP_LOGE(APP_TAG, "Required fault-log event task creation failed");
    }
    const BaseType_t monitor_task_status =
        xTaskCreatePinnedToCore(monitor_event_task, "monitor_evt", 3072,
                                NULL, 4, NULL, APP_CORE_SYSTEM);
    if (monitor_task_status != pdPASS)
    {
        startup_runtime_tasks_ready = false;
        ESP_LOGE(APP_TAG, "Required monitor event task creation failed");
    }
    const BaseType_t protection_task_status =
        xTaskCreatePinnedToCore(protection_event_task, "prot_evt", 3072,
                                NULL, 9, NULL, APP_CORE_SYSTEM);
    if (protection_task_status != pdPASS)
    {
        startup_runtime_tasks_ready = false;
        ESP_LOGE(APP_TAG, "CRITICAL: protection event task creation failed");
    }
    startup_supervisor_finish(startup_runtime_tasks_ready,
                              startup_runtime_tasks_ready
                                  ? "required event consumers created"
                                  : "required runtime task creation failed; startup must remain inhibited");

    /* Start all consumers before enabling physical inputs so no press can race
     * task creation. */
    esp_err_t ret = app_buttons_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "FATAL: button init");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }

    if (!task_watchdog_start_supervisor())
    {
        ESP_LOGE(APP_TAG, "Failed to start watchdog health supervisor");
    }

    /* Wait for the ADC subsystem’s valid/fresh required snapshot and the LCD
     * task’s completed initialization before running POST. Every 100 ms timeout
     * feeds the watchdog while still allowing either task to publish a fatal
     * result. */
    startup_supervisor_begin("READINESS_GATES");
    const EventBits_t startup_wait_mask =
        APP_EVENT_ADC_READY | APP_EVENT_ADC_FAILED |
        APP_EVENT_LCD_READY | APP_EVENT_LCD_FAILED;
    EventBits_t startup_bits = 0U;
    const TickType_t startup_wait_start = xTaskGetTickCount();
    uint8_t spinner_frame = 0U;
    while ((xTaskGetTickCount() - startup_wait_start) < pdMS_TO_TICKS(10000))
    {
        startup_bits = xEventGroupWaitBits(
            sys_event_group,
            startup_wait_mask,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(100));
        const bool adc_ready = (startup_bits & APP_EVENT_ADC_READY) != 0U;
        const bool lcd_ready = (startup_bits & APP_EVENT_LCD_READY) != 0U;
        const bool startup_failed =
            (startup_bits & (APP_EVENT_ADC_FAILED | APP_EVENT_LCD_FAILED)) != 0U;
        const uint32_t wait_elapsed_ms =
            (uint32_t)((xTaskGetTickCount() - startup_wait_start) *
                       portTICK_PERIOD_MS);
        if (adc_ready && lcd_ready)
        {
            lcd_update_loading_progress(60U, spinner_frame++, "ADC READY");
        }
        else if (!startup_failed)
        {
            lcd_update_loading_progress(
                startup_bounded_progress(40U, 60U, wait_elapsed_ms, 10000U),
                spinner_frame++, "ADC / SENSORS");
        }
        if (startup_failed || (adc_ready && lcd_ready))
        {
            break;
        }
    }

    const bool adc_ready = (startup_bits & APP_EVENT_ADC_READY) != 0U;
    const bool lcd_ready = (startup_bits & APP_EVENT_LCD_READY) != 0U;
    startup_supervisor_finish(adc_ready && lcd_ready,
                              (adc_ready && lcd_ready) ? "ADC and LCD readiness confirmed" : "readiness failed or 10s timeout");
    if (adc_ready && lcd_ready)
    {
        startup_show_stage(LCD_STARTUP_STAGE_ADC_READY, false, false,
                           lcd_ready, true, false);
        startup_show_stage(LCD_STARTUP_STAGE_SETTINGS, false, false,
                           lcd_ready, true, false);
        startup_show_stage(LCD_STARTUP_STAGE_POST, false, false,
                           lcd_ready, true, false);
        startup_supervisor_begin("POST");
        startup_post = post_run_all();
        startup_supervisor_finish(startup_post.all_passed,
                                  startup_post.all_passed ? "all POST checks passed" : "one or more POST checks failed");
        post_completed = true;
        post_show_result_and_notify(startup_post);
        post_buzzer_event(startup_post.all_passed);
        post_led_event(startup_post.all_passed);
    }
    else
    {
        const bool adc_failed = (startup_bits & APP_EVENT_ADC_FAILED) != 0U;
        /* Startup begins with the inverter OFF and system_ready false. */
        const bool lcd_failed = (startup_bits & APP_EVENT_LCD_FAILED) != 0U;
        ESP_LOGE(APP_TAG, "Startup prerequisite %s; inhibiting inverter output",
                 (adc_failed || lcd_failed) ? "failed" : "timed out");
        startup_post = (post_result_t){
            .lcd_ok = lcd_ready,
            .adc_ok = adc_ready,
            .fan_ok = false,
            .failure_mask = (lcd_ready ? 0U : POST_FAILURE_LCD) |
                            (adc_ready ? 0U : POST_FAILURE_ADC),
            .all_passed = false,
        };
        post_completed = true;
        post_buzzer_event(false);
        post_led_event(false);
        lcd_show_startup_status(LCD_STARTUP_STAGE_HARDWARE, true, false,
                                lcd_ready, adc_ready, false);
        ESP_LOGI("POST", "Startup prerequisite result propagated: complete=1 passed=0 lcd=%d adc=%d",
                 lcd_ready, adc_ready);

        if (lcd_ready)
        {
            const char *fault = lcd_failed ? "LCD INIT FAIL   " : (adc_failed ? "ADC INIT FAIL   " : "ADC TIMEOUT     ");
            lcd_show_fault("SENSOR STARTUP ", fault);
        }
        else
        {
            ESP_LOGE(APP_TAG, "LCD task was not ready; cannot display startup fault");
        }
    }

    if (startup_post.all_passed && startup_runtime_tasks_ready &&
        nvs_is_initialized())
    {
        app_runtime_start_deferred_settings_persistence();
    }
    else
    {
        ESP_LOGW(APP_TAG,
                 "Skipping deferred settings persistence after failed startup");
    }

    const bool startup_healthy = nvs_is_initialized() && lcd_event_ready &&
                                 startup_runtime_tasks_ready && post_completed &&
                                 startup_post.all_passed;
    if (!startup_runtime_tasks_ready && lcd_ready)
    {
        lcd_show_fault("STARTUP TASK ERR", "OUTPUT INHIBITED");
    }

    /* Initialize only the Wi-Fi panel/control path after POST.  This does
     * not start the radio or any network service; it makes the selected
     * STA/AP/APSTA architecture and its menu actions available even when
     * broader background services are suppressed by a startup fault. */
    const esp_err_t wifi_panel_init_err = app_services_wifi_toggle_init();
    if (wifi_panel_init_err != ESP_OK) {
        ESP_LOGW(APP_TAG, "Wi-Fi panel initialization deferred: %s",
                 esp_err_to_name(wifi_panel_init_err));
    }

    startup_supervisor_begin("BACKGROUND_SERVICES");
    if (startup_healthy)
    {
        startup_show_stage(LCD_STARTUP_STAGE_SERVICES, true, true,
                           true, true, startup_post.fan_ok);
        ESP_LOGI(APP_TAG, "POST passed — starting background services");
        if (app_services_init() != ESP_OK)
        {
            ESP_LOGW(APP_TAG, "Network/update services unavailable; continuing offline");
        }
        if (cloud_reporting_init() != ESP_OK)
        {
            ESP_LOGW(APP_TAG, "Cloud reporting unavailable; continuing with local operation");
        }
        startup_show_stage(LCD_STARTUP_STAGE_READY, true, true,
                           true, true, startup_post.fan_ok);
    }
    else
    {
        ESP_LOGW(APP_TAG,
                 "POST failed or startup unhealthy — cloud reporting suppressed");
        /* Owner decision: firmware update must work whether startup passed or
         * failed (a hardware/POST fault is exactly when a fixed image may be
         * needed). Bring up the same Wi-Fi/network/OTA services as a healthy
         * start. This does NOT change protection: system_ready stays false and
         * inverter output stays inhibited below. Cloud telemetry stays off
         * because its readings are not trustworthy after a failed check. */
        if (app_services_init() != ESP_OK)
        {
            ESP_LOGW(APP_TAG, "Network/update services unavailable after startup fault");
        }
    }

    startup_supervisor_finish(startup_healthy,
                              startup_healthy ? "startup passed; output may be enabled by normal control logic" : "fault latched; output remains inhibited; recovery services continue");
    sys_state.system_ready = startup_healthy;

    /* Owner decision: a new OTA image is kept once it has reached runtime,
     * independent of the POST result. POST/ADC/fan failures describe the
     * hardware state, not whether this firmware is broken, so they must not
     * roll a freshly installed image back. A firmware that crashes, hangs or
     * resets before this point is still rolled back automatically by the
     * bootloader (the image stays PENDING_VERIFY until this call). */
    if (!startup_healthy && post_completed)
    {
        ESP_LOGW(APP_TAG,
                 "Startup fault latched, but firmware reached runtime; "
                 "keeping this image (output stays inhibited)");
    }
    const esp_err_t rollback_err = ota_service_validate_running_app(post_completed);
    if (rollback_err != ESP_OK && rollback_err != ESP_ERR_INVALID_STATE &&
        rollback_err != ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGE(APP_TAG, "OTA startup validation failed: %s",
                 esp_err_to_name(rollback_err));
    }

    /* Keep the terminal startup result visible for a deterministic minimum
     * duration. This task yields and feeds the watchdog while ADC, event,
     * monitoring, and protection tasks continue running normally. Readiness
     * and failure state remain authoritative; the timer cannot make a failed
     * startup healthy. */
    while (!lcd_startup_minimum_elapsed())
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Release startup filtering only after POST has reported and the visible
     * startup minimum has elapsed. This is the single authoritative
     * STARTUP -> NORMAL transition. */
    lcd_startup_release();
    ESP_LOGI("STARTUP_SUPERVISOR",
             "terminal_state=%s post_complete=%d post_passed=%d lcd_ready=%d adc_ready=%d nvs_ready=%d free_heap=%u largest_block=%u",
             startup_healthy ? "READY" : "FAULT_LATCHED", post_completed,
             startup_post.all_passed, lcd_ready, adc_ready, nvs_is_initialized(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (startup_healthy)
    {
        lcd_boot_complete();
    }
    /* Quietly bring Wi-Fi up in the background. Wi-Fi is enabled first only
     * when "Wi-Fi Auto On" (the master switch) is set and the user last left
     * Wi-Fi ON, then the saved network is connected when "Wi-Fi Auto Rec" is
     * set. Never notifies.
     *
     * Deliberately NOT gated on startup_healthy (owner decision): a failed
     * startup check (for example an ADC channel that cannot be read) must not
     * keep Wi-Fi off. This only affects Wi-Fi. The fault stays latched,
     * system_ready stays false and inverter output stays inhibited exactly as
     * before; nothing in the check or protection path is bypassed. */
    if (!startup_healthy)
    {
        ESP_LOGW(APP_TAG,
                 "Startup fault latched; Wi-Fi auto-start still allowed "
                 "(inverter output remains inhibited)");
    }
    (void)app_services_wifi_background_start();
    if (ota_service_rollback_notification_pending())
    {
        lcd_flash_info_to("Firmware Update", "Previous restored", 3500U,
                          LCD_SCREEN_MAIN);
    }
    if (!startup_healthy)
    {
        /* Keep button_task and the event consumers alive in the latched
         * startup-fault state. They remain safety-gated by system_ready, but
         * deinitializing them here made the physical inputs impossible to
         * diagnose or use for an explicitly safe recovery action. */
        ESP_LOGW(APP_TAG,
                 "Startup fault latched; retaining button/event tasks while output remains inhibited");
    }
    while (sys_state.system_ready || !startup_healthy)
    {
        update_lcd_activity_state();
        handle_menu_timeout();
        display_timeout_tick();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(APP_TAG, "Main loop ended");
    (void)lcd_event_receiver_stop();
    app_buttons_deinit();
}
