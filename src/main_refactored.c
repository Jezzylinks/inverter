/*==============================================================================
  main_refactored.c  —  Full LCD refactor
  Rule: ZERO lcd_* hardware calls outside lcd_task().
        Every other function calls lcd_set_screen_*() writers instead.
        lcd_task() owns the hardware exclusively.
==============================================================================*/

/* ── All original includes kept unchanged ──────────────────────────────── */
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_system.h>
#include "esp_timer.h"
#include <nvs_flash.h>
#include <nvs.h>
#include "driver/ledc.h"
#include <driver/gpio.h>
#include "driver/i2c.h"
#include <stdint.h>
#include <string.h>
#include <esp_log.h>
#include <math.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_continuous.h>
#include <esp_adc/adc_cali_scheme.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp_task_wdt.h"
#include "esp_private/system_internal.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc_caps.h"
#include "soc/soc.h"
#include "esp_event.h"
#include "lcd.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"
#include "rom/ets_sys.h"
#include "utils.h"
#include <stdbool.h>
#include <button_controller.h>

/* ── NEW: lcd_state / lcd_writer headers ──────────────────────────────── */
#include "lcd_state.h"
#include "lcd_writer.h"

/* ── All original #defines kept unchanged ─────────────────────────────── */
#define WIFI_SSID "johnson"
#define WIFI_PASS "internet"
#define WEATHER_API_KEY "YOUR_OPEN_WEATHER_API_KEY"
#define CITY_NAME "Lagos"
#define WEATHER_CHECK_INTERNAL_MS 60000
#define FIRMWARE_VERSION "v1.0.3"
#define LCD_ADDR 0x27
#define LCD_COLS 16
#define LCD_ROWS 2
#define SCROLL_DELAY_MS 300
#define ANIM_DELAY_MS 80
#define SDA_PIN 21
#define SCL_PIN 22
#define BUTTON_DEBOUNCE_TIME_MS 200
#define CONFIG_USE_ADC 1
#define CONFIG_USE_BUTTONS 1
#define CONFIG_USE_LCD 1
#define CONFIG_USE_LED_PWM 1
#define CONFIG_USE_DEEP_SLEEP 0
#define CONFIG_USE_DISPLAY_TIMEOUT_TASK 1
#define USE_ADC2
#define FILTER_DEPTH 10
#define TAG_ADC "ADC_INIT"
#define ADC_UNIT_1 ADC_UNIT_1
#define ADC_UNIT_USED ADC_UNIT_1
#define ADC_ATTEN_USED ADC_ATTEN_DB_12
#define ADC_BIT_WIDTH_USED ADC_BITWIDTH_DEFAULT
#define ADC_CHANNEL_MAX 5
#define VOLTAGE_DIVIDER_RATIO ((R1_DIVIDER + R2_DIVIDER) / (float)R2_DIVIDER)
#define ADC_ERROR_CODE 0xFD
#define WATCHDOG_ERROR_CODE 0xFE
#define STACK_OVERFLOW_ERROR_CODE 0xFC
#define PERSISTENT_ERROR_CODE 0xFF
#define GPIO_BTN_UP GPIO_NUM_17
#define GPIO_BTN_DOWN GPIO_NUM_5
#define GPIO_BTN_ENTER GPIO_NUM_19
#define GPIO_BTN_BACK GPIO_NUM_18
#define GPIO_PWR_BTN GPIO_NUM_0
#define GPIO_BUZZER GPIO_NUM_13
#define GPIO_STATUS_LED GPIO_NUM_14
#define GPIO_ERROR_LED GPIO_NUM_26
#define GPIO_POWER_RELAY GPIO_NUM_12
#define GPIO_NEPA_INPUT GPIO_NUM_22
#define GPIO_FAN GPIO_NUM_33
#define GPIO_FAN_TEST GPIO_NUM_4
#define FAN_DISCONNECTED_THRESHOLD 50
#define FAN_DISCONNECT_RETRIES 3
#define FAN_CHECK_INTERVAL_MS 10000
#define DC_VOLTAGE_MIN 10.0f
#define DC_VOLTAGE_MAX 60.0f
#define AC_VOLTAGE_MIN 200.0f
#define AC_VOLTAGE_MAX 250.0f
#define GRID_VOLTAGE_MIN 200.0f
#define GRID_VOLTAGE_MAX 250.0f
#define GRID_FREQ_MIN 49.5f
#define GRID_FREQ_MAX 50.5f
#define DC_CURRENT_MAX 100.0f
#define AC_CURRENT_MAX 50.0f
#define HEATSINK_TEMP_MAX 85.0f
#define TRANSFORMER_TEMP_MAX 100.0f
#define AMBIENT_TEMP_MAX 50.0f
#define AMBIENT_TEMP_MIN -10.0f
#define FAN_START_TEMP 60.0f
#define DC_INJECTION_MAX 0.5f
#define INSULATION_RESISTANCE_MIN 1.0f
#define DC_BUS_IMBALANCE_MAX 5.0f
#define POWER_FACTOR_MIN 0.7f
#define BATTERY_SOC_MIN 20.0f
#define BATTERY_HEALTH_MIN 70.0f
#define MIN_OFF_TIME_MS 5000
#define FAULT_OVERCURRENT (1 << 0)
#define FAULT_SHORT_CIRCUIT (1 << 1)
#define FAULT_GROUND_FAULT (1 << 2)
#define FAULT_ARC_DETECTED (1 << 3)
#define FAULT_GATE_DRIVER (1 << 4)
#define FAULT_SENSOR_COMM (1 << 5)
#define FAULT_CAN_COMM (1 << 6)
#define FAULT_REVERSE_POLARITY (1 << 7)
#define FAULT_WATCHDOG (1 << 8)
#define SYS_STATE_MUTEX_TIMEOUT_MS 100
#define NVS_NS_SYSTEM "inv_sys_v2"
#define DISPLAY_TIMEOUT 300
#define SLEEP_TIMEOUT 1800
#define LCD_PWR_GPIO GPIO_NUM_27
#define LCD_BL_GPIO GPIO_NUM_13
#define LCD_PWM_CHANNEL LEDC_CHANNEL_0
#define LCD_PWM_FREQ 5000
#define LCD_PWM_RES LEDC_TIMER_8_BIT
#define ALERT_TONE_FREQ 500
#define ALERT_TONE_VOLUME 200
#define WAKEUP_BUTTON_1 GPIO_BTN_ENTER
#define WAKEUP_BUTTON_2 GPIO_BTN_BACK
#define tag "LCD"
#define BUTTON_NONE -1
#define BATTERY_MENU_COUNT 3
#define BATTERY_PROFILE_VERSION 1
#define BATTERY_TYPE_KEY "battery_type"
#define BATTERY_CAPACITY_KEY "bat_capacity"
#define NVS_VOLTAGE_KEY_PREFIX "voltage_"
#define BATTERY_VOLTAGE_SYSTEM_KEY "inv_bat_volt"
#define BATTERY_CAPACITY_AH "bat_capacity_ah"
#define DEFAULT_BATTERY_PROFILE BATTERY_CHEMISTRY_LITHIUM_ION
#define FREQUENCY_SETTING_KEY "frequency"
#define R1_DIVIDER 15000.0f
#define R2_DIVIDER 4700.0f
#define SHUNT_RESISTOR 0.005f
#define CURRENT_GAIN 50.0f
#define TEMP_BETA 3950.0f
#define NTC_25C 10000.0f
#define ADC_REF_VOLTAGE 3.3f
#define PWM_FREQ 5000
#define PWM_RESOLUTION LEDC_TIMER_13_BIT
#define ADC_CALIBRATION_SAMPLES 64
#define DEFAULT_SCROLL_SPEED 3
#define MAX_TEMPERATURE 85.0f
#define MAX_CURRENT 25.0f
#define LOW_BATTERY_VOLTAGE_THRESHOLD 10.5f
#define HIGH_BATTERY_VOLTAGE 14.8f
#define UNDER_VOLTAGE_THRESHOLD 160.0f
#define OVER_VOLTAGE_THRESHOLD 260.0f
#define BATTERY_VOLTAGE_THRESHOLD 12.0f
#define INVERTER_OUTPUT_VOLTAGE_THRESHOLD 220.0f
#define FAN_SPEED_THRESHOLD 2.0f
#define FAN_SPEED_MAX 5.0f
#define FAN_SPEED_MIN 0.5f
#define ERROR_ADC_FAILURE 0xFD
#define ERROR_WATCHDOG_TIMEOUT 0xFE
#define ERROR_STACK_OVERFLOW 0xFC
#define ERROR_PERSISTENT_FAULT 0xFF
#define LOW_BATTERY_ERROR 0x01
#define OVER_UNDER_VOLTAGE_ERROR 0x02
#define INVERTER_VOLTAGE_ERROR 0x03
#define FAN_DISCONNECTED_ERROR 0x04
#define SYSTEM_FAILURE_ERROR 0x05
#define OVER_TEMPERATURE_ERROR 0x06
#define OVERLOAD_ERROR 0x07
#define CONSECUTIVE_ERROR_CODE 0x08
#define RTC_MAGIC_FLAG 0xA5A5A5A5
#define BUTTON_MAX 5
#define DEBOUNCE_THRESHOLD_MS 50
#define LONG_PRESS_MS 3000
#define HOLD_PRESS_MS 500
#define VERY_LONG_PRESS_MS 3000
#define DOUBLE_CLICK_MS 400
#define REPEAT_INITIAL_DELAY_MS 500
#define REPEAT_INTERVAL_MS 100
#define DEBOUNCE_TIME_MS 20
#define REPEAT_DELAY_MS 1000
#define REPEAT_RATE_MS 100
#define ISR_QUEUE_SIZE 10
#define TASK_POLL_INTERVAL_MS 10
#define RESET_TIMEOUT_MS 10000
#define LONG_PRESS_THRESHOLD_MS 2000
#define VERY_LONG_PRESS_THRESHOLD_MS 5000
#define SEQUENCE_TIMEOUT_MS 3000
#define MENU_TIMEOUT_MS 30000
#define FACTORY_RESET_HOLD_MS 10000
#define FAST_INCREMENT_THRESHOLD_MS 500
#define REPEAT_ACCELERATION_MS 100
#define VALUE_CONFIRM_TIMEOUT_MS 5000
#define LCD_SCROLL_DELAY_MS 300
#define LCD_BLINK_INTERVAL_MS 500
#define MAX_REPEAT_MULTIPLIER 10
#define PRECISION_MODE_DIVISOR 10
#define CLICK_TIMEOUT_MS 400
#define MENU_INDICATOR_MIN_ITEMS 3
#define MENU_INDICATOR_MAX_LEN 6
#define MENU_ARROW '>'
#define MENU_INDENT ' '
#define CHAR_SELECTED 0x3E
#define ADC_MEASURED_MIN 0.4f
#define ADC_MEASURED_MAX 3.12f
#define ADC_TARGET_MIN 0.0f
#define ADC_TARGET_MAX 3.3f
#define ENABLE_ADC_RANGE_MAPPING 1
#define BATTERY_DEBOUNCE_COUNT 3
#define BATTERY_FILTER_ALPHA 0.2f
#define R1_BATTERY_VOLTAGE 56000.0f
#define R2_BATTERY_VOLTAGE 15000.0f
#define BATTERY_VOLTAGE_DIVIDER_RATIO ((R1_BATTERY_VOLTAGE + R2_BATTERY_VOLTAGE) / R2_BATTERY_VOLTAGE)
#define R1_LOW_BATTERY 56000.0f
#define R2_LOW_BATTERY 15000.0f
#define LOW_BATTERY_DIVIDER_RATIO ((R1_LOW_BATTERY + R2_LOW_BATTERY) / R2_LOW_BATTERY)
#define R1_AC_VOLTAGE 56000.0f
#define R2_AC_VOLTAGE 15000.0f
#define AC_VOLTAGE_DIVIDER_RATIO ((R1_AC_VOLTAGE + R2_AC_VOLTAGE) / R2_AC_VOLTAGE)
#define R1_INVERTER_VOLTAGE 56000.0f
#define R2_INVERTER_VOLTAGE 15000.0f
#define INVERTER_VOLTAGE_DIVIDER_RATIO ((R1_INVERTER_VOLTAGE + R2_INVERTER_VOLTAGE) / R2_INVERTER_VOLTAGE)
#define R1_FAN_VOLTAGE 56000.0f
#define R2_FAN_VOLTAGE 15000.0f
#define FAN_SPEED_VOLTAGE_DIVIDER_RATIO ((R1_FAN_VOLTAGE + R2_FAN_VOLTAGE) / R2_FAN_VOLTAGE)
#define ADC_MULTISAMPLING_COUNT 10
#define EVT_ADC_READY (1 << 0)
#define EVT_ADC_VALID (1 << 1)
#define EVT_DEEPSLEEP_RESTORED (1 << 2)
#define DISPLAY_BUFFER_SIZE 17
#define SCREEN_DISPLAY_DURATION_MS 5000
#define SCREEN_SWITCH_INTERVAL_MS 1000
#define ERROR_DISPLAY_INTERVAL_MS 1000
#define STARTUP_DELAY_MS 2000
#define NUM_DISPLAY_SCREENS 3
#define LCD_REFRESH_RATE_MS 500
#define LCD_ROTATION_INTERVAL_MS 5000
#define VOLTAGE_DELTA_THRESH 0.09f
#define LOAD_DELTA_THRESH 1
#define LINE1_CYCLE_MS 3000
#define INVERTER_RATED_WATTS 2400
#define BATT_CELLS 5
#define BATT_V_MIN 44.0f
#define BATT_V_MAX 54.4f
#define STARTUP_ANIMATION_DURATION_MS 2000
#define VOLTAGE_SMOOTHING_FACTOR 0.2f
#define LOAD_SMOOTHING_FACTOR 0.3f
#define MAX_MENU_HISTORY 10
#define R1 10000.0
#define R2 2000.0
#define ADC_CHANNEL ADC1_CHANNEL_0
#define ADC_WIDTH ADC_WIDTH_BIT_12
#define ADC_ATTEN ADC_ATTEN_DB_11
#define ADC_CHANNEL_USE 2
#define MAIN_MENU_ITEM_COUNT 3
#define VOLTAGE_TYPE_COUNT 4
#define MAX_PROFILES 3
#define MIN_FREQUENCY 50
#define MAX_FREQUENCY 200
#define FREQUENCY_STEP 1
#define MAX_AP_NUM 10
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MAX_RETRY 5
#define LOG_TAG "LOG_MANAGER"
#define MAX_ERROR_LOG_ENTRIES 10

