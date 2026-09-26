#include <stdbool.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
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
#include "utility/buzzer.h"
#include "utility/led.h"

static const char *APP_TAG = "APP_INIT";

static void startup_show_identity(void)
{
    static const uint8_t cgram_startup_sine_wave[8] = {
        0x00, 0x01, 0x03, 0x06, 0x0C, 0x18, 0x10, 0x00};
    const uint32_t identity_started_ms =
        (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    if (lcd_geometry_is_20x4())
    {
        lcd_create_custom_char(CHAR_SINE_WAVE, cgram_startup_sine_wave);

        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_print_str("       ");
        for (int i = 0; i < 6; ++i)
        {
            lcd_print_char(CHAR_SINE_WAVE);
        }

        lcd_set_cursor(1, 0);
        lcd_print_str("   JEZZYLINKS");

        lcd_set_cursor(2, 0);
        lcd_print_str(" SOLAR INVERTER");

        lcd_set_cursor(3, 0);
        lcd_print_str("                    ");
    }
    else
    {
        lcd_show_message("   JEZZYLINKS", " SOLAR INVERTER");
    }

    ESP_LOGI(APP_TAG, "Startup identity screen drawn, elapsed=%ums",
             (unsigned)(((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS)) -
                        identity_started_ms));

    /*
     * The identity presentation is owned by app_main, not lcd_task.
     * This is presentation time only; readiness/POST transitions remain
     * controlled by the startup coordinator below.
     */
    vTaskDelay(pdMS_TO_TICKS(3000));

    if ((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) -
            identity_started_ms >= LCD_STARTUP_IDENTITY_DURATION_MS)
    {
        /*
         * Restore BAR_0/BAR_1, the progress block, and Wi-Fi glyphs before
         * the normal startup/loading renderer uses the shared CGRAM slots.
         */
        lcd_init_cgram();
        lcd_show_loading("System Starting", LCD_STARTUP_LOADING_DURATION_MS,
                         LCD_SCREEN_STARTUP_STATUS);
    }
}

static void startup_show_stage(lcd_startup_stage_t stage,
                               bool post_complete,
                               bool post_passed,
                               bool lcd_ok,
                               bool adc_ok,
                               bool fan_ok)
{
    lcd_show_startup_status(stage, post_complete, post_passed,
                            lcd_ok, adc_ok, fan_ok);
    /* This is a minimum readable presentation window, not a readiness
     * substitute. The caller only advances after the real milestone for the
     * stage has completed. */
    vTaskDelay(pdMS_TO_TICKS(LCD_STARTUP_STAGE_DURATION_MS));
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
void app_main(void)
{
    bool lcd_event_ready = true;
    bool post_completed = false;
    post_result_t startup_post = {0};
    if (!init_watchdog(true, true))
    {
        ESP_LOGE(APP_TAG, "FATAL: watchdog initialization failed");
        return;
    }

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
    vTaskDelay(pdMS_TO_TICKS(2000));
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

    /* Buzzer owns its LEDC timer/channel. A buzzer failure is deliberately
     * non-fatal: physical button events must remain independent of sound. */
    const esp_err_t buzzer_init_result = buzzer_init();
    if (buzzer_init_result != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "Buzzer unavailable; continuing without sound: %s",
                 esp_err_to_name(buzzer_init_result));
    }

    /*
     * The startup identity is rendered and timed by app_main. This keeps
     * startup progression under the single startup coordinator instead of
     * allowing lcd_task to block and autonomously transition screens.
     */
    startup_show_identity();

    xEventGroupClearBits(sys_event_group,
                         APP_EVENT_ADC_READY | APP_EVENT_ADC_FAILED |
                             APP_EVENT_LCD_READY | APP_EVENT_LCD_FAILED);
    if (lcd_init_result != ESP_OK)
    {
        xEventGroupSetBits(sys_event_group, APP_EVENT_LCD_FAILED);
    }
    const esp_err_t adc_start_result = adc_manager_start();
    if (adc_start_result != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "Failed to start ADC subsystem: %s",
                 esp_err_to_name(adc_start_result));
        xEventGroupSetBits(sys_event_group, APP_EVENT_ADC_FAILED);
    }
    const BaseType_t lcd_task_status =
        xTaskCreate(lcd_task, "lcd_task", 4096, NULL, 4, &lcd_task_handle);
    if (lcd_task_status != pdPASS)
    {
        ESP_LOGE(APP_TAG, "Failed to create LCD task");
        xEventGroupSetBits(sys_event_group, APP_EVENT_LCD_FAILED);
    }

    startup_show_stage(LCD_STARTUP_STAGE_HARDWARE, false, false,
                       lcd_init_result == ESP_OK, false, false);
    startup_show_stage(LCD_STARTUP_STAGE_ADC_INIT, false, false,
                       lcd_init_result == ESP_OK, false, false);
    if (lcd_event_receiver_start() != ESP_OK)
    {
        lcd_event_ready = false;
        ESP_LOGE(APP_TAG, "Failed to start LCD event receiver");
    }
    xTaskCreatePinnedToCore(event_dispatcher_task, "dispatcher", 4096, NULL, 10, NULL, 1);
    const BaseType_t buzzer_task_status =
        xTaskCreatePinnedToCore(buzzer_event_task, "buzzer_evt", 2048, NULL, 7, NULL, 1);
    if (buzzer_task_status != pdPASS)
    {
        ESP_LOGE(APP_TAG, "FATAL: failed to create buzzer event task");
    }
    xTaskCreatePinnedToCore(led_event_task, "led_evt", 2048, NULL, 7, NULL, 1);
    xTaskCreatePinnedToCore(fault_log_event_task, "logger_evt", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(monitor_event_task, "monitor_evt", 3072, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(protection_event_task, "prot_evt", 4096, NULL, 9, NULL, 0);

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
    const EventBits_t startup_wait_mask =
        APP_EVENT_ADC_READY | APP_EVENT_ADC_FAILED |
        APP_EVENT_LCD_READY | APP_EVENT_LCD_FAILED;
    EventBits_t startup_bits = 0U;
    const TickType_t startup_wait_start = xTaskGetTickCount();
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
        if (startup_failed || (adc_ready && lcd_ready))
        {
            break;
        }
    }

    const bool adc_ready = (startup_bits & APP_EVENT_ADC_READY) != 0U;
    const bool lcd_ready = (startup_bits & APP_EVENT_LCD_READY) != 0U;
    if (adc_ready && lcd_ready)
    {
        startup_show_stage(LCD_STARTUP_STAGE_ADC_READY, false, false,
                           lcd_ready, true, false);
        startup_show_stage(LCD_STARTUP_STAGE_SETTINGS, false, false,
                           lcd_ready, true, false);
        startup_show_stage(LCD_STARTUP_STAGE_POST, false, false,
                           lcd_ready, true, false);
        startup_post = post_run_all();
        post_completed = true;
        post_show_result_and_notify(startup_post);
        post_buzzer_event(startup_post.all_passed);
        post_led_event(startup_post.all_passed);
    }
    else
    {
        const bool adc_failed = (startup_bits & APP_EVENT_ADC_FAILED) != 0U;
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

    if (startup_post.all_passed && nvs_is_initialized())
    {
        app_runtime_start_deferred_settings_persistence();
    }
    else
    {
        ESP_LOGW(APP_TAG,
                 "Skipping deferred settings persistence after failed startup");
    }

    const bool startup_healthy = nvs_is_initialized() && lcd_event_ready &&
                                 post_completed && startup_post.all_passed;
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
                 "POST failed or startup unhealthy — background services suppressed");
    }

    sys_state.system_ready = startup_healthy;
    const esp_err_t rollback_err = ota_service_validate_running_app(startup_healthy);
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
    if (startup_healthy)
    {
        lcd_boot_complete();
    }
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
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(APP_TAG, "Main loop ended");
    (void)lcd_event_receiver_stop();
    app_buttons_deinit();
}