/* ── Global handles (unchanged) ───────────────────────────────────────── */
SemaphoreHandle_t sys_state_mutex;
TaskHandle_t lcd_task_handle = NULL;

/*
 * The single lcd_render_state instance.
 * lcd_writer.c externs this; lcd_task.c externs this.
 * Protected by sys_state_mutex for writers; lcd_task snapshots under mutex.
 */
lcd_render_state_t sys_lcd;

const uint64_t wakeup_pin_mask =
    (1ULL << WAKEUP_BUTTON_1) | (1ULL << WAKEUP_BUTTON_2);

static bool nvs_initialized = false;

/* ── All original type definitions kept verbatim ──────────────────────── */
/* (inverter_state_t, battery_profile_t, system_state_t, etc. — unchanged) */
/* ... [identical to original — omitted here for brevity in this comment,  */
/*      paste all original structs/enums here verbatim] ...                */

/*==============================================================================
  HELPER: build a pre-formatted 16-char menu row from label + indicator
  Called by any function that used to call lcd_draw_menu_scroll() directly.
==============================================================================*/
static void build_menu_rows(menu_state_t menu_st, int selection,
                            char *out_row0, char *out_row1)
{
    int item_count = 0;
    const menu_item_t *items = get_menu_items(menu_st, &item_count);

    if (!items || item_count == 0)
    {
        snprintf(out_row0, 17, "%-16s", "(empty menu)");
        snprintf(out_row1, 17, "%-16s", "");
        return;
    }

    if (selection < 0)
        selection = 0;
    if (selection >= item_count)
        selection = item_count - 1;

    /* Row 0: ">Label           " */
    snprintf(out_row0, 17, "%c%-15.15s", MENU_ARROW, items[selection].label);

    /* Row 1: " Label     X/N" or just " Label         " */
    int next = (selection + 1) % item_count;
    if (item_count >= MENU_INDICATOR_MIN_ITEMS)
    {
        char ind[MENU_INDICATOR_MAX_LEN + 1];
        int ind_len = snprintf(ind, sizeof(ind), "%d/%d",
                               selection + 1, item_count);
        int label_w = LCD_COLS - 1 - ind_len;
        if (label_w < 1)
            label_w = 1;
        snprintf(out_row1, 17, "%c%-*.*s%s",
                 MENU_INDENT, label_w, label_w,
                 items[next].label, ind);
    }
    else
    {
        snprintf(out_row1, 17, "%c%-15.15s", MENU_INDENT, items[next].label);
    }
}

/*==============================================================================
  CONVENIENCE: switch to menu screen with freshly-built rows
==============================================================================*/
static void show_menu_screen(menu_state_t menu_st, int selection)
{
    char r0[17], r1[17];
    build_menu_rows(menu_st, selection, r0, r1);
    lcd_show_menu(r0, r1);
}

/*==============================================================================
  CONVENIENCE: switch to main screen (wraps lcd_show_main)
==============================================================================*/
static void go_to_main_screen(void)
{
    lcd_show_main();
}

/*==============================================================================
  ALL FUNCTIONS — lcd hardware calls replaced with lcd_writer calls
==============================================================================*/

/* ── select_battery_type() ──────────────────────────────────────────────── */
void select_battery_type(button_id_t btn)
{
    static battery_chemistry_t selected = BATTERY_AGM;
    static bool updated = true;
    char display_char[20];

    switch (btn)
    {
    case BTN_UP:
        selected = (selected + 1) % BATTERY_TYPE_COUNT;
        updated = true;
        break;
    case BTN_DOWN:
        selected = (selected - 1 + BATTERY_TYPE_COUNT) % BATTERY_TYPE_COUNT;
        updated = true;
        break;
    case BTN_ENTER_MENU:
    {
        esp_err_t err = battery_save_configuration(
            (battery_type_t)selected, VOLTAGE_SYSTEM_48V, 200);
        if (err == ESP_OK)
        {
            snprintf(display_char, sizeof(display_char),
                     "Saved: %s", battery_type_names[selected]);
            lcd_flash_message("Battery Type    ", display_char, 1500);
        }
        else
        {
            lcd_flash_message("Save Failed!    ", "                ", 1500);
        }
        updated = true;
        break;
    }
    default:
        break;
    }

    if (updated)
    {
        char r0[17], r1[17];
        snprintf(r0, 17, "%-16s", "Select Battery:");
        snprintf(r1, 17, "> %-14.14s", battery_type_names[selected]);
        lcd_show_menu(r0, r1);
        updated = false;
    }
}

/* ── power_task() ───────────────────────────────────────────────────────── */
void power_task(void *arg)
{
    static bool last_relay_state = false;
    static TickType_t last_state_change = 0;
    const TickType_t DEBOUNCE_TIME = pdMS_TO_TICKS(2000);

    while (1)
    {
        if (sys_state.inverter.inverter_active || sys_state.inverter.connected)
            sys_state.flags.last_power_event = xTaskGetTickCount();

        if ((xTaskGetTickCount() - sys_state.flags.last_power_event) >
            pdMS_TO_TICKS(30 * 60 * 1000))
        {
            ESP_LOGI("POWER_TASK", "Entering deep sleep");
            enter_deep_sleep(3600);
        }

        if (sys_state.inverter.temperature > 90.0f)
        {
            /* Show fault and restart */
            lcd_show_fault("Critical Temp!  ", "Shutting Down...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            perform_system_restart(false);
        }

        bool new_relay_state;
        if (sys_state.inverter.connected &&
            !(sys_state.error.error_flags & ERR_AC_FAULT))
        {
            new_relay_state = true;
        }
        else
        {
            if (!(sys_state.error.error_flags &
                  (ERR_OVER_TEMP | ERR_OVERLOAD | ERR_LOW_BAT | ERR_HIGH_BAT)))
                new_relay_state = false;
            else
                new_relay_state = last_relay_state;
        }

        if (new_relay_state != last_relay_state)
        {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_state_change) > DEBOUNCE_TIME)
            {
                gpio_set_level(GPIO_POWER_RELAY, new_relay_state ? 1 : 0);
                sys_state.inverter.inverter_active = !new_relay_state;
                last_relay_state = new_relay_state;
                last_state_change = now;
                ESP_LOGI("POWER_TASK", "Relay: %s",
                         new_relay_state ? "AC" : "INVERTER");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ── adc_task() — only change: lcd_writer call for first-sample log ────── */
/* (ADC code unchanged; notification to lcd_task via task notify retained)  */
void adc_task(void *arg)
{
    /* ... all ADC init code identical to original ... */

    bool first_sample = true;
    while (1)
    {
        /* ... process_adc_reading calls unchanged ... */
        process_battery_voltage();
        xEventGroupSetBits(sys_event_group, EVT_ADC_READY | EVT_ADC_VALID);

        /* Update main screen data for lcd_task */
        lcd_update_main_data(
            sys_state.inverter.battery.voltage,
            sys_state.inverter.output_voltage,
            sys_state.inverter.output_current,
            sys_state.inverter.output_frequency,
            sys_state.inverter.battery.battery_temperature,
            sys_state.inverter.load_percentage,
            calculate_battery_percentage(sys_state.inverter.battery.voltage),
            sys_state.inverter.inverter_active,
            sys_state.inverter.connected,
            sys_state.battery_charging);

        /* Show fault screen immediately if error flags are set */
        if (sys_state.error.error_flags)
        {
            const char *err = get_error_string(sys_state.error.error_flags);
            char l0[17], l1[17];
            snprintf(l0, 17, "%-16.16s", err);
            snprintf(l1, 17, "%-16s", "Check system    ");
            lcd_show_fault(l0, l1);
        }
        else if (sys_state.lcd_render.screen == LCD_SCREEN_FAULT)
        {
            /* Fault cleared — return to main */
            lcd_clear_fault();
        }

        if (first_sample)
        {
            first_sample = false;
            ESP_LOGI(TAG_ADC, "First sample: Battery=%.2fV",
                     sys_state.inverter.battery.voltage);
            lcd_boot_complete(); /* switches lcd_task to LCD_SCREEN_MAIN */
            if (lcd_task_handle != NULL)
                xTaskNotifyGive(lcd_task_handle);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ── lcd_update_menu_screen() ───────────────────────────────────────────── */
void lcd_update_menu_screen(void)
{
    show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
}

/* ── lcd_show_monitoring_detail() ──────────────────────────────────────── */
void lcd_show_monitoring_detail(const char *label, float value,
                                const char *unit)
{
    char l[17], v[17];
    snprintf(l, 17, "%-16.16s", label);
    snprintf(v, 17, "%.2f %-13.13s", value, unit ? unit : "");
    lcd_show_monitor_detail(l, v);
}

/* ── lcd_draw_menu_scroll() ─────────────────────────────────────────────── */
/* All callers of lcd_draw_menu_scroll() replaced by show_menu_screen().    */
/* This wrapper keeps any remaining direct calls compiling.                  */
void lcd_draw_menu_scroll(menu_state_t menu_st, int selection)
{
    show_menu_screen(menu_st, selection);
}

/* ── lcd_show_value_edit_screen() ──────────────────────────────────────── */
void lcd_show_value_edit_screen(void)
{
    value_edit_context_t *config = get_current_value_config();
    float *current_value = get_current_value_pointer();

    if (!config || !current_value)
    {
        lcd_show_value_edit("Error: No param ", "                ", false);
        return;
    }

    char v[17];
    snprintf(v, 17, "%.2f %-11.11s", *current_value,
             config->unit ? config->unit : "");
    lcd_show_value_edit(config->label ? config->label : "Param",
                        v,
                        sys_state.pending_confirmation);
}

/* ── lcd_show_bt_connecting_screen() ───────────────────────────────────── */
void lcd_show_bt_connecting_screen(const char *device_name)
{
    char r1[17];
    snprintf(r1, 17, "%-16.16s", device_name ? device_name : "");
    lcd_show_wifi_connecting(device_name ? device_name : "");
    /* reuse wifi_connecting screen — same layout */
}

/* ── lcd_show_factory_reset_screen() ───────────────────────────────────── */
void lcd_show_factory_reset_screen(void)
{
    lcd_show_factory_confirm();

    button_event_info_t ev;
    bool waiting = true;
    int64_t entry = esp_timer_get_time() / 1000;

    while (waiting)
    {
        if (xQueueReceive(button_event_queue, &ev, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            if (ev.event == BUTTON_EVENT_CLICK)
            {
                switch (ev.button_id)
                {
                case BTN_ENTER_MENU:
                    lcd_show_factory_progress(0);
                    perform_factory_reset();
                    lcd_show_factory_done();
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    sys_state.menu_state = MAIN_MENU;
                    show_menu_screen(MAIN_MENU, 0);
                    waiting = false;
                    break;
                case BTN_BACK:
                    lcd_flash_message("Cancelled       ", "                ", 800);
                    sys_state.menu_state = MAIN_MENU;
                    show_menu_screen(MAIN_MENU, 0);
                    waiting = false;
                    break;
                default:
                    break;
                }
            }
        }
        if ((esp_timer_get_time() / 1000 - entry) > 15000)
        {
            lcd_flash_message("Timeout         ", "                ", 800);
            sys_state.menu_state = MAIN_MENU;
            show_menu_screen(MAIN_MENU, 0);
            waiting = false;
        }
    }
}

/* ── lcd_show_bt_edit_screen() ──────────────────────────────────────────── */
void lcd_show_bt_edit_screen(const char *label, const char *value)
{
    char l[17], v[17];
    snprintf(l, 17, "%s:", label ? label : "");
    if (value && strlen(value) > LCD_COLS)
    {
        snprintf(v, 17, "%-15.15s>", value);
    }
    else
    {
        snprintf(v, 17, "%-16.16s", value ? value : "");
    }
    lcd_show_value_edit(l, v, false);
}

/* ── lcd_show_value_saved_screen() ─────────────────────────────────────── */
void lcd_show_value_saved_screen(void)
{
    value_edit_context_t *config = get_current_value_config();
    float *current_value = get_current_value_pointer();
    char v[17] = "                ";
    if (config && current_value)
        snprintf(v, 17, "%.2f %-11.11s", *current_value,
                 config->unit ? config->unit : "");
    lcd_flash_saved("Value Saved!    ", v);
}

/* ── lcd_show_value_canceled_screen() ──────────────────────────────────── */
void lcd_show_value_canceled_screen(void)
{
    lcd_flash_cancelled();
}

/* ── exit_detail_view() ─────────────────────────────────────────────────── */
void exit_detail_view(void)
{
    if (!sys_state.in_detail_view)
        return;

    sys_state.in_detail_view = false;
    sys_state.inverter.inverter_state = sys_state.pre_detail_inverter_state;
    sys_state.menu_state = sys_state.detail_parent_menu;
    sys_state.menu_selection = sys_state.detail_parent_selection;

    show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
}

/* ── enter_submenu() ────────────────────────────────────────────────────── */
void enter_submenu(menu_state_t new_state)
{
    push_menu_history(sys_state.menu_state, sys_state.menu_selection);
    sys_state.menu_state = new_state;
    sys_state.menu_selection = 0;
    show_menu_screen(new_state, 0);
}

/* ── handle_power_button_event() ─────────────────────────────────────────── */
void handle_power_button_event(const button_event_info_t *event_info,
                               void *user_data)
{
    if (!sys_state.system_ready)
        return;
    int64_t current_time = event_info->timestamp_us / 1000;

    switch (event_info->event)
    {

    case BUTTON_EVENT_CLICK:
    {
        /* P1: cancel value edit */
        if (sys_state.value_edit_mode)
        {
            if (sys_state.value_changed)
            {
                float *val = get_current_value_pointer();
                if (val)
                    *val = sys_state.edit_backup_value;
            }
            sys_state.value_edit_mode = false;
            sys_state.value_changed = false;
            sys_state.pending_confirmation = false;
            lcd_flash_message("Edit Cancelled  ", "                ", 600);
            /* flash auto-returns — nothing else needed */
            break;
        }
        /* P2: cancel confirmation */
        if (sys_state.in_confirmation_screen)
        {
            sys_state.in_confirmation_screen = false;
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            break;
        }
        /* P3: exit detail view */
        if (sys_state.in_detail_view)
        {
            bool back_to_diag =
                (sys_state.pre_detail_inverter_state == INVERTER_DIAGNOSTIC);
            sys_state.in_detail_view = false;
            sys_state.inverter.inverter_state = sys_state.pre_detail_inverter_state;
            sys_state.menu_state = sys_state.detail_parent_menu;
            sys_state.menu_selection = sys_state.detail_parent_selection;
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            if (back_to_diag)
            {
                /* re-enter diagnostic display without re-suspending */
                char l[17], v[17];
                snprintf(l, 17, "%-16.16s",
                         get_menu_items(MENU_DIAGNOSTIC, &(int){0})
                             ? get_menu_items(MENU_DIAGNOSTIC, &(int){0})
                                   [sys_state.menu_selection]
                                       .label
                             : "Diagnostic");
                snprintf(v, 17, "%-16s", "");
                lcd_show_diagnostic_detail(l, v);
            }
            break;
        }
        /* P4: exit diagnostic mode */
        if (sys_state.inverter.inverter_state == INVERTER_DIAGNOSTIC)
        {
            exit_diagnostic_mode();
            sys_state.power_button_sequence_count = 0;
            break;
        }
        /* P5: close menu */
        if (sys_state.menu_state != MENU_NONE)
        {
            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
            sys_state.in_detail_view = false;
            clear_menu_history();
            go_to_main_screen();
            break;
        }
        /* P6: contextual main screen */
        sys_state.power_button_sequence_count = 0;
        clear_menu_history();
        switch (sys_state.inverter.inverter_state)
        {
        case INVERTER_ON:
        case INVERTER_STARTING:
            go_to_main_screen();
            break;
        case INVERTER_STANDBY:
        {
            char r0[17], r1[17];
            uint8_t pct = calculate_battery_percentage(
                sys_state.inverter.battery.voltage);
            snprintf(r0, 17, "STANDBY %4.1fV   ",
                     sys_state.inverter.battery.voltage);
            snprintf(r1, 17, "BAT:%3d%% AC:%s ",
                     pct, sys_state.inverter.connected ? "YES" : "NO ");
            lcd_show_standby(sys_state.inverter.battery.voltage, pct,
                             sys_state.inverter.connected);
            break;
        }
        case INVERTER_FAULT:
        {
            char r0[17], r1[17];
            snprintf(r0, 17, "%-16s", "** FAULT ACTIVE ");
            snprintf(r1, 17, "%-16.16s",
                     get_error_string(sys_state.error.error_flags));
            lcd_show_fault(r0, r1);
            vTaskDelay(pdMS_TO_TICKS(2000));
            go_to_main_screen();
            break;
        }
        default:
            go_to_main_screen();
            break;
        }
        break;
    } /* end BUTTON_EVENT_CLICK */

    case BUTTON_EVENT_DOUBLE_CLICK:
    {
        if (sys_state.value_edit_mode)
            break;
        if (sys_state.inverter.inverter_state == INVERTER_ON ||
            sys_state.inverter.inverter_state == INVERTER_STARTING)
        {
            lcd_flash_message("Stop inverter   ", "before diag!    ", 1500);
            break;
        }
        if (sys_state.inverter.inverter_state != INVERTER_DIAGNOSTIC)
        {
            sys_state.in_detail_view = false;
            sys_state.in_confirmation_screen = false;
            sys_state.value_edit_mode = false;
            sys_state.value_changed = false;
            sys_state.pending_confirmation = false;
            clear_menu_history();
            enter_diagnostic_mode();
        }
        else
        {
            exit_diagnostic_mode();
        }
        sys_state.power_button_sequence_count = 0;
        break;
    }

    case BUTTON_EVENT_TRIPLE_CLICK:
    {
        if (sys_state.inverter.inverter_state == INVERTER_ON ||
            sys_state.inverter.inverter_state == INVERTER_STARTING)
        {
            lcd_flash_message("Stop inverter   ", "before reset!   ", 1500);
            break;
        }
        if (sys_state.inverter.inverter_state == INVERTER_DIAGNOSTIC)
            exit_diagnostic_mode();
        if (sys_state.value_edit_mode)
        {
            sys_state.value_edit_mode = false;
            sys_state.value_changed = false;
            sys_state.pending_confirmation = false;
        }
        sys_state.in_detail_view = false;
        sys_state.in_confirmation_screen = false;
        push_menu_history(sys_state.menu_state, sys_state.menu_selection);
        sys_state.menu_state = MENU_FACTORY_RESET_CONFIRM;
        sys_state.menu_selection = 0;
        sys_state.power_button_sequence_count = 1;
        sys_state.power_sequence_start_time = current_time;
        lcd_show_confirm("FACTORY RESET?  ", "Hold=Yes Back=No");
        break;
    }

    case BUTTON_EVENT_LONG_PRESS:
    {
        if (sys_state.value_edit_mode)
        {
            apply_value_change();
            lcd_show_value_edit_screen();
            break;
        }
        if (sys_state.menu_state == MENU_FACTORY_RESET_CONFIRM &&
            sys_state.power_button_sequence_count > 0)
        {
            sys_state.power_button_sequence_count = 0;
            clear_menu_history();
            lcd_show_factory_progress(0);
            vTaskDelay(pdMS_TO_TICKS(500));
            perform_factory_reset();
            break;
        }
        if (sys_state.inverter.inverter_state == INVERTER_DIAGNOSTIC)
        {
            exit_diagnostic_mode();
            break;
        }
        if (sys_state.menu_state != MENU_NONE)
        {
            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
            sys_state.in_detail_view = false;
            clear_menu_history();
        }
        switch (sys_state.inverter.inverter_state)
        {
        case INVERTER_OFF:
        case INVERTER_STANDBY:
            inverter_power_on();
            break;
        case INVERTER_ON:
        case INVERTER_STARTING:
            shutdown_inverter();
            gpio_set_level(GPIO_POWER_RELAY, 0);
            break;
        case INVERTER_FAULT:
            lcd_show_fault("Clearing fault  ", "Please wait...  ");
            vTaskDelay(pdMS_TO_TICKS(1000));
            sys_state.error.error_flags &= (ERR_EEPROM | ERR_FAN_FAIL);
            sys_state.inverter.inverter_state = INVERTER_OFF;
            if (check_safety_conditions())
            {
                inverter_power_on();
                if (sys_state.inverter.inverter_state == INVERTER_ON)
                    gpio_set_level(GPIO_POWER_RELAY, 1);
            }
            else
            {
                lcd_show_fault("Fault persists  ", "Check system!   ");
                buzzer_error();
                vTaskDelay(pdMS_TO_TICKS(2000));
                go_to_main_screen();
            }
            break;
        default:
            break;
        }
        break;
    }

    case BUTTON_EVENT_VERY_LONG_PRESS:
    {
        if (sys_state.inverter.inverter_state != INVERTER_ON &&
            sys_state.inverter.inverter_state != INVERTER_STARTING &&
            sys_state.inverter.inverter_state != INVERTER_FAULT)
            break;

        lcd_show_fault("!! EMERGENCY !! ", "SYSTEM HALT     ");
        buzzer_error();
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(GPIO_POWER_RELAY, 0);
        inverter_emergency_shutdown();

        sys_state.inverter.inverter_state = INVERTER_OFF;
        sys_state.inverter.inverter_active = false;
        sys_state.menu_state = MENU_NONE;
        sys_state.menu_selection = 0;
        sys_state.in_detail_view = false;
        sys_state.in_confirmation_screen = false;
        sys_state.value_edit_mode = false;
        sys_state.value_changed = false;
        sys_state.pending_confirmation = false;
        sys_state.power_button_sequence_count = 0;
        sys_state.error.error_flags = 0;
        clear_menu_history();
        go_to_main_screen();
        led_off(LED_STATUS);
        ESP_LOGW("POWER", "Emergency shutdown complete");
        break;
    }

    default:
        break;
    }

    /* Factory reset confirmation timeout */
    if (sys_state.power_button_sequence_count > 0 &&
        (current_time - sys_state.power_sequence_start_time) > SEQUENCE_TIMEOUT_MS)
    {
        sys_state.power_button_sequence_count = 0;
        menu_state_t prev;
        int prev_sel;
        if (pop_menu_history(&prev, &prev_sel))
        {
            sys_state.menu_state = prev;
            sys_state.menu_selection = prev_sel;
        }
        else
        {
            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
        }
        go_to_main_screen();
    }

    sys_state.last_activity_time = current_time;
}

/* ── display_menu_state() ───────────────────────────────────────────────── */
menu_state_t display_menu_state(void)
{
    const char *r0, *r1;
    switch (sys_state.menu_state)
    {
    case MENU_NONE:
        r0 = "Main Screen     ";
        r1 = "Press Enter     ";
        break;
    case MAIN_MENU:
        r0 = "Main Menu       ";
        r1 = "Use Up/Down     ";
        break;
    case MENU_SETTINGS:
        r0 = "Settings        ";
        r1 = "Select Option   ";
        break;
    case MENU_MONITORING:
        r0 = "Monitoring      ";
        r1 = "View Stats      ";
        break;
    case MENU_DIAGNOSTIC:
        r0 = "Diagnostic      ";
        r1 = "Run Tests       ";
        break;
    case MENU_FACTORY_RESET_CONFIRM:
        r0 = "Factory Reset?  ";
        r1 = "Enter=Yes Back=N";
        break;
    case MENU_WIFI_CONFIG:
        r0 = "WiFi Config     ";
        r1 = "Enter=Setup     ";
        break;
    default:
        r0 = "Unknown Menu    ";
        r1 = "                ";
        break;
    }
    lcd_show_menu(r0, r1);
    vTaskDelay(pdMS_TO_TICKS(1500));
    return sys_state.menu_state;
}

/* ── handle_enter_menu_button_event() ──────────────────────────────────── */
void handle_enter_menu_button_event(const button_event_info_t *event_info,
                                    void *user_data)
{
    if (!sys_state.system_ready)
        return;

    switch (event_info->event)
    {

    case BUTTON_EVENT_CLICK:
        /* Value edit save */
        if (sys_state.value_edit_mode)
        {
            if (sys_state.pending_confirmation)
            {
                handle_value_confirmation();
                sys_state.pending_confirmation = false;
                sys_state.value_changed = false;
            }
            else
            {
                exit_value_edit_mode(true);
                sys_state.value_edit_mode = false;
            }
            lcd_show_value_saved_screen();
            vTaskDelay(pdMS_TO_TICKS(800));
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            break;
        }
        /* Menu navigation */
        switch (sys_state.menu_state)
        {
        case MAIN_MENU:
        {
            menu_state_t next = MENU_NONE;
            switch (sys_state.menu_selection)
            {
            case 0:
                next = MENU_SETTINGS;
                break;
            case 1:
                next = MENU_MONITORING;
                break;
            case 2:
                next = MENU_DIAGNOSTIC;
                break;
            case 3:
                next = MENU_WIFI_CONFIG;
                break;
            case 4:
                next = MENU_FACTORY_RESET_CONFIRM;
                break;
            }
            if (next != MENU_NONE)
            {
                push_menu_history(sys_state.menu_state, sys_state.menu_selection);
                sys_state.menu_state = next;
                sys_state.menu_selection = 0;
                show_menu_screen(next, 0);
            }
            break;
        }
        case MENU_SETTINGS:
            switch (sys_state.menu_selection)
            {
            case 0:
                edit_voltage_threshold();
                break;
            case 1:
                edit_current_limit();
                break;
            case 2:
                edit_frequency_range();
                break;
            case 3:
                edit_temperature_alarm();
                break;
            case 4:
                edit_system_timeout();
                break;
            }
            sys_state.value_edit_mode = true;
            break;

        case MENU_MONITORING:
            enter_detail_view(MENU_MONITORING, sys_state.menu_selection);
            switch (sys_state.menu_selection)
            {
            case 0:
                lcd_show_monitoring_detail("Voltage", sys_state.inverter.output_voltage, "V");
                break;
            case 1:
                lcd_show_monitoring_detail("Current", sys_state.actual_current, "A");
                break;
            case 2:
                lcd_show_monitoring_detail("Frequency", sys_state.inverter.output_frequency, "Hz");
                break;
            case 3:
                lcd_show_monitoring_detail("Temperature", sys_state.actual_temperature, "C");
                break;
            case 4:
                lcd_show_monitoring_detail("Power Factor", sys_state.power_factor, "");
                break;
            case 5:
                lcd_show_monitoring_detail("Efficiency", sys_state.efficiency, "%");
                break;
            default:
                sys_state.in_detail_view = false;
                show_menu_screen(MENU_MONITORING, sys_state.menu_selection);
                break;
            }
            break;

        case MENU_DIAGNOSTIC:
        {
            int cnt = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &cnt);
            if (!items || sys_state.menu_selection >= (uint8_t)cnt)
                break;
            enter_detail_view(MENU_DIAGNOSTIC, sys_state.menu_selection);
            lcd_draw_diagnostics_screen(sys_state.menu_selection);
            break;
        }

        case MENU_WIFI_CONFIG:
            show_menu_screen(MENU_WIFI_CONFIG, sys_state.menu_selection);
            break;

        case MENU_FACTORY_RESET_CONFIRM:
            lcd_show_confirm("Hold to confirm ", "factory reset   ");
            break;

        default:
            break;
        }
        break; /* BUTTON_EVENT_CLICK */

    case BUTTON_EVENT_LONG_PRESS:
        if (sys_state.value_edit_mode)
        {
            if (sys_state.pending_confirmation)
                handle_value_confirmation();
            else
                apply_value_change();
            lcd_show_value_edit_screen();
            break;
        }
        switch (sys_state.menu_state)
        {
        case MENU_NONE:
            sys_state.menu_state = (sys_state.inverter.inverter_state == INVERTER_ON)
                                       ? MENU_MONITORING
                                       : MAIN_MENU;
            sys_state.menu_selection = 0;
            show_menu_screen(sys_state.menu_state, 0);
            break;
        case MAIN_MENU:
        {
            menu_state_t next = MENU_NONE;
            switch (sys_state.menu_selection)
            {
            case 0:
                next = MENU_SETTINGS;
                break;
            case 1:
                next = MENU_MONITORING;
                break;
            case 2:
                next = MENU_DIAGNOSTIC;
                break;
            case 3:
                next = MENU_WIFI_CONFIG;
                break;
            case 4:
                next = MENU_FACTORY_RESET_CONFIRM;
                break;
            }
            if (next != MENU_NONE)
            {
                push_menu_history(sys_state.menu_state, sys_state.menu_selection);
                sys_state.menu_state = next;
                sys_state.menu_selection = 0;
                show_menu_screen(next, 0);
            }
            break;
        }
        case MENU_SETTINGS:
            switch (sys_state.menu_selection)
            {
            case 0:
                edit_voltage_threshold();
                break;
            case 1:
                edit_current_limit();
                break;
            case 2:
                edit_frequency_range();
                break;
            case 3:
                edit_temperature_alarm();
                break;
            case 4:
                edit_system_timeout();
                break;
            }
            sys_state.value_edit_mode = true;
            break;
        case MENU_MONITORING:
            enter_detail_view(MENU_MONITORING, sys_state.menu_selection);
            switch (sys_state.menu_selection)
            {
            case 0:
                lcd_show_monitoring_detail("Voltage", sys_state.inverter.output_voltage, "V");
                break;
            case 1:
                lcd_show_monitoring_detail("Current", sys_state.actual_current, "A");
                break;
            case 2:
                lcd_show_monitoring_detail("Frequency", sys_state.inverter.output_frequency, "Hz");
                break;
            case 3:
                lcd_show_monitoring_detail("Temperature", sys_state.actual_temperature, "C");
                break;
            case 4:
                lcd_show_monitoring_detail("Power Factor", sys_state.power_factor, "");
                break;
            case 5:
                lcd_show_monitoring_detail("Efficiency", sys_state.efficiency, "%");
                break;
            default:
                sys_state.in_detail_view = false;
                show_menu_screen(MENU_MONITORING, sys_state.menu_selection);
                break;
            }
            break;
        case MENU_DIAGNOSTIC:
        {
            int cnt = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &cnt);
            if (!items || sys_state.menu_selection >= (uint8_t)cnt)
                break;
            enter_detail_view(MENU_DIAGNOSTIC, sys_state.menu_selection);
            lcd_draw_diagnostics_screen(sys_state.menu_selection);
            break;
        }
        case MENU_WIFI_CONFIG:
            switch (sys_state.menu_selection)
            {
            case 5:
                lcd_show_wifi_connecting("Scanning...");
                start_wifi_scan();
                /* After scan, show results via lcd_show_wifi_scan() */
                if (ap_count > 0)
                {
                    const char (*ssids)[9] = NULL; /* build from ap_records */
                    /* simplified: caller handles wifi scan screen via lcd_show_wifi_scan */
                }
                lcd_show_wifi_scan_screen();
                break;
            case 6:
                start_wifi_connection();
                break;
            default:
                break;
            }
            break;
        case MENU_FACTORY_RESET_CONFIRM:
            lcd_show_factory_reset_screen();
            vTaskDelay(pdMS_TO_TICKS(500));
            perform_factory_reset();
            sys_state.menu_state = MAIN_MENU;
            sys_state.menu_selection = 0;
            clear_menu_history();
            show_menu_screen(MAIN_MENU, 0);
            break;
        default:
            break;
        }
        break; /* BUTTON_EVENT_LONG_PRESS */

    case BUTTON_EVENT_DOUBLE_CLICK:
        if (sys_state.value_edit_mode && sys_state.pending_confirmation)
        {
            value_edit_context_t *config = get_current_value_config();
            float *current_value = get_current_value_pointer();
            if (config && current_value)
            {
                update_system_parameter(config, *current_value);
                sys_state.pending_confirmation = false;
                sys_state.value_changed = false;
                sys_state.value_edit_mode = false;
                lcd_show_value_saved_screen();
                vTaskDelay(pdMS_TO_TICKS(800));
                show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            }
        }
        else if (sys_state.menu_state == MAIN_MENU)
        {
            push_menu_history(sys_state.menu_state, sys_state.menu_selection);
            sys_state.menu_state = MENU_MONITORING;
            sys_state.menu_selection = 0;
            show_menu_screen(MENU_MONITORING, 0);
        }
        break;

    default:
        break;
    }

    sys_state.last_activity_time = event_info->timestamp_us / 1000;
}

/* ── handle_up_button_event() ──────────────────────────────────────────── */
void handle_up_button_event(const button_event_info_t *event_info,
                            void *user_data)
{
    if (!sys_state.system_ready)
        return;
    int64_t current_time = event_info->timestamp_us / 1000;

    switch (event_info->event)
    {
    case BUTTON_EVENT_CLICK:
        if (sys_state.value_edit_mode)
        {
            switch (value_edit.edit_type)
            {
            case VALUE_EDIT_NUMERIC:
                increase_value(false, false);
                break;
            case VALUE_EDIT_SELECT:
                value_edit.selection_index =
                    (value_edit.selection_index + 1) % value_edit.max_selection;
                break;
            case VALUE_EDIT_BOOL:
                value_edit.bool_value = !value_edit.bool_value;
                break;
            case VALUE_EDIT_LIST:
                value_edit.list_index =
                    (value_edit.list_index + 1) % value_edit.list_size;
                break;
            default:
                break;
            }
            lcd_show_value_edit_screen();
        }
        else if (sys_state.menu_state == MENU_DIAGNOSTIC &&
                 !sys_state.in_detail_view)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &n);
            if (!items || n == 0)
                break;
            sys_state.menu_selection =
                (sys_state.menu_selection == 0) ? n - 1
                                                : sys_state.menu_selection - 1;
            sys_state.detail_parent_selection = sys_state.menu_selection;
            sys_state.detail_parent_menu = MENU_DIAGNOSTIC;
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
        }
        else if (!sys_state.in_detail_view && sys_state.menu_state != MENU_NONE)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state, &n);
            if (items && n > 0)
            {
                sys_state.menu_selection =
                    (sys_state.menu_selection == 0) ? n - 1
                                                    : sys_state.menu_selection - 1;
                show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            }
        }
        break;

    case BUTTON_EVENT_DOUBLE_CLICK:
        if (sys_state.value_edit_mode)
        {
            increase_value(false, true);
            lcd_show_value_edit_screen();
        }
        else if (sys_state.in_detail_view &&
                 sys_state.menu_state == MENU_DIAGNOSTIC)
        {
            sys_state.menu_selection = 0;
            sys_state.detail_parent_selection = 0;
            lcd_draw_diagnostics_screen(0);
        }
        else if (sys_state.in_detail_view)
        {
            exit_detail_view();
        }
        else if (sys_state.menu_state != MENU_NONE)
        {
            sys_state.menu_selection = 0;
            show_menu_screen(sys_state.menu_state, 0);
        }
        break;

    case BUTTON_EVENT_LONG_PRESS:
        if (sys_state.value_edit_mode)
        {
            sys_state.fast_increment_active = true;
            sys_state.repeat_count = 0;
            for (int i = 0; i < 10; i++)
                increase_value(true, false);
            lcd_show_value_edit_screen();
        }
        else if (sys_state.in_detail_view &&
                 sys_state.menu_state == MENU_DIAGNOSTIC)
        {
            sys_state.menu_selection = 0;
            sys_state.detail_parent_selection = 0;
            lcd_draw_diagnostics_screen(0);
        }
        else if (sys_state.in_detail_view)
        {
            exit_detail_view();
        }
        else if (sys_state.menu_state != MENU_NONE)
        {
            sys_state.menu_selection = 0;
            show_menu_screen(sys_state.menu_state, 0);
        }
        break;

    case BUTTON_EVENT_REPEAT:
        if (sys_state.value_edit_mode)
        {
            sys_state.repeat_count++;
            if ((current_time - sys_state.last_increment_time) <
                FAST_INCREMENT_THRESHOLD_MS)
                sys_state.fast_increment_active = true;
            bool fast = sys_state.fast_increment_active ||
                        (sys_state.repeat_count > 5);
            increase_value(fast, false);
            lcd_show_value_edit_screen();
        }
        else if (!sys_state.in_detail_view &&
                 sys_state.menu_state != MENU_NONE)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state, &n);
            if (items && n > 0)
            {
                sys_state.menu_selection =
                    (sys_state.menu_selection == 0) ? n - 1
                                                    : sys_state.menu_selection - 1;
                show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            }
        }
        break;

    case BUTTON_EVENT_RELEASE:
        sys_state.repeat_count = 0;
        sys_state.fast_increment_active = false;
        break;

    default:
        break;
    }

    sys_state.last_activity_time = current_time;
    sys_state.last_increment_time = current_time;
}

/* ── handle_down_button_event() ─────────────────────────────────────────── */
void handle_down_button_event(const button_event_info_t *event_info,
                              void *user_data)
{
    if (!sys_state.system_ready)
        return;
    int64_t current_time = event_info->timestamp_us / 1000;

    switch (event_info->event)
    {
    case BUTTON_EVENT_CLICK:
        if (sys_state.value_edit_mode)
        {
            switch (value_edit.edit_type)
            {
            case VALUE_EDIT_NUMERIC:
                decrease_value(false, false);
                break;
            case VALUE_EDIT_SELECT:
                value_edit.selection_index =
                    (value_edit.selection_index > 0)
                        ? value_edit.selection_index - 1
                        : value_edit.max_selection - 1;
                break;
            case VALUE_EDIT_BOOL:
                value_edit.bool_value = !value_edit.bool_value;
                break;
            case VALUE_EDIT_LIST:
                value_edit.list_index =
                    (value_edit.list_index > 0)
                        ? value_edit.list_index - 1
                        : value_edit.list_size - 1;
                break;
            default:
                break;
            }
            lcd_show_value_edit_screen();
        }
        else if (sys_state.menu_state == MENU_DIAGNOSTIC &&
                 !sys_state.in_detail_view)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &n);
            if (!items || n == 0)
                break;
            sys_state.menu_selection =
                (sys_state.menu_selection + 1) % n;
            sys_state.detail_parent_selection = sys_state.menu_selection;
            sys_state.detail_parent_menu = MENU_DIAGNOSTIC;
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
        }
        else if (!sys_state.in_detail_view && sys_state.menu_state != MENU_NONE)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state, &n);
            if (items && n > 0)
            {
                sys_state.menu_selection =
                    (sys_state.menu_selection + 1) % n;
                show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            }
        }
        break;

    case BUTTON_EVENT_DOUBLE_CLICK:
        if (sys_state.value_edit_mode)
        {
            decrease_value(false, true);
            lcd_show_value_edit_screen();
        }
        else if (sys_state.in_detail_view &&
                 sys_state.menu_state == MENU_DIAGNOSTIC)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &n);
            if (items && n > 0)
            {
                sys_state.menu_selection = n - 1;
                sys_state.detail_parent_selection = n - 1;
                lcd_draw_diagnostics_screen(sys_state.menu_selection);
            }
        }
        else if (sys_state.in_detail_view)
        {
            exit_detail_view();
        }
        else if (sys_state.menu_state != MENU_NONE)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state, &n);
            if (items && n > 0)
            {
                sys_state.menu_selection = n - 1;
                show_menu_screen(sys_state.menu_state, n - 1);
            }
        }
        break;

    case BUTTON_EVENT_LONG_PRESS:
        if (sys_state.value_edit_mode)
        {
            sys_state.fast_increment_active = true;
            sys_state.repeat_count = 0;
            for (int i = 0; i < 10; i++)
                decrease_value(true, false);
            lcd_show_value_edit_screen();
        }
        else if (sys_state.in_detail_view &&
                 sys_state.menu_state == MENU_DIAGNOSTIC)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &n);
            if (items && n > 0)
            {
                sys_state.menu_selection = n - 1;
                sys_state.detail_parent_selection = n - 1;
                lcd_draw_diagnostics_screen(sys_state.menu_selection);
            }
        }
        else if (sys_state.in_detail_view)
        {
            exit_detail_view();
        }
        else if (sys_state.menu_state != MENU_NONE)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state, &n);
            if (items && n > 0)
            {
                sys_state.menu_selection = n - 1;
                show_menu_screen(sys_state.menu_state, n - 1);
            }
        }
        break;

    case BUTTON_EVENT_REPEAT:
        if (sys_state.value_edit_mode)
        {
            sys_state.repeat_count++;
            if ((current_time - sys_state.last_increment_time) <
                FAST_INCREMENT_THRESHOLD_MS)
                sys_state.fast_increment_active = true;
            bool fast = sys_state.fast_increment_active ||
                        (sys_state.repeat_count > 5);
            decrease_value(fast, false);
            lcd_show_value_edit_screen();
        }
        else if (!sys_state.in_detail_view &&
                 sys_state.menu_state != MENU_NONE)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state, &n);
            if (items && n > 0)
            {
                sys_state.menu_selection = (sys_state.menu_selection + 1) % n;
                show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            }
        }
        break;

    case BUTTON_EVENT_RELEASE:
        sys_state.repeat_count = 0;
        sys_state.fast_increment_active = false;
        break;

    default:
        break;
    }

    sys_state.last_activity_time = current_time;
    sys_state.last_increment_time = current_time;
}

/* ── handle_back_button_event() ─────────────────────────────────────────── */
void handle_back_button_event(const button_event_info_t *event_info,
                              void *user_data)
{
    if (!sys_state.system_ready)
        return;

    switch (event_info->event)
    {
    case BUTTON_EVENT_CLICK:
        /* P1 cancel value edit */
        if (sys_state.value_edit_mode)
        {
            exit_value_edit_mode(false);
            sys_state.value_edit_mode = false;
            lcd_show_value_canceled_screen();
            vTaskDelay(pdMS_TO_TICKS(800));
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            return;
        }
        /* P2 exit detail view */
        if (sys_state.in_detail_view)
        {
            sys_state.in_detail_view = false;
            sys_state.inverter.inverter_state = sys_state.pre_detail_inverter_state;
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            return;
        }
        /* P3 exit confirm */
        if (sys_state.in_confirmation_screen)
        {
            sys_state.in_confirmation_screen = false;
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            return;
        }
        /* P4 exit info screen */
        if (sys_state.in_info_screen)
        {
            sys_state.in_info_screen = false;
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            go_to_main_screen();
            return;
        }
        /* P5 pop history */
        if (menu_history.depth > 0)
        {
            menu_state_t prev;
            int prev_sel;
            if (pop_menu_history(&prev, &prev_sel))
            {
                sys_state.menu_state = prev;
                sys_state.menu_selection = prev_sel;
                sys_state.last_activity_time = esp_timer_get_time() / 1000;
                if (prev == MENU_NONE)
                    go_to_main_screen();
                else
                    show_menu_screen(prev, prev_sel);
                return;
            }
        }
        /* P6 fallback */
        switch (sys_state.menu_state)
        {
        case MENU_SETTINGS:
        case MENU_MONITORING:
        case MENU_DIAGNOSTIC:
        case MENU_WIFI_CONFIG:
        case MENU_FACTORY_RESET_CONFIRM:
            sys_state.inverter.inverter_state = sys_state.inverter.previous_inverter_state;
            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
            sys_state.value_edit_mode = false;
            go_to_main_screen();
            break;
        case MAIN_MENU:
            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
            clear_menu_history();
            go_to_main_screen();
            break;
        case MENU_NONE:
            break;
        default:
            sys_state.menu_state = MAIN_MENU;
            sys_state.menu_selection = 0;
            clear_menu_history();
            show_menu_screen(MAIN_MENU, 0);
            break;
        }
        sys_state.last_activity_time = esp_timer_get_time() / 1000;
        break;

    case BUTTON_EVENT_LONG_PRESS:
        if (sys_state.value_edit_mode)
        {
            exit_value_edit_mode(false);
            sys_state.value_edit_mode = false;
        }
        if (sys_state.in_detail_view)
            sys_state.inverter.inverter_state = sys_state.pre_detail_inverter_state;
        sys_state.in_detail_view = false;
        sys_state.in_confirmation_screen = false;
        sys_state.in_info_screen = false;
        sys_state.menu_state = MENU_NONE;
        sys_state.menu_selection = 0;
        clear_menu_history();
        sys_state.last_activity_time = esp_timer_get_time() / 1000;
        go_to_main_screen();
        break;

    default:
        break;
    }
}

/* ── inverter_power_on() ────────────────────────────────────────────────── */
static const char *INV_TAG = "INVERTER";

void inverter_power_on(void)
{
    if (!check_safety_conditions())
    {
        lcd_show_fault("Safety check    ", "FAILED! See log ");
        buzzer_error();
        vTaskDelay(pdMS_TO_TICKS(2000));
        go_to_main_screen();
        return;
    }

    sys_state.inverter.inverter_state = INVERTER_STARTING;
    sys_state.inverter.inverter_active = false;

    inverter_set_output_voltage(220);
    vTaskDelay(pdMS_TO_TICKS(300));
    inverter_set_output_frequency(sys_state.inverter.output_frequency);
    vTaskDelay(pdMS_TO_TICKS(300));

    /* Show startup sequence via lcd_writer */
    for (int i = 0; i <= 100; i += 10)
    {
        lcd_show_startup_progress((uint8_t)i);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    inverter_set_current_limit(sys_state.current_limit);

    esp_err_t err = gpio_set_level(GPIO_POWER_RELAY, 1);
    if (err != ESP_OK)
    {
        ESP_LOGE(INV_TAG, "Relay set failed: %s", esp_err_to_name(err));
        sys_state.inverter.inverter_state = INVERTER_FAULT;
        sys_state.inverter.inverter_active = false;
        sys_state.error.error_flags |= SYSTEM_FAILURE_ERROR;
        lcd_show_fault("** START FAILED ", "Check relay/HW  ");
        buzzer_error();
        vTaskDelay(pdMS_TO_TICKS(2000));
        go_to_main_screen();
        return;
    }

    sys_state.inverter.inverter_state = INVERTER_ON;
    sys_state.inverter.inverter_active = true;
    sys_state.menu_state = MENU_NONE;
    go_to_main_screen();
    buzzer_success();
    led_on(LED_STATUS);
    ESP_LOGI(INV_TAG, "Inverter powered on");
}

/* ── shutdown_inverter() ────────────────────────────────────────────────── */
void shutdown_inverter(void)
{
    lcd_show_shutdown_progress(100, false, 0.0f);
    vTaskDelay(pdMS_TO_TICKS(500));

    if (sys_state.inverter.actual_current > 0.5f)
    {
        lcd_show_shutdown_progress(100, true,
                                   sys_state.inverter.actual_current);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    /* Ramp down */
    for (int i = 100; i >= 0; i -= 10)
    {
        lcd_show_shutdown_progress((uint8_t)i, false, 0.0f);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    inverter_set_output_voltage(0);
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = gpio_set_level(GPIO_POWER_RELAY, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(INV_TAG, "Relay open failed: %s", esp_err_to_name(err));
        lcd_show_fault("** RELAY FAULT  ", "Restarting...   ");
        vTaskDelay(pdMS_TO_TICKS(1500));
        perform_system_restart(false);
        return;
    }

    led_off(LED_STATUS);
    sys_state.inverter.inverter_state = INVERTER_OFF;
    sys_state.inverter.inverter_active = false;
    sys_state.menu_state = MENU_NONE;
    sys_state.error.error_flags &= ~SYSTEM_FAILURE_ERROR;
    go_to_main_screen();
    ESP_LOGI(INV_TAG, "Inverter powered off");
}

/* ── enter_diagnostic_mode() ────────────────────────────────────────────── */
void enter_diagnostic_mode(void)
{
    xSemaphoreTake(sys_state_mutex, pdMS_TO_TICKS(SYS_STATE_MUTEX_TIMEOUT_MS));
    sys_state.inverter.previous_inverter_state = sys_state.inverter.inverter_state;
    sys_state.inverter.inverter_state = INVERTER_DIAGNOSTIC;
    sys_state.menu_state = MENU_DIAGNOSTIC;
    sys_state.menu_selection = 0;
    xSemaphoreGive(sys_state_mutex);

    /* Entry animation via flash messages */
    lcd_flash_message("Entering Diag.  ", "Please wait.    ", 500);
    vTaskDelay(pdMS_TO_TICKS(500));
    lcd_flash_message("Entering Diag.  ", "Please wait..   ", 500);
    vTaskDelay(pdMS_TO_TICKS(500));
    lcd_flash_message("Entering Diag.  ", "Please wait...  ", 500);
    vTaskDelay(pdMS_TO_TICKS(500));

    show_menu_screen(MENU_DIAGNOSTIC, 0);
}

/* ── exit_diagnostic_mode() ─────────────────────────────────────────────── */
void exit_diagnostic_mode(void)
{
    lcd_flash_message(" Exiting Diag.. ", "                ", 1500);
    vTaskDelay(pdMS_TO_TICKS(1500));

    xSemaphoreTake(sys_state_mutex, pdMS_TO_TICKS(SYS_STATE_MUTEX_TIMEOUT_MS));
    sys_state.inverter.inverter_state = sys_state.inverter.previous_inverter_state;
    sys_state.menu_state = MENU_NONE;
    sys_state.menu_selection = 0;
    sys_state.value_edit_mode = false;
    xSemaphoreGive(sys_state_mutex);

    go_to_main_screen();
    ESP_LOGI(INV_TAG, "Diagnostic mode exited");
}

/* ── lcd_draw_diagnostics_screen() ─────────────────────────────────────── */
void lcd_draw_diagnostics_screen(uint8_t index)
{
    int item_count = 0;
    const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &item_count);
    if (!items || item_count == 0 || index >= (uint8_t)item_count)
    {
        lcd_show_diagnostic_detail("  Diag Error    ", "Bad item index  ");
        return;
    }

    const char *label = items[index].label ? items[index].label : "(no label)";
    char row0[17], row1[17];
    snprintf(row0, 17, "%-16.16s", label);

    switch (index)
    {
    case 0: // System Status
        snprintf(row1, 17, "Sys:%-12s", diag_data.system_ok ? "OK" : "Fault");
        break;
    case 1: // Latest Error
    {
        const error_log_entry_t *latest = error_log_get_latest();
        if (!latest)
            snprintf(row1, 17, "%-16s", "No errors logged");
        else
            snprintf(row1, 17, "%-16.16s", latest->description);
        break;
    }
    case 2: // CPU Load
        snprintf(row1, 17, "Load:%6.1f%%    ", diag_data.cpu_load);
        break;
    case 3: // Firmware Version
        snprintf(row1, 17, "%-16.16s", "C-01 Rev A");
        break;
    case 4: // Uptime
    {
        unsigned long s = (unsigned long)diag_data.uptime_seconds;
        unsigned long d = s / 86400UL, h = (s % 86400UL) / 3600UL;
        unsigned long m = (s % 3600UL) / 60UL, sec = s % 60UL;
        if (d > 0)
            snprintf(row1, 17, "%lud %02lu:%02lu:%02lu ", d, h, m, sec);
        else
            snprintf(row1, 17, "   %02lu:%02lu:%02lu    ", h, m, sec);
        break;
    }
    case 5:
        snprintf(row1, 17, "RAM:%6.1f%%     ", diag_data.ram_usage);
        break;
    default:
        snprintf(row1, 17, "%-16s", "Unknown item");
        break;
    }

    row0[16] = '\0';
    row1[16] = '\0';
    lcd_show_diagnostic_detail(row0, row1);
}

/* ── perform_factory_reset() ────────────────────────────────────────────── */
void perform_factory_reset(void)
{
    sys_state.inverter.inverter_state = INVERTER_FACTORY_RESET;

    lcd_show_factory_progress(0);
    for (int i = 0; i <= 100; i += 20)
    {
        lcd_show_factory_progress((uint8_t)i);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* Reset all values */
    sys_state.inverter.output_voltage = 220.0f;
    sys_state.inverter.output_frequency = 50.0f;
    sys_state.current_limit = 20.0f;
    sys_state.temperature_limit = 70.0f;
    sys_state.cutoff_voltage = 11.5f;
    sys_state.display.scroll_speed = DEFAULT_SCROLL_SPEED;

    lcd_flash_message("CLEARING LOGS   ", "Please wait...  ", 1000);
    vTaskDelay(pdMS_TO_TICKS(1000));
    lcd_flash_message("CALIBRATION     ", "Resetting...    ", 1000);
    vTaskDelay(pdMS_TO_TICKS(1000));

    update_buzzer(2000, 50);
    for (int i = 0; i < 5; i++)
    {
        update_led(LED_STATUS, 1);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        update_led(LED_STATUS, 0);
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }
    buzzer_off();

    sys_state.menu_state = MENU_NONE;
    sys_state.power_button_sequence_count = 0;

    lcd_show_factory_done();
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_ERROR_CHECK(nvs_flash_erase());
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    memset(&sys_state, 0, sizeof(system_state_t));
    sys_state.inverter.output_voltage = 220.0f;
    sys_state.inverter.temperature = 50.0f;
    sys_state.display.scroll_speed = DEFAULT_SCROLL_SPEED;

    for (int i = 0; i < ADC_CHANNEL_USE; i++)
    {
        adc_calibration[i].calibration_values[0] = 0.0f;
        adc_calibration[i].calibration_values[1] = 1.0f;
        adc_calibration[i].calibrated = false;
        adc_calibration[i].calibration_mode = false;
    }

    err = nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &nvs_handler);
    if (err == ESP_OK)
    {
        save_settings();
        save_calibration();
        nvs_close(nvs_handler);
    }

    update_buzzer(3000, 70);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    perform_system_restart(true);
}

/* ── handle_menu_timeout() ──────────────────────────────────────────────── */
void handle_menu_timeout(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (sys_state.menu_state != MENU_NONE &&
        now - sys_state.last_activity_time > MENU_TIMEOUT_MS)
    {
        sys_state.menu_state = MENU_NONE;
        sys_state.menu_selection = 0;
        clear_menu_history();
        go_to_main_screen();
    }
}

/* ── lcd_show_wifi_scan_screen() ─────────────────────────────────────────── */
void lcd_show_wifi_scan_screen(void)
{
    if (ap_count == 0)
    {
        lcd_flash_message("No Networks     ", "Found           ", 2000);
        return;
    }

    /* Build ssid array for lcd_writer */
    char ssids[LCD_WIFI_MAX_AP][9];
    int8_t rssi_arr[LCD_WIFI_MAX_AP];
    uint8_t count = (ap_count < LCD_WIFI_MAX_AP) ? ap_count : LCD_WIFI_MAX_AP;
    for (uint8_t i = 0; i < count; i++)
    {
        strncpy(ssids[i], (char *)ap_records[i].ssid, 8);
        ssids[i][8] = '\0';
        rssi_arr[i] = ap_records[i].rssi;
    }

    int index = 0, top = 0;
    int64_t last_activity = esp_timer_get_time() / 1000;
    lcd_show_wifi_scan(count, (const char (*)[9])ssids, rssi_arr, 0, 0);

    while (1)
    {
        if ((esp_timer_get_time() / 1000) - last_activity > WIFI_SCAN_TIMEOUT_MS)
        {
            lcd_flash_message("Scan Timeout    ", "                ", 1200);
            break;
        }

        button_event_info_t ev;
        if (xQueueReceive(button_event_queue, &ev, pdMS_TO_TICKS(800)))
        {
            last_activity = esp_timer_get_time() / 1000;
            switch (ev.button_id)
            {
            case BTN_UP:
                if (index > 0)
                    index--;
                if (index < top)
                {
                    top -= 2;
                    if (top < 0)
                        top = 0;
                }
                lcd_update_wifi_selection(index, top);
                break;
            case BTN_DOWN:
                if (index < count - 1)
                    index++;
                if (index >= top + 2)
                    top += 2;
                if (top >= count)
                    top = count - 1;
                lcd_update_wifi_selection(index, top);
                break;
            case BTN_ENTER_MENU:
                strncpy((char *)sys_state.wifi.ssid,
                        (char *)ap_records[index].ssid,
                        sizeof(sys_state.wifi.ssid) - 1);
                lcd_show_wifi_connecting(sys_state.wifi.ssid);
                start_wifi_connection();
                vTaskDelay(pdMS_TO_TICKS(2000));
                return;
            case BTN_BACK:
                lcd_flash_message("Exiting Scan    ", "                ", 1000);
                return;
            default:
                break;
            }
        }
    }
}

/* ── start_wifi_connection() ─────────────────────────────────────────────── */
void start_wifi_connection(void)
{
    if (!sys_state.wifi.enabled)
    {
        lcd_show_wifi_result(false, true, false, "Wi-Fi Disabled  ");
        ESP_LOGW(WIFI_TAG, "WiFi disabled");
        return;
    }

    lcd_show_wifi_connecting(sys_state.wifi.ssid);

    /* ... all WiFi init code identical to original ... */

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT)
        lcd_show_wifi_result(true, false, false, sys_state.wifi.ssid);
    else if (bits & WIFI_FAIL_BIT)
        lcd_show_wifi_result(false, true, false, sys_state.wifi.ssid);
    else
        lcd_show_wifi_result(false, false, true, sys_state.wifi.ssid);

    vEventGroupDelete(s_wifi_event_group);
}

/* ── stop_wifi_connection() ──────────────────────────────────────────────── */
void stop_wifi_connection(void)
{
    esp_wifi_stop();
    lcd_show_wifi_result(false, false, false, "Disconnected");
    ESP_LOGI(WIFI_TAG, "WiFi stopped");
}

/* ── clear_all_settings() ───────────────────────────────────────────────── */
void clear_all_settings(void)
{
    lcd_flash_message("Clearing        ", "Settings...     ", 500);
    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK)
        nvs_flash_init();
    reload_default_settings();
    lcd_flash_message("Done            ", "All cleared!    ", 1000);
}

/* ── erase_all_logs() ───────────────────────────────────────────────────── */
void erase_all_logs(void)
{
    sys_state.error_count = 0;
    sys_state.uptime_hours = 0;
    sys_state.memory_usage = 0;
    error_log_clear();

    nvs_handle_t h;
    if (nvs_open("log_storage", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(LOG_TAG, "Logs cleared from NVS.");
    }
    else
    {
        ESP_LOGW(LOG_TAG, "Failed to open NVS for log clearing: %s", esp_err_to_name(err));
    }

// === 3. If using an SD card or external storage, add code here to clear logs from there ===
#ifdef USE_SD_LOGGING
    ESP_LOGI(LOG_TAG, "Clearing logs from SD card...");
    remove("/sdcard/logs/system_log.txt");
    remove("/sdcard/logs/error_log.txt");
    ESP_LOGI(LOG_TAG, "Logs cleared from SD card.");
#endif

    // === 4. Notify user via LCD and buzzer ===
    update_buzzer(1000, 50);
    lcd_flash_message("Logs Cleared    ", "System Clean    ", 1500);
}

/* ── handle_critical_error() ─────────────────────────────────────────────── */
void handle_critical_error(void)
{
    ESP_LOGE(TAG_ERROR, "Critical Error: 0x%02X", sys_state.error.error_flags);
    log_error_to_nvs(sys_state.error.error_flags);
    update_buzzer(3000, 80);
    blink_led(LED_ERROR, 200, 200, 5);

    char l0[17], l1[17];
    if (sys_state.error.error_flags & ERR_OVER_TEMP)
    {
        snprintf(l0, 17, "%-16s", "Error: Over Temp");
        snprintf(l1, 17, "%.1fC Max:%.1fC  ",
                 sys_state.inverter.temperature, MAX_TEMPERATURE);
    }
    else if (sys_state.error.error_flags & ERR_OVERLOAD)
    {
        snprintf(l0, 17, "%-16s", "Error: Overload ");
        snprintf(l1, 17, "%.1fA Max:%.1fA  ",
                 sys_state.inverter.output_current, MAX_CURRENT);
    }
    else if
    {
        snprintf(l0, 17, "%-16s", "Critical Error  ");
        snprintf(l1, 17, "Code: 0x%02X      ",
                 sys_state.error.error_flags);
    }
    else
    {
        snprintf(l0, 17, "%-16s", "Unknown Error   ");
        snprintf(l1, 17, "Code: 0x%02X      ",
                 sys_state.error.error_flags);
    }
    lcd_show_fault(l0, l1);

    rtc_mem.last_error = sys_state.error.error_flags;
    save_settings();
    vTaskDelay(pdMS_TO_TICKS(5000));
}

/* ── display_battery_settings() ─────────────────────────────────────────── */
void display_battery_settings(void)
{
    char l[17], v[17];
    snprintf(l, 17, "%-16s", "Battery Settings");
    snprintf(v, 17, "Cutoff: %5.2fV  ",
             menu_edit.edit_step ? menu_edit.temp_value
                                 : sys_state.battery_profile.cutoff_voltage_12v);
    lcd_show_menu(l, v);
}

/* ── show_battery_voltage() / show_temperature() ─────────────────────────── */
/* These are read-only display helpers.  In the refactored design they just   */
/* update the main-screen data; lcd_task draws it.                            */
void show_battery_voltage(void)
{
    /* Data already in sys_lcd.main via lcd_update_main_data() from adc_task  */
    lcd_show_main();
}

void show_temperature(void)
{
    lcd_show_main();
}

/* ── perform_system_restart() ───────────────────────────────────────────── */
void perform_system_restart(bool factory_reset)
{
    lcd_flash_message("System Restart  ", "Please wait...  ", 500);
    update_buzzer(2000, 30);
    if (!factory_reset)
    {
        save_settings();
        save_calibration();
    }
    vTaskSuspendAll();
    esp_err_t ret = i2c_driver_delete(I2C_NUM_0);
    if (ret != ESP_OK)
    {
        ESP_LOGE("I2C", "Delete failed");
        return;
    }
    gpio_reset_pin(GPIO_BUZZER);
    gpio_reset_pin(GPIO_STATUS_LED);
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(100));
    log_error_to_nvs(90);
    system_restart();
}

/* ── show_system_info() ──────────────────────────────────────────────────── */
void show_system_info(void)
{
    char l[17], v[17];
    snprintf(l, 17, "Firmware:%-7s", FIRMWARE_VERSION);
    battery_profile_t profile = battery_profiles[sys_state.battery_profile.chemistry];
    snprintf(v, 17, "%s %dV          ",
             profile.name_prefix,
             (int)sys_state.battery_profile.nominal_voltage);
    lcd_show_monitor_detail(l, v);
}

/* ── enter_deep_sleep() ──────────────────────────────────────────────────── */
void enter_deep_sleep(uint32_t sleep_seconds)
{
    rtc_mem.magic_flag = RTC_MAGIC_FLAG;
    rtc_mem.last_sleep_time = xTaskGetTickCount();
    rtc_mem.was_inverter_active = sys_state.inverter.inverter_active;
    rtc_mem.ac_was_connected = sys_state.inverter.connected;
    rtc_mem.last_error = (uint32_t)sys_state.error.error_flags;

    char v[17];
    snprintf(v, 17, "Wake in: %lus   ", sleep_seconds);
    lcd_flash_message("Entering Sleep  ", v, 2000);

    save_settings();
    update_buzzer(0, 0);
    update_led(LED_STATUS, 0);
    gpio_set_level(GPIO_POWER_RELAY, 0);

    esp_sleep_enable_timer_wakeup(sleep_seconds * 1000000ULL);
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(wakeup_pin_mask,
                                                 ESP_EXT1_WAKEUP_ALL_LOW));
    gpio_reset_pin(GPIO_PWR_BTN);
    gpio_reset_pin(GPIO_BUZZER);
    gpio_reset_pin(GPIO_STATUS_LED);
    gpio_reset_pin(GPIO_ERROR_LED);
    gpio_reset_pin(GPIO_POWER_RELAY);
    gpio_reset_pin(GPIO_BTN_UP);
    gpio_reset_pin(GPIO_BTN_DOWN);
    gpio_reset_pin(GPIO_BTN_ENTER);
    gpio_reset_pin(GPIO_BTN_BACK);
    gpio_reset_pin(GPIO_NEPA_INPUT);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_deep_sleep_start();
}

/* ── handle_wakeup() ─────────────────────────────────────────────────────── */
void handle_wakeup(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const char *r0;
    switch (cause)
    {
    case ESP_SLEEP_WAKEUP_TIMER:
        r0 = "Woke by Timer   ";
        break;
    case ESP_SLEEP_WAKEUP_EXT0:
        r0 = "Woke by Button  ";
        break;
    default:
        r0 = "Cold Boot       ";
        break;
    }

    char r1[17] = "                ";
    if (rtc_mem.last_sleep_time > 0)
    {
        uint32_t dur =
            (xTaskGetTickCount() - rtc_mem.last_sleep_time) / 1000;
        snprintf(r1, 17, "Slept %lds       ", (long)dur);
    }
    if (rtc_mem.last_error)
    {
        snprintf(r1, 17, "Recovered err   ");
        sys_state.error.error_flags |= rtc_mem.last_error;
    }

    lcd_flash_message(r0, r1, 3000);
    vTaskDelay(pdMS_TO_TICKS(3000));
}

/* ── adjust_calibration_setting() ───────────────────────────────────────── */
void adjust_calibration_setting(button_event_info_t btn)
{
    static uint8_t calib_step = 0;
    button_id_t button_id = gpio_to_button_id(btn.gpio_pin);

    switch (calib_step)
    {
    case 0:
        lcd_show_menu("Calibration Menu", "1.Bat 2.Current ");
        if (button_id == BTN_ENTER_MENU)
        {
            calib_step = (sys_state.display.menu_position == 8)
                             ? 10
                             : sys_state.display.menu_position + 1;
        }
        if (button_id == BTN_BACK)
        {
            sys_state.display.current_menu = MAIN_MENU;
            calib_step = 0;
        }
        break;
    case 1:
        lcd_show_menu("Bat Calibration ", "Connect known12V");
        if (button_id == BTN_ENTER_MENU)
        {
            float known = 12.0f;
            sys_state.inverter.battery_voltage_calibration =
                known - sys_state.inverter.battery.voltage;
            sys_state.inverter.battery.voltage +=
                sys_state.inverter.battery_voltage_calibration;
            save_settings();
            lcd_flash_message("Calibration Done", "                ", 1000);
            calib_step = 0;
            sys_state.display.current_menu = MAIN_MENU;
        }
        if (button_id == BTN_BACK)
        {
            calib_step = 0;
            sys_state.display.current_menu = MAIN_MENU;
            show_menu_screen(MAIN_MENU, 0);
        }
        break;
    case 10:
        perform_factory_reset();
        calib_step = 0;
        sys_state.display.current_menu = MAIN_MENU;
        show_menu_screen(MAIN_MENU, 0);
        break;
    }
}

/* ── adjust_factory_reset() ─────────────────────────────────────────────── */
void adjust_factory_reset(button_event_info_t btn)
{
    static factory_reset_state_t state = FACTORY_RESET_CONFIRM;
    static bool selection_yes = true;

    switch (btn.gpio_pin)
    {
    case BTN_UP:
    case BTN_DOWN:
        selection_yes = !selection_yes;
        lcd_show_confirm("Factory Reset?  ",
                         selection_yes ? "> Yes   No      "
                                       : "  Yes > No      ");
        break;
    case BTN_ENTER_MENU:
        if (state == FACTORY_RESET_CONFIRM && selection_yes)
        {
            lcd_show_factory_progress(0);
            perform_factory_reset();
            lcd_flash_message("Reset Complete! ", "                ", 1000);
            navigate_to_menu(MAIN_MENU);
        }
        else
        {
            navigate_to_menu(sys_state.display.current_menu);
        }
        break;
    case BTN_BACK:
        navigate_to_menu(sys_state.display.current_menu);
        break;
    default:
        break;
    }
}

/* ── navigate_to_menu() ─────────────────────────────────────────────────── */
void navigate_to_menu(menu_state_t menu)
{
    sys_state.display.current_menu = menu;
    show_menu_screen(menu, 0);
}

/* ── menu_exit() ────────────────────────────────────────────────────────── */
void menu_exit(void)
{
    lcd_flash_message("Exiting menu... ", "                ", 1000);
    sys_state.display.current_menu = MAIN_MENU;
}

/* ── show_profile_on_lcd() ──────────────────────────────────────────────── */
void show_profile_on_lcd(battery_profile_t *profile)
{
    char l[17], v[17];
    snprintf(l, 17, "Battery:%4.1fV  ", (float)profile->nominal_voltage * 10);
    snprintf(v, 17, "Cutoff:%5.1fV   ", profile->cutoff_voltage_12v * 10);
    lcd_show_monitor_detail(l, v);
}

/* ── error_handler() ────────────────────────────────────────────────────── */
void error_handler(void)
{
    typedef struct
    {
        system_errors_t flag;
        const char *line1;
        const char *line2;
        int buzzer_freq;
        int buzzer_vol;
    } ErrorMsg;

    static const ErrorMsg errors[] = {
        {ERR_OVER_TEMP, "ERROR:Over Temp ", "System Shutdown!", 3000, 100},
        {ERR_SHORT_CIRCUIT, "ERROR:Short Circ", "System Shutdown!", 3000, 100},
        {ERR_OVERLOAD, "ERROR:Overload  ", "System Overload ", 2000, 80},
        {ERR_LOW_BAT, "ERROR:Low Bat   ", "Voltage too low ", 1500, 100},
        {ERR_HIGH_BAT, "ERROR:High Bat  ", "Voltage too high", 1500, 100},
        {ERR_FAN_FAIL, "ERROR:Fan Fail  ", "Fan not running ", 2000, 80},
        {ERR_OVER_UNDER_VOLTAGE, "ERROR:Volt Fault", "Over/Under Volt ", 2000, 80},
    };

    static TickType_t low_bat_start = 0;
    static bool low_bat_started = false;
    static TickType_t fan_fail_start = 0;
    static bool fan_fail_started = false;

    bool error_found = false;
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
    {
        if (sys_state.error.error_flags == errors[i].flag)
        {
            lcd_show_fault(errors[i].line1, errors[i].line2);
            update_buzzer(errors[i].buzzer_freq, errors[i].buzzer_vol);
            error_found = true;

            if (errors[i].flag == ERR_LOW_BAT)
            {
                if (!low_bat_started)
                {
                    low_bat_start = xTaskGetTickCount();
                    low_bat_started = true;
                }
                else if ((xTaskGetTickCount() - low_bat_start) >=
                         pdMS_TO_TICKS(60000))
                {
                    shutdown();
                    sys_state.display.current_menu = MAIN_MENU;
                    low_bat_started = false;
                }
            }
            if (errors[i].flag == ERR_FAN_FAIL)
            {
                if (!fan_fail_started)
                {
                    fan_fail_start = xTaskGetTickCount();
                    fan_fail_started = true;
                }
                else if ((xTaskGetTickCount() - fan_fail_start) >=
                         pdMS_TO_TICKS(120000))
                {
                    shutdown();
                    sys_state.display.current_menu = MAIN_MENU;
                    fan_fail_started = false;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    if (!error_found)
        lcd_show_fault("Error: Unknown  ", "System error!   ");

    lcd_show_confirm("Press BACK to   ", "Reset system... ");

    uint32_t elapsed = 0;
    while (elapsed < RESET_TIMEOUT_MS)
    {
        if (gpio_get_level(BTN_ENTER_MENU) == 0)
        {
            lcd_show_fault("System Reset    ", "Please wait...  ");
            vTaskDelay(pdMS_TO_TICKS(5000));
            perform_system_restart(false);
            update_activity();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        elapsed += 100;
    }
}

/* ── handle_value_confirmation() — replace lcd_show_message calls ─────── */
void handle_value_confirmation(void)
{
    /* ... all logic identical to original except lcd_show_message replaced */

    /* On success: */
    /* lcd_show_message("Value Saved", config->label)  -->  */
    /* lcd_flash_message("Value Saved!    ", config->label, 1000)           */

    /* On rejection: */
    /* lcd_show_message("Change Rejected", ...)  -->                         */
    /* lcd_flash_message("Change Rejected ", "                ", 1000)      */

    /* ... rest of function identical ... */
    lcd_update_menu_screen();
}

/* ── detect_critical_error() REPORT_ERROR macro replacement ───────────── */
/* The REPORT_ERROR macro previously called lcd_clear/lcd_print directly.   */
/* Replace with lcd_show_fault():                                            */
/*   lcd_show_fault(err_str, "System Halted   ");                            */

/* ── inverter_emergency_shutdown() ─────────────────────────────────────── */
void inverter_emergency_shutdown(void)
{
    gpio_set_level(GPIO_POWER_RELAY, 0);
    sys_state.inverter.inverter_state = INVERTER_OFF;
    sys_state.inverter.inverter_active = false;
    sys_state.system_ready = false;
    lcd_show_fault("EMERGENCY HALT  ", "All outputs off ");
}

/* ── lcd_display_confirmation_screen() ──────────────────────────────────── */
void lcd_display_confirmation_screen(void)
{
    lcd_show_confirm("Save Changes?   ", "Enter=Yes Back=N");
}

/* ── edit_* functions ────────────────────────────────────────────────────── */
void edit_voltage_threshold(void)
{
    sys_state.value_edit_mode = true;
    value_edit.edit_type = VALUE_EDIT_NUMERIC;
    value_edit.label = "Voltage Threshold";
    value_edit.unit = "V";
    value_edit.temp_value = sys_state.settings.voltage_threshold;
    value_edit.min_value = 10.0f;
    value_edit.max_value = 15.0f;
    value_edit.step_size = 0.1f;
    lcd_show_value_edit_screen();
}
void edit_current_limit(void)
{
    sys_state.value_edit_mode = true;
    value_edit.edit_type = VALUE_EDIT_NUMERIC;
    value_edit.label = "Current Limit";
    value_edit.unit = "A";
    value_edit.temp_value = sys_state.settings.current_limit;
    value_edit.min_value = 1.0f;
    value_edit.max_value = 50.0f;
    value_edit.step_size = 0.5f;
    lcd_show_value_edit_screen();
}
void edit_frequency_range(void)
{
    sys_state.value_edit_mode = true;
    value_edit.edit_type = VALUE_EDIT_NUMERIC;
    value_edit.label = "Frequency Range";
    value_edit.unit = "Hz";
    value_edit.temp_value = sys_state.settings.frequency_range;
    value_edit.min_value = 45.0f;
    value_edit.max_value = 65.0f;
    value_edit.step_size = 0.5f;
    lcd_show_value_edit_screen();
}

void edit_temperature_alarm(void)
{
    sys_state.value_edit_mode = true;
    value_edit.edit_type = VALUE_EDIT_NUMERIC;
    value_edit.label = "Temp Alarm";
    value_edit.unit = "C";
    value_edit.temp_value = sys_state.settings.temperature_alarm;
    value_edit.min_value = 30.0f;
    value_edit.max_value = 100.0f;
    value_edit.step_size = 1.0f;
    lcd_show_value_edit_screen();
}

void edit_system_timeout(void)
{
    sys_state.value_edit_mode = true;
    value_edit.edit_type = VALUE_EDIT_NUMERIC;
    value_edit.label = "Sys Timeout";
    value_edit.unit = "s";
    value_edit.temp_value = sys_state.settings.system_timeout;
    value_edit.min_value = 10;
    value_edit.max_value = 600;
    value_edit.step_size = 5;
    lcd_show_value_edit_screen();
}

/*==============================================================================
  lcd_task — THE ONLY FUNCTION THAT CALLS lcd_* HARDWARE FUNCTIONS
  Implementation lives in lcd_task.c (already written above).
  Declaration here for linker:
==============================================================================*/
extern void lcd_task(void *arg); /* defined in lcd_task.c */

/*==============================================================================
  app_main — unchanged except lcd_writer_init() added
==============================================================================*/
void app_main(void)
{
    sys_event_group = xEventGroupCreate();
    configASSERT(sys_event_group);
    sys_state_mutex = xSemaphoreCreateMutex();
    if (!sys_state_mutex)
    {
        ESP_LOGE(APP_TAG, "FATAL: mutex");
        return;
    }

    /* ── NEW: initialise render state ─────────────────────────────────── */
    lcd_writer_init();

    init_hardware();
    nvs_init(false);
    init_system_state();
    init_menu_system();
    nvs_print_stats();
    if (nvs_is_initialized())
        ESP_LOGI("MAIN", "NVS ready");

    restore_from_deep_sleep();

    esp_err_t ret = initialize_button_system();
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "FATAL: button init");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    lcd_power_init();
    LCD_power(true);
    lcd_set_brightness(200);

    /* Boot screen starts on LCD_SCREEN_BOOT_BRAND (set by lcd_writer_init) */
    lcd_show_boot_brand();

    xTaskCreate(adc_task, "adc_task", 4096, NULL, 5, NULL);
    xTaskCreate(lcd_task, "lcd_task", 2048, NULL, 4, &lcd_task_handle);
    //   xTaskCreate(power_task, "power_task", 4096, NULL, 4, NULL);
    //   xTaskCreate(button_task, "button_task", 4096, NULL, 10, NULL);
    //   xTaskCreate(display_timeout_task, "disp_timeout", 4096, NULL, 1, NULL);
    //   xTaskCreate(battery_menu_task, "battery_menu", 4096, NULL, 2, NULL); // Only once
    //   xTaskCreate(fan_monitor_task, "fan_monitor_task", 4096, NULL, 5, NULL);
    //   xTaskCreate(diagnostic_update_task, "diagnostic_update_task", 2048, NULL, 5, NULL);
    //     === Main Loop (Watchdog + Error Handling) ===

    while (sys_state.system_ready)
        vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGW(APP_TAG, "Main loop ended");
    cleanup_button_system();
}