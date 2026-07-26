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
#include <driver/i2c.h>
#include "lcd.h"
#include "lcd_writer.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/event_groups.h"
#include "security/security.h"
#include "security/factory_reset.h"

#include "sdkconfig.h"
#include "rom/ets_sys.h"
#include "utils.h" // Added MIN & MAX
#include "battery_soc.h"
#include <stdbool.h>
#include <button_controller.h>

/* ── NEW: lcd_state / lcd_writer headers ──────────────────────────────── */
#include "lcd_state.h"
#include "lcd_writer.h"

// WIFI Credentials
#include "esp_wifi.h"
#include "lcd_watchdog.h"
#include "lcd_flash_queue.h"

// SECURITY
#include "security/change_pin_flow.h"
#include "security/protection.h"
// System state
#include "system_state.h"
// Events
#include "events/event_dispatcher.h"
#include "events/system_events.h"
#include "events/fault_log.h"
#include "events/protection_handler.h"

// Utility
#include "utility/led.h"
#include "utility/buzzer.h"

/* ── All original #defines─────────────────────────────── */
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
#define CONFIG_USE_ADC 1
#define CONFIG_USE_BUTTONS 1
#define CONFIG_USE_LCD 1
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

// Tags
#define tag "LCD"
#define NVS_LOAD_TAG "NVS_LOAD"

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
#define LONG_PRESS_MS 1000
#define HOLD_PRESS_MS 500
#define VERY_LONG_PRESS_MS 2000
#define DOUBLE_CLICK_MS 400
#define REPEAT_INITIAL_DELAY_MS 500
#define REPEAT_INTERVAL_MS 100
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

// voltage_divider_ratio = (R1 + R2) / R2
// Example: For 12V input with 10kΩ and 3.3kΩ resistors: ratio = (10k + 3.3k) / 3.3k = 4.03
// Example: For direct connection (no divider): ratio = 1.0

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
#define LCD_SCROLL_DELAY_MS 300
#define LCD_BLINK_INTERVAL_MS 500
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
#define NVS_TAG "NVS"
#define MAX_ERROR_LOG_ENTRIES 10

// static int fan_disconnect_count = 0;
// static bool fan_connected_last_state = true;
// static const char *TAG = "FAN_MONITOR";

// Queue for button events
QueueHandle_t button_event_queue;
QueueHandle_t protection_event_queue;
QueueHandle_t g_event_subscriber_queue[EVENT_SUB_COUNT];

// ADC Configuration
typedef enum
{
    ADC_LOW_BATTERY = ADC_CHANNEL_6,             // GPIO 34
    ADC_OVER_UNDER_VOLTAGE = ADC_CHANNEL_0,      // GPIO 36
    ADC_BATTERY_VOLTAGE = ADC_CHANNEL_7,         // GPIO 35
    ADC_FAN = ADC_CHANNEL_5,                     // GPIO 33
    ADC_INVERTER_OUTPUT_VOLTAGE = ADC_CHANNEL_4, // GPIO 32
    ADC_CHANNEL_COUNTER
} adc_channels_t;

// ADC2 Configuration
typedef enum
{
    ADC2_CHANNEL_0 = ADC_CHANNEL_0,
    ADC2_CHANNEL_1 = ADC_CHANNEL_1,
    ADC2_CHANNEL_2 = ADC_CHANNEL_2,
    ADC2_CHANNEL_3 = ADC_CHANNEL_3,
    ADC2_CHANNEL_COUNTER
} adc2_channels_t;

// used to sample adc 10x
typedef struct
{
    float buffer[FILTER_DEPTH];
    float sum;
    uint8_t index;
} MovingAverageFilter;

// static bool editing_battery_menu = false;
//  end
/* ── Global handles (unchanged) ───────────────────────────────────────── */

lcd_render_state_t sys_lcd;
SemaphoreHandle_t sys_state_mutex;
TaskHandle_t lcd_task_handle = NULL;
active_flash_t s_active_flash = {0};
bool g_system_initialized = false;

const uint64_t wakeup_pin_mask =
    (1ULL << WAKEUP_BUTTON_1) | (1ULL << WAKEUP_BUTTON_2);

static bool nvs_initialized = false;

// battery submenu started here
typedef enum
{
    BATTERY_CUTOFF_MENU_MAIN,
    BATTERY_CUTOFF_MENU_SELECT_TYPE,
    BATTERY_CUTOFF_MENU_SELECT_VOLTAGE,
    BATTERY_CUTOFF_MENU_EDIT_CUTOFF
} battery_cutoff_menu_state_t;

// Base profiles array (12V reference)
static const battery_profile_t battery_profiles[BATTERY_TYPE_COUNT] = {
    [BATTERY_LEAD_ACID] = {
        .name_prefix = "L-Acid",
        .chemistry = BATTERY_CHEMISTRY_LEAD_ACID,
        .depth_of_discharge_max = 0.50f,
        .bulk_charge_voltage_12v = 14.4f,
        .float_charge_voltage_12v = 13.5f,
        .equalization_voltage_12v = 15.5f,
        .full_charge_voltage_12v = 12.7f,
        .nominal_voltage_actual_12v = 12.0f,
        .low_voltage_warning_12v = 11.8f,
        .low_voltage_alarm_12v = 11.5f,
        .cutoff_voltage_12v = 10.5f,
        .cutoff_voltage_min_12v = 10.0f,
        .high_battery_voltage_12v = 15.5f,
        .recharge_voltage_12v = 11.5f,
        .overvoltage_protection_12v = 15.5f,
        .undervoltage_protection_12v = 10.0f,
        .max_charge_current_per_100ah = 20.0f,
        .max_discharge_current_per_100ah = 100.0f,
        .recommended_charge_current_per_100ah = 10.0f,
        .trickle_charge_current_per_100ah = 1.0f,
        .temp_coefficient = -0.03f,
        .operating_temp_min = -20.0f,
        .operating_temp_max = 50.0f,
        .charge_temp_min = -10.0f,
        .charge_temp_max = 50.0f,
        .discharge_temp_min = -20.0f,
        .discharge_temp_max = 50.0f,
        .bulk_charge_timeout_min = 480,
        .absorption_time_min = 180,
        .float_time_min = 1440,
        .equalization_time_min = 120,
        .equalization_interval_days = 30,
        .soc_full_threshold = 98.0f,
        .soc_empty_threshold = 5.0f,
        .soc_low_warning = 20.0f,
        .internal_resistance_mohm_12v = 5.0f,
        .cycle_life_rated = 500,
        .cycle_life_dod = 0.50f,
        .self_discharge_rate = 5.0f,
        .charge_termination_current_per_100ah = 1.0f,
        .charge_termination_voltage_12v = 14.4f,
        .charge_termination_timeout = 480.0f,
        .requires_balancing = false,
        .balance_start_voltage_12v = 0.0f,
        .balance_voltage_delta_max = 0.0f,
        .has_bms = false,
        .requires_external_bms = false,
        .supports_temperature_comp = true,
        .charge_efficiency = 0.85f,
        .discharge_efficiency = 0.90f,
        .requires_equalization = true,
        .is_sealed = false,
        .maintenance_interval_days = 30},

    [BATTERY_AGM] = {.name_prefix = "AGM", .chemistry = BATTERY_CHEMISTRY_AGM, .depth_of_discharge_max = 0.70f, .bulk_charge_voltage_12v = 14.4f, .float_charge_voltage_12v = 13.6f, .equalization_voltage_12v = 14.8f, .full_charge_voltage_12v = 12.8f, .nominal_voltage_actual_12v = 12.0f, .low_voltage_warning_12v = 11.8f, .low_voltage_alarm_12v = 11.3f, .cutoff_voltage_12v = 10.8f, .cutoff_voltage_min_12v = 10.5f, .recharge_voltage_12v = 11.5f, .overvoltage_protection_12v = 15.0f, .undervoltage_protection_12v = 10.5f, .max_charge_current_per_100ah = 30.0f, .max_discharge_current_per_100ah = 100.0f, .recommended_charge_current_per_100ah = 15.0f, .trickle_charge_current_per_100ah = 1.5f, .temp_coefficient = -0.025f, .operating_temp_min = -20.0f, .operating_temp_max = 50.0f, .charge_temp_min = -10.0f, .charge_temp_max = 50.0f, .discharge_temp_min = -20.0f, .discharge_temp_max = 50.0f, .bulk_charge_timeout_min = 360, .absorption_time_min = 120, .float_time_min = 1440, .equalization_time_min = 60, .equalization_interval_days = 90, .soc_full_threshold = 98.0f, .soc_empty_threshold = 5.0f, .soc_low_warning = 20.0f, .internal_resistance_mohm_12v = 4.0f, .cycle_life_rated = 800, .cycle_life_dod = 0.50f, .self_discharge_rate = 3.0f, .charge_termination_current_per_100ah = 1.5f, .charge_termination_voltage_12v = 14.4f, .charge_termination_timeout = 360.0f, .requires_balancing = false, .balance_start_voltage_12v = 0.0f, .balance_voltage_delta_max = 0.0f, .has_bms = false, .requires_external_bms = false, .supports_temperature_comp = true, .charge_efficiency = 0.90f, .discharge_efficiency = 0.95f, .requires_equalization = true, .is_sealed = true, .maintenance_interval_days = 90},

    [BATTERY_GEL] = {.name_prefix = "GEL", .chemistry = BATTERY_CHEMISTRY_GEL, .depth_of_discharge_max = 0.80f, .bulk_charge_voltage_12v = 14.1f, .float_charge_voltage_12v = 13.5f, .equalization_voltage_12v = 14.4f, .full_charge_voltage_12v = 12.8f, .nominal_voltage_actual_12v = 12.0f, .low_voltage_warning_12v = 11.9f, .low_voltage_alarm_12v = 11.5f, .cutoff_voltage_12v = 11.0f, .cutoff_voltage_min_12v = 10.5f, .recharge_voltage_12v = 11.8f, .overvoltage_protection_12v = 14.8f, .undervoltage_protection_12v = 10.5f, .max_charge_current_per_100ah = 25.0f, .max_discharge_current_per_100ah = 80.0f, .recommended_charge_current_per_100ah = 12.0f, .trickle_charge_current_per_100ah = 1.2f, .temp_coefficient = -0.028f, .operating_temp_min = -20.0f, .operating_temp_max = 55.0f, .charge_temp_min = -10.0f, .charge_temp_max = 50.0f, .discharge_temp_min = -20.0f, .discharge_temp_max = 55.0f, .bulk_charge_timeout_min = 420, .absorption_time_min = 180, .float_time_min = 1440, .equalization_time_min = 90, .equalization_interval_days = 120, .soc_full_threshold = 98.0f, .soc_empty_threshold = 5.0f, .soc_low_warning = 20.0f, .internal_resistance_mohm_12v = 6.0f, .cycle_life_rated = 1200, .cycle_life_dod = 0.50f, .self_discharge_rate = 2.0f, .charge_termination_current_per_100ah = 1.2f, .charge_termination_voltage_12v = 14.1f, .charge_termination_timeout = 420.0f, .requires_balancing = false, .balance_start_voltage_12v = 0.0f, .balance_voltage_delta_max = 0.0f, .has_bms = false, .requires_external_bms = false, .supports_temperature_comp = true, .charge_efficiency = 0.88f, .discharge_efficiency = 0.93f, .requires_equalization = true, .is_sealed = true, .maintenance_interval_days = 120},

    [BATTERY_LIFEPO4] = {.name_prefix = "LiFePO4", .chemistry = BATTERY_CHEMISTRY_LIFEPO4, .depth_of_discharge_max = 0.95f, .bulk_charge_voltage_12v = 14.6f, .float_charge_voltage_12v = 13.6f, .equalization_voltage_12v = 0.0f, .full_charge_voltage_12v = 14.4f, .nominal_voltage_actual_12v = 12.8f, .low_voltage_warning_12v = 12.4f, .low_voltage_alarm_12v = 12.0f, .cutoff_voltage_12v = 11.0f, .cutoff_voltage_min_12v = 10.0f, .recharge_voltage_12v = 12.4f, .overvoltage_protection_12v = 15.0f, .undervoltage_protection_12v = 10.0f, .max_charge_current_per_100ah = 50.0f, .max_discharge_current_per_100ah = 100.0f, .recommended_charge_current_per_100ah = 30.0f, .trickle_charge_current_per_100ah = 0.0f, .temp_coefficient = 0.0f, .operating_temp_min = -20.0f, .operating_temp_max = 60.0f, .charge_temp_min = 0.0f, .charge_temp_max = 45.0f, .discharge_temp_min = -20.0f, .discharge_temp_max = 60.0f, .bulk_charge_timeout_min = 240, .absorption_time_min = 60, .float_time_min = 0, .equalization_time_min = 0, .equalization_interval_days = 0, .soc_full_threshold = 98.0f, .soc_empty_threshold = 5.0f, .soc_low_warning = 20.0f, .internal_resistance_mohm_12v = 15.0f, .cycle_life_rated = 3000, .cycle_life_dod = 0.80f, .self_discharge_rate = 3.0f, .charge_termination_current_per_100ah = 2.0f, .charge_termination_voltage_12v = 14.4f, .charge_termination_timeout = 240.0f, .requires_balancing = true, .balance_start_voltage_12v = 14.0f, .balance_voltage_delta_max = 50.0f, .has_bms = true, .requires_external_bms = false, .supports_temperature_comp = false, .charge_efficiency = 0.95f, .discharge_efficiency = 0.98f, .requires_equalization = false, .is_sealed = true, .maintenance_interval_days = 0},

    [BATTERY_LITHIUM_ION] = {.name_prefix = "Li-Ion", .chemistry = BATTERY_CHEMISTRY_LITHIUM_ION, .depth_of_discharge_max = 0.90f, .bulk_charge_voltage_12v = 12.6f, .float_charge_voltage_12v = 12.4f, .equalization_voltage_12v = 0.0f, .full_charge_voltage_12v = 12.6f, .nominal_voltage_actual_12v = 11.1f, .low_voltage_warning_12v = 10.5f, .low_voltage_alarm_12v = 10.0f, .cutoff_voltage_12v = 9.0f, .cutoff_voltage_min_12v = 8.4f, .recharge_voltage_12v = 10.5f, .overvoltage_protection_12v = 13.0f, .undervoltage_protection_12v = 8.4f, .max_charge_current_per_100ah = 50.0f, .max_discharge_current_per_100ah = 100.0f, .recommended_charge_current_per_100ah = 20.0f, .trickle_charge_current_per_100ah = 0.0f, .temp_coefficient = 0.0f, .operating_temp_min = -10.0f, .operating_temp_max = 50.0f, .charge_temp_min = 0.0f, .charge_temp_max = 45.0f, .discharge_temp_min = -10.0f, .discharge_temp_max = 50.0f, .bulk_charge_timeout_min = 180, .absorption_time_min = 30, .float_time_min = 0, .equalization_time_min = 0, .equalization_interval_days = 0, .soc_full_threshold = 99.0f, .soc_empty_threshold = 5.0f, .soc_low_warning = 20.0f, .internal_resistance_mohm_12v = 25.0f, .cycle_life_rated = 1000, .cycle_life_dod = 0.80f, .self_discharge_rate = 5.0f, .charge_termination_current_per_100ah = 1.0f, .charge_termination_voltage_12v = 12.6f, .charge_termination_timeout = 180.0f, .requires_balancing = true, .balance_start_voltage_12v = 12.4f, .balance_voltage_delta_max = 30.0f, .has_bms = true, .requires_external_bms = false, .supports_temperature_comp = false, .charge_efficiency = 0.92f, .discharge_efficiency = 0.96f, .requires_equalization = false, .is_sealed = true, .maintenance_interval_days = 0},

    [BATTERY_NIMH] = {.name_prefix = "NiMH", .chemistry = BATTERY_CHEMISTRY_NIMH, .depth_of_discharge_max = 0.90f, .bulk_charge_voltage_12v = 16.8f, .float_charge_voltage_12v = 14.4f, .equalization_voltage_12v = 0.0f, .full_charge_voltage_12v = 16.0f, .nominal_voltage_actual_12v = 14.4f, .low_voltage_warning_12v = 12.0f, .low_voltage_alarm_12v = 11.0f, .cutoff_voltage_12v = 10.0f, .cutoff_voltage_min_12v = 9.0f, .recharge_voltage_12v = 12.0f, .overvoltage_protection_12v = 17.5f, .undervoltage_protection_12v = 9.0f, .max_charge_current_per_100ah = 50.0f, .max_discharge_current_per_100ah = 200.0f, .recommended_charge_current_per_100ah = 10.0f, .trickle_charge_current_per_100ah = 1.0f, .temp_coefficient = -0.01f, .operating_temp_min = -20.0f, .operating_temp_max = 60.0f, .charge_temp_min = 0.0f, .charge_temp_max = 45.0f, .discharge_temp_min = -20.0f, .discharge_temp_max = 60.0f, .bulk_charge_timeout_min = 600, .absorption_time_min = 60, .float_time_min = 60, .equalization_time_min = 0, .equalization_interval_days = 0, .soc_full_threshold = 95.0f, .soc_empty_threshold = 10.0f, .soc_low_warning = 25.0f, .internal_resistance_mohm_12v = 100.0f, .cycle_life_rated = 500, .cycle_life_dod = 0.80f, .self_discharge_rate = 30.0f, .charge_termination_current_per_100ah = 0.5f, .charge_termination_voltage_12v = 16.8f, .charge_termination_timeout = 600.0f, .requires_balancing = false, .balance_start_voltage_12v = 0.0f, .balance_voltage_delta_max = 0.0f, .has_bms = false, .requires_external_bms = false, .supports_temperature_comp = true, .charge_efficiency = 0.70f, .discharge_efficiency = 0.85f, .requires_equalization = false, .is_sealed = true, .maintenance_interval_days = 0}};

/**
 * @brief Generate a battery profile for a specific voltage system and capacity
 *
 * @param battery_type Type of battery chemistry
 * @param voltage_system System voltage (12V, 24V, 48V, 96V)
 * @param capacity_ah Battery capacity in ampere-hours
 * @param profile_out Pointer to output battery profile structure
 * @return true if profile generated successfully, false otherwise
 */
bool battery_generate_profile(battery_type_t battery_type,
                              voltage_system_t voltage_system,
                              uint16_t capacity_ah,
                              battery_profile_t *profile_out)
{
    if (battery_type >= BATTERY_TYPE_COUNT || profile_out == NULL)
    {
        return false;
    }

    const battery_profile_t *base = &battery_profiles[battery_type];
    float voltage_multiplier = (float)voltage_system / 12.0f;
    float capacity_multiplier = (float)capacity_ah / 100.0f;
    uint8_t name_prefix_len = snprintf(profile_out->name_prefix, sizeof(profile_out->name_prefix), "%s %dV %uAh", base->name_prefix, voltage_system, capacity_ah);
    if (name_prefix_len >= sizeof(profile_out->name_prefix))
    {
        // Name was truncated
        profile_out->name_prefix[sizeof(profile_out->name_prefix) - 1] = '\0';
    }
    // Copy basic info
    profile_out->chemistry = base->chemistry;
    profile_out->nominal_voltage = voltage_system;
    profile_out->profile_id = base->profile_id;

    // Capacity
    profile_out->capacity_ah = capacity_ah;
    profile_out->usable_capacity_ah = (uint16_t)(capacity_ah * base->depth_of_discharge_max);
    profile_out->depth_of_discharge_max = base->depth_of_discharge_max;
    profile_out->high_battery_voltage_12v = base->high_battery_voltage_12v * voltage_multiplier;

    // Scale voltages by voltage multiplier
    profile_out->bulk_charge_voltage_12v = base->bulk_charge_voltage_12v * voltage_multiplier;
    profile_out->float_charge_voltage_12v = base->float_charge_voltage_12v * voltage_multiplier;
    profile_out->equalization_voltage_12v = base->equalization_voltage_12v * voltage_multiplier;
    profile_out->full_charge_voltage_12v = base->full_charge_voltage_12v * voltage_multiplier;
    profile_out->nominal_voltage_actual_12v = base->nominal_voltage_actual_12v * voltage_multiplier;
    profile_out->low_voltage_warning_12v = base->low_voltage_warning_12v * voltage_multiplier;
    profile_out->low_voltage_alarm_12v = base->low_voltage_alarm_12v * voltage_multiplier;
    profile_out->cutoff_voltage_12v = base->cutoff_voltage_12v * voltage_multiplier;
    profile_out->cutoff_voltage_min_12v = base->cutoff_voltage_min_12v * voltage_multiplier;
    profile_out->recharge_voltage_12v = base->recharge_voltage_12v * voltage_multiplier;
    profile_out->overvoltage_protection_12v = base->overvoltage_protection_12v * voltage_multiplier;
    profile_out->undervoltage_protection_12v = base->undervoltage_protection_12v * voltage_multiplier;
    profile_out->charge_termination_voltage_12v = base->charge_termination_voltage_12v * voltage_multiplier;
    profile_out->balance_start_voltage_12v = base->balance_start_voltage_12v * voltage_multiplier;

    // Scale currents by capacity
    profile_out->max_charge_current_per_100ah = base->max_charge_current_per_100ah * capacity_multiplier;
    profile_out->max_discharge_current_per_100ah = base->max_discharge_current_per_100ah * capacity_multiplier;
    profile_out->recommended_charge_current_per_100ah = base->recommended_charge_current_per_100ah * capacity_multiplier;
    profile_out->trickle_charge_current_per_100ah = base->trickle_charge_current_per_100ah * capacity_multiplier;
    profile_out->charge_termination_current_per_100ah = base->charge_termination_current_per_100ah * capacity_multiplier;
    // Temperature parameters (no scaling needed)
    profile_out->temp_coefficient = base->temp_coefficient;
    profile_out->operating_temp_min = base->operating_temp_min;
    profile_out->operating_temp_max = base->operating_temp_max;
    profile_out->charge_temp_min = base->charge_temp_min;
    profile_out->charge_temp_max = base->charge_temp_max;
    profile_out->discharge_temp_min = base->discharge_temp_min;
    profile_out->discharge_temp_max = base->discharge_temp_max;

    // Timing parameters (no scaling needed)
    profile_out->bulk_charge_timeout_min = base->bulk_charge_timeout_min;
    profile_out->absorption_time_min = base->absorption_time_min;
    profile_out->float_time_min = base->float_time_min;
    profile_out->equalization_time_min = base->equalization_time_min;
    profile_out->equalization_interval_days = base->equalization_interval_days;

    // SOC parameters (no scaling needed)
    profile_out->soc_full_threshold = base->soc_full_threshold;
    profile_out->soc_empty_threshold = base->soc_empty_threshold;
    profile_out->soc_low_warning = base->soc_low_warning;

    // Internal resistance scales with voltage, inversely with capacity
    profile_out->internal_resistance_mohm_12v = (base->internal_resistance_mohm_12v * voltage_multiplier) / capacity_multiplier;

    // Cycle life (no scaling needed)
    profile_out->cycle_life_rated = base->cycle_life_rated;
    profile_out->cycle_life_dod = base->cycle_life_dod;

    // Self-discharge (no scaling needed)
    profile_out->self_discharge_rate = base->self_discharge_rate;

    // Charge termination
    profile_out->charge_termination_timeout = base->charge_termination_timeout;

    // Balancing
    profile_out->requires_balancing = base->requires_balancing;
    profile_out->balance_voltage_delta_max = base->balance_voltage_delta_max;

    // Safety features (no scaling needed)
    profile_out->has_bms = base->has_bms;
    profile_out->requires_external_bms = base->requires_external_bms;
    profile_out->supports_temperature_comp = base->supports_temperature_comp;

    // Efficiency (no scaling needed)
    profile_out->charge_efficiency = base->charge_efficiency;
    profile_out->discharge_efficiency = base->discharge_efficiency;

    // Maintenance (no scaling needed)
    profile_out->requires_equalization = base->requires_equalization;
    profile_out->is_sealed = base->is_sealed;
    profile_out->maintenance_interval_days = base->maintenance_interval_days;

    return true;
}

/**
 * @brief Print battery profile details
 */
void battery_print_profile(const battery_profile_t *profile)
{
    printf("\n========================================\n");
    printf("BATTERY PROFILE: %s\n", profile->name_prefix);
    printf("========================================\n");

    printf("\n--- IDENTIFICATION ---\n");
    printf("Chemistry: %d\n", profile->chemistry);
    printf("Nominal Voltage: %.2fV\n", profile->nominal_voltage_actual_12v);
    printf("Capacity: %fAh (Usable: %dAh)\n", profile->capacity_ah, profile->usable_capacity_ah);
    printf("Max DoD: %.1f%%\n", profile->depth_of_discharge_max * 100.0f);

    printf("\n--- CHARGING VOLTAGES ---\n");
    printf("Bulk Charge: %.2fV\n", profile->bulk_charge_voltage_12v);
    printf("Float Charge: %.2fV\n", profile->float_charge_voltage_12v);
    printf("Equalization: %.2fV\n", profile->equalization_voltage_12v);
    printf("Full Charge: %.2fV\n", profile->full_charge_voltage_12v);

    printf("\n--- OPERATING VOLTAGES ---\n");
    printf("Nominal (Actual): %.2fV\n", profile->nominal_voltage_actual_12v);
    printf("Low Warning: %.2fV\n", profile->low_voltage_warning_12v);
    printf("Low Alarm: %.2fV\n", profile->low_voltage_alarm_12v);
    printf("Cutoff: %.2fV\n", profile->cutoff_voltage_12v);
    printf("Cutoff Min: %.2fV\n", profile->cutoff_voltage_min_12v);
    printf("Recharge: %.2fV\n", profile->recharge_voltage_12v);

    printf("\n--- PROTECTION VOLTAGES ---\n");
    printf("Overvoltage: %.2fV\n", profile->overvoltage_protection_12v);
    printf("Undervoltage: %.2fV\n", profile->undervoltage_protection_12v);

    printf("\n--- CURRENT LIMITS ---\n");
    printf("Max Charge: %.2fA\n", profile->max_charge_current_per_100ah);
    printf("Max Discharge: %.2fA\n", profile->max_discharge_current_per_100ah);
    printf("Recommended Charge: %.2fA\n", profile->recommended_charge_current_per_100ah);
    printf("Trickle Charge: %.2fA\n", profile->trickle_charge_current_per_100ah);

    printf("\n--- TEMPERATURE LIMITS ---\n");
    printf("Operating: %.1f°C to %.1f°C\n",
           profile->operating_temp_min, profile->operating_temp_max);
    printf("Charging: %.1f°C to %.1f°C\n",
           profile->charge_temp_min, profile->charge_temp_max);
    printf("Discharging: %.1f°C to %.1f°C\n",
           profile->discharge_temp_min, profile->discharge_temp_max);
    printf("Temp Coefficient: %.3fV/°C/cell\n", profile->temp_coefficient);

    printf("\n--- CHARGING TIMING ---\n");
    printf("Bulk Timeout: %d min\n", profile->bulk_charge_timeout_min);
    printf("Absorption: %d min\n", profile->absorption_time_min);
    printf("Float: %d min\n", profile->float_time_min);
    printf("Equalization: %d min (every %d days)\n",
           profile->equalization_time_min, profile->equalization_interval_days);

    printf("\n--- STATE OF CHARGE ---\n");
    printf("Full Threshold: %.1f%%\n", profile->soc_full_threshold);
    printf("Empty Threshold: %.1f%%\n", profile->soc_empty_threshold);
    printf("Low Warning: %.1f%%\n", profile->soc_low_warning);

    printf("\n--- BATTERY CHARACTERISTICS ---\n");
    printf("Internal Resistance: %.2f mΩ\n", profile->internal_resistance_mohm_12v);
    printf("Cycle Life: %d cycles @ %.0f%% DoD\n",
           profile->cycle_life_rated, profile->cycle_life_dod * 100.0f);
    printf("Self-discharge: %.1f%%/month\n", profile->self_discharge_rate);
    printf("Charge Efficiency: %.1f%%\n", profile->charge_efficiency * 100.0f);
    printf("Discharge Efficiency: %.1f%%\n", profile->discharge_efficiency * 100.0f);

    printf("\n--- SAFETY FEATURES ---\n");
    printf("Has BMS: %s\n", profile->has_bms ? "Yes" : "No");
    printf("Requires External BMS: %s\n", profile->requires_external_bms ? "Yes" : "No");
    printf("Temperature Compensation: %s\n", profile->supports_temperature_comp ? "Yes" : "No");
    printf("Requires Balancing: %s\n", profile->requires_balancing ? "Yes" : "No");
    if (profile->requires_balancing)
    {
        printf("  Balance Start Voltage: %.2fV\n", profile->balance_start_voltage_12v);
        printf("  Max Cell Delta: %.1f mV\n", profile->balance_voltage_delta_max);
    }

    printf("\n--- MAINTENANCE ---\n");
    printf("Requires Equalization: %s\n", profile->requires_equalization ? "Yes" : "No");
    printf("Sealed Battery: %s\n", profile->is_sealed ? "Yes" : "No");
    printf("Maintenance Interval: %d days\n", profile->maintenance_interval_days);

    printf("========================================\n\n");
}

/**
 * @brief Save battery configuration to NVS
 */
bool battery_save_configuration(battery_type_t battery_type,
                                voltage_system_t voltage_system,
                                uint16_t capacity_ah)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Open NVS in read-write mode
    err = nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        printf("ERROR: Failed to open NVS for writing!\n");
        return false;
    }

    // Save battery type
    err = nvs_set_u8(nvs_handle, BATTERY_TYPE_KEY, (uint8_t)battery_type);
    if (err != ESP_OK)
    {
        printf(": Failed to save battery type!\n");
        nvs_close(nvs_handle);
        return false;
    }

    // Save voltage system
    err = nvs_set_u8(nvs_handle, BATTERY_VOLTAGE_SYSTEM_KEY, (uint8_t)voltage_system);
    if (err != ESP_OK)
    {
        printf("ERROR: Failed to save voltage system!\n");
        nvs_close(nvs_handle);
        return false;
    }

    // Save capacity
    err = nvs_set_u16(nvs_handle, BATTERY_CAPACITY_KEY, capacity_ah);
    if (err != ESP_OK)
    {
        printf("ERROR: Failed to save capacity!\n");
        nvs_close(nvs_handle);
        return false;
    }

    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK)
    {
        printf("ERROR: Failed to commit NVS changes!\n");
        nvs_close(nvs_handle);
        return false;
    }

    nvs_close(nvs_handle);

    return true;
}

/**
 * @brief Get battery profile from NVS or generate default
 */
bool battery_load_profile(battery_profile_t *profile_out)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    uint8_t battery_type;
    uint8_t voltage_system;
    uint16_t capacity_ah;

    // Open NVS
    err = nvs_open(NVS_NS_SYSTEM, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        printf("ERROR: Failed to open NVS!\n");
        return false;
    }

    // Read battery configuration from NVS
    err = nvs_get_u8(nvs_handle, BATTERY_TYPE_KEY, &battery_type);
    if (err != ESP_OK)
    {
        printf("ERROR: Failed to read battery type from NVS!\n");
        nvs_close(nvs_handle);
        return false;
    }

    err = nvs_get_u8(nvs_handle, BATTERY_VOLTAGE_SYSTEM_KEY, &voltage_system);
    if (err != ESP_OK)
    {
        printf("ERROR: Failed to read voltage system from NVS!\n");
        nvs_close(nvs_handle);
        return false;
    }

    err = nvs_get_u16(nvs_handle, BATTERY_CAPACITY_KEY, &capacity_ah);
    if (err != ESP_OK)
    {
        printf("ERROR: Failed to read capacity from NVS!\n");
        nvs_close(nvs_handle);
        return false;
    }

    nvs_close(nvs_handle);

    // Generate profile based on NVS settings
    return battery_generate_profile((battery_type_t)battery_type,
                                    (voltage_system_t)voltage_system,
                                    capacity_ah,
                                    profile_out);
}

void battery_system_init(battery_profile_t *profile)

{ // Initialize battery management system with the provided profile
    printf("Initializing battery system with profile: %s\n", profile->name_prefix);
    battery_profile_t current_battery_profile;

    // User selects: LiFePO4, 48V system, 200Ah capacity
    battery_type_t selected_type = BATTERY_LIFEPO4;
    voltage_system_t selected_voltage = VOLTAGE_SYSTEM_12V;
    uint16_t selected_capacity = 200;

    // Save configuration to NVS
    if (battery_save_configuration(selected_type, selected_voltage, selected_capacity))
    {
        printf("Configuration saved to NVS\n");
    }

    // Generate and display the profile
    if (battery_generate_profile(selected_type, selected_voltage, selected_capacity, &current_battery_profile))
    {
        battery_print_profile(&current_battery_profile);
    }
    else
    {
        printf("Failed to generate battery profile!\n");
    }
}

void battery_profile_load_from_nvs(battery_profile_t *profile)
{
    printf("\n=== EXAMPLE 2: Load from NVS ===\n");

    battery_profile_t current_battery;

    // Load battery profile from NVS
    if (battery_load_profile(&current_battery))
    {
        printf("Battery profile loaded successfully!\n");
        battery_print_profile(&current_battery);

        // Use the profile for battery management
        printf("Using profile for battery management:\n");
        printf("Charging at %.2fA until %.2fV\n",
               current_battery.recommended_charge_current_per_100ah,
               current_battery.bulk_charge_voltage_12v);
        printf("Will cutoff discharge at %.2fV\n",
               current_battery.cutoff_voltage_12v);
    }
    else
    {
        printf("Failed to load battery profile from NVS!\n");
    }
}

// Battery type names for display
const char *battery_type_names[BATTERY_TYPE_COUNT] = {
    "Lead-Acid",   // BATTERY_LEAD_ACID = 0
    "AGM",         // BATTERY_AGM = 1
    "GEL",         // BATTERY_GEL = 2
    "LiFePO4",     // BATTERY_LIFEPO4 = 3
    "Lithium Ion", // BATTERY_LITHIUM_ION = 4
    "NiMH"         // BATTERY_NIMH = 5
};

/* Voltage system names for the settings-menu select item. voltage_system_t
 * values (12/24/48/96) are not sequential/zero-based, so a select's
 * 0..N-1 index has to be mapped through voltage_system_values[] below
 * rather than cast directly like battery_type_t is. */
#define BATTERY_VOLTAGE_SYSTEM_OPTION_COUNT 4
const char *battery_voltage_system_names[BATTERY_VOLTAGE_SYSTEM_OPTION_COUNT] = {
    "12V",
    "24V",
    "48V",
    "96V"};
static const voltage_system_t voltage_system_values[BATTERY_VOLTAGE_SYSTEM_OPTION_COUNT] = {
    VOLTAGE_SYSTEM_12V,
    VOLTAGE_SYSTEM_24V,
    VOLTAGE_SYSTEM_48V,
    VOLTAGE_SYSTEM_96V};

/* Reverse lookup: nominal_voltage -> select index, for populating the
 * select's current selection_index when entering edit mode. Falls back
 * to index 0 (12V) if the stored value doesn't match any option. */
static int voltage_system_to_index(voltage_system_t v)
{
    for (int i = 0; i < BATTERY_VOLTAGE_SYSTEM_OPTION_COUNT; i++)
    {
        if (voltage_system_values[i] == v)
            return i;
    }
    return 0;
}

// Call this to display and handle selection
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
    case BTN_ENTER:
    {
        bool saved = battery_save_configuration(
            (battery_type_t)selected, VOLTAGE_SYSTEM_48V, 200);
        if (saved == true)
        {
            snprintf(display_char, sizeof(display_char),
                     "Saved: %s", battery_type_names[selected]);
            lcd_flash_info("Battery Type    ", display_char, 1500);
        }
        else
        {
            lcd_flash_info("Save Failed!    ", "                ", 1500);
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

typedef struct
{
    uint8_t error_code;    // Raw error code (ERR_OVER_TEMP etc. cast to uint8_t)
    uint32_t timestamp_ms; // xTaskGetTickCount() * portTICK_PERIOD_MS at log time
    char description[16];  // Human-readable, NUL-terminated, matches last_error_msg width
} error_log_entry_t;

typedef enum
{
    VALUE_TYPE_VOLTAGE = 0,
    VALUE_TYPE_FREQUENCY,
    VALUE_TYPE_CURRENT,
    VALUE_TYPE_TEMPERATURE,
    VALUE_TYPE_BATTERY_VOLTAGE,
    VALUE_TYPE_MENU_SELECTION,
    VALUE_TYPE_TIMEOUT,
    VALUE_TYPE_BLUETOOTH,
    VALUE_TYPE_WIFI,
    VALUE_TYPE_AUTO_SHUTDOWN,
    VALUE_TYPE_SCROLL_ENABLE,
    VALUE_TYPE_SCROLL_SPEED,
    VALUE_TYPE_BATTERY_TYPE,
    VALUE_TYPE_BATTERY_VOLTAGE_SYSTEM,
    VALUE_TYPE_SOUND_ENABLE,
    VALUE_TYPE_COUNT
} value_edit_param_t;

system_state_t sys_state;

typedef struct
{
    uint8_t click_count;
    int64_t first_click_time;
    int64_t last_release_time;
    bool waiting_for_timeout;
    button_id_t button_id;
} click_detector_t;

static click_detector_t g_click_detectors[BUTTON_MAX_CLICK_COUNT]; // One detector per button

// adc   //added manually
static const char *TAG_ERROR = "ERROR_MONITOR";
static const char *TAG_NVS = "NVS";
static const char *TAG_SYS = "System";

// Create a struct for both ADC and ADC2
typedef struct
{
    float calibration_values[2]; // [0]=offset, [1]=gain
    bool adc2_calibrated;        // Flag to indicate if ADC2 is calibrated
    bool calibrated;
    bool calibration_mode;
} adc_calibration_t;
// Create an array to hold calibration data for both ADC1 and ADC2
static adc_calibration_t adc_calibration[ADC_CHANNEL_USE]; // Array for ADC1 and ADC2

// Factory reset confirmation menu states
typedef enum
{
    FACTORY_RESET_CONFIRM,     // Initial prompt
    FACTORY_RESET_YES_SELECTED // Yes is selected
} factory_reset_state_t;

diagnostic_data_t diag_data;

// Security Architectural data
change_pin_ctx_t change_pin_ctx;
SemaphoreHandle_t change_pin_mutex = NULL;

// Menu editing state
static menu_edit_state_t menu_edit = {
    .temp_value = 0,
    .edit_step = 0,
    .value_changed = false,
};

typedef enum
{
    MENU_EDIT_TRUE,
    MENU_EDIT_FALSE
} menu_edit_t;

// Menu item definitions
typedef struct
{
    const char *label;
    menu_state_t state;
} menu_item_t;

menu_edit_t menu_edit_status = MENU_EDIT_FALSE;

// Back button event handler
// Add this structure to track navigation history (add to your header file)
typedef struct
{
    menu_state_t state;
    int selection;
} menu_history_entry_t;

#define MAX_MENU_HISTORY 10

typedef struct
{
    menu_history_entry_t stack[MAX_MENU_HISTORY];
    int depth;
} menu_history_t;

// Global or in sys_state
static menu_history_t menu_history = {0};

// MAIN MENU (5 items)
static const menu_item_t main_menu_items[] = {
    {"Settings", MENU_SETTINGS},
    {"Monitoring", MENU_MONITORING},
    {"Diagnostic", MENU_DIAGNOSTIC},
    {"WiFi Config", MENU_WIFI_CONFIG},
    {"Factory Reset", MENU_FACTORY_RESET},
    {"Security", MENU_SECURITY}};

// SETTINGS MENU (5 items)
static const menu_item_t settings_items[] = {
    {"Voltage Thresh", MENU_SETTINGS},
    {"Current Limit", MENU_SETTINGS},
    {"Freq Range", MENU_SETTINGS},
    {"Temp Alarm", MENU_SETTINGS},
    {"Sys Timeout", MENU_SETTINGS},
    {"Auto Shutdown", MENU_SETTINGS},
    {"Scroll Enable", MENU_SETTINGS},
    {"Scroll Speed", MENU_SETTINGS},
    {"Battery Type", MENU_SETTINGS},
    {"Voltage System", MENU_SETTINGS},
    {"Sound", MENU_SETTINGS}};

// MONITORING MENU (6 items)
static const menu_item_t monitoring_items[] = {
    {"Voltage", MENU_MONITORING},
    {"Current", MENU_MONITORING},
    {"Frequency", MENU_MONITORING},
    {"Temperature", MENU_MONITORING},
    {"Power Factor", MENU_MONITORING},
    {"Efficiency", MENU_MONITORING}};

// DIAGNOSTIC MENU (6 items)
static const menu_item_t diagnostic_items[] = {
    {"System Health", MENU_DIAGNOSTIC},
    {"Error Logs", MENU_DIAGNOSTIC},
    {"Performance", MENU_DIAGNOSTIC},
    {"Device Info", MENU_DIAGNOSTIC},
    {"Uptime", MENU_DIAGNOSTIC},
    {"Memory Usage", MENU_DIAGNOSTIC}};

// WIFI CONFIG MENU (7 items)
static const menu_item_t wifi_items[] = {
    {"SSID", MENU_WIFI_CONFIG},
    {"Password", MENU_WIFI_CONFIG},
    {"IP Address", MENU_WIFI_CONFIG},
    {"Gateway", MENU_WIFI_CONFIG},
    {"DNS Server", MENU_WIFI_CONFIG},
    {"Scan Networks", MENU_WIFI_CONFIG},
    {"Connect", MENU_WIFI_CONFIG}};

static const menu_item_t factory_reset_items[] = {
    {"Reset All Data", MENU_FACTORY_RESET},
    {"Clear Settings", MENU_FACTORY_RESET},
    {"Erase Logs", MENU_FACTORY_RESET}};

static const menu_item_t security_items[] = {
    {"View PIN", MENU_SECURITY},
    {"Change PIN", MENU_SECURITY},
    {"PIN Reset", MENU_SECURITY}};

// ================== RTC STRUCT ==================
typedef struct
{
    uint32_t magic_flag;        // To validate RTC memory content
    TickType_t last_sleep_time; // Time of last sleep entry
    bool was_inverter_active;
    bool ac_was_connected;
    uint16_t last_error;
    uint32_t wake_count;
} rtc_mem_t;

// Application state structure
typedef struct
{
    button_handle_t power_button;
    button_handle_t menu_button;
    button_handle_t button_config_t;
    button_handle_t button_up;
    button_handle_t button_down;
    button_handle_t button_back;
    bool system_ready;
    uint32_t power_press_count;
    uint32_t menu_press_count;
} app_state_t;

static app_state_t g_app_state = {0};

// GLOBAL LED controller
led_pattern_t pattern =
    {
        .led = LED_STATUS,
        .type = LED_PATTERN_BLINK,
        .brightness = 100,
        .on_time_ms = 200,
        .off_time_ms = 200,
        .repeat = 5,
};

// Get current time in milliseconds
int64_t lcd_get_current_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void lcd_create_custom_char(uint8_t location, uint8_t charmap[]);

adc_cali_handle_t handle = NULL;

// =============== FUNCTION PROTOTYPES ===============

void init_hardware();
void nvs_init(bool erase_on_fail);
bool save_settings();
bool load_settings();
void lcd_show_bt_edit_screen(const char *label, const char *value);
void lcd_show_value_edit_screen(void);
void lcd_show_bt_connecting_screen(const char *device_name);
void lcd_show_factory_reset_screen(void);
void adc_task(void *arg);
static bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle);
static void adc_calibration_deinit(adc_cali_handle_t handle);
void power_task(void *arg);
void error_handler();
void log_all_error_flags(uint32_t flags);
void toggle_display();

// Function prototypes
void inverter_power_on(void);
void shutdown_inverter(void);
static void post_inverter_power_event(bool powered_on);
static void post_inverter_fault_event(void);
static void post_inverter_success_event(void);
static void post_button_click_event(void);
void enter_diagnostic_mode(void);
void exit_diagnostic_mode(void);
void lcd_draw_diagnostics_screen(uint8_t index);
void perform_factory_reset(void);
static void show_menu_screen(menu_state_t menu_st, int selection);
void menu_navigate_up(void);
void menu_navigate_down(void);
void menu_go_back(void);
bool check_safety_conditions(void);
void handle_menu_timeout(void);
void enter_submenu(menu_state_t submenu);
static void clear_menu_history(void);
static bool pop_menu_history(menu_state_t *state, int *selection);
void enter_detail_view(menu_state_t parent_menu, int parent_selection);
void exit_detail_view(void);
static void push_menu_history(menu_state_t state, uint8_t selection);

// LCD display functions
void lcd_update_value_edit_screen(void);
void update_lcd_activity_state(void);

void process_battery_voltage(void);

bool inverter_set_output_voltage(float voltage_setpoint);
bool inverter_set_output_frequency(float frequency_setpoint);
bool inverter_set_current_limit(float current_limit);
bool thermal_protection_set_limit(float temperature_limit_celsius);
bool battery_monitor_set_cutoff(float cutoff_voltage);
void set_system_timeout(uint32_t timeout_ms);
void inverter_emergency_shutdown(void);
static void apply_voltage_threshold(float v);
static void apply_frequency(float v);
static void apply_current_limit(float v);
static void apply_temperature_limit(float v);
static void apply_battery_cutoff(float v);
static void apply_wifi(float v);
static void apply_bluetooth(float v);
static void apply_auto_shutdown(float v);
static void apply_scroll_enable(float v);
static void apply_scroll_speed(float v);
static void apply_system_timeout(float v);
static void apply_battery_type(float v);
static void apply_battery_voltage_system(float v);
static void apply_sound_enable(float v);
static void sync_battery_protection_thresholds(void);

// Value adjustment functions
void increase_value(bool fast_mode, bool precision_mode);
void decrease_value(bool fast_mode, bool precision_mode);
static void get_accel_step(int64_t hold_duration_ms, bool *fast, bool *big);
void enter_value_edit_mode(value_edit_context_t *value_type);
void exit_value_edit_mode(bool save_changes);
void apply_value_change(void);
void reset_value_to_backup(void);
float *get_current_value_pointer(void);
value_edit_context_t *get_current_value_config(void);
float calculate_increment(bool fast_mode, bool precision_mode);
bool validate_value_range(float new_value);
void handle_value_confirmation(void);
void update_system_parameter(value_edit_context_t *config, float new_value);
static const menu_item_t *get_menu_items(menu_state_t state, int *item_count);
void edit_voltage_threshold(void);
void edit_current_limit(void);
void edit_frequency_range(void);
void edit_temperature_alarm(void);
void edit_system_timeout(void);
void edit_auto_shutdown(void);
void edit_scroll_enable(void);
void edit_scroll_speed(void);
void edit_battery_type(void);
void edit_battery_voltage_system(void);
void edit_sound_enable(void);
void security_pin(void);

// Submenu functions
void lcd_show_monitoring_detail(const char *label, float value, const char *unit);
void lcd_draw_diagnostics_screen(uint8_t index);

static void wifi_init_sta(void);
void start_wifi_scan(void);
void lcd_show_wifi_scan_screen(void);
void start_wifi_connection(void);
void stop_wifi_connection(void);
void clear_settings(void);
void reload_default_settings(void);
// Function to save the frequency setting to NVS
void save_frequency_to_nvs(int frequency);
esp_err_t get_setting_value(const char *key, int32_t default_val, int32_t *out_value);
esp_err_t set_setting_value(const char *key, int32_t value);
esp_err_t set_i32_safe(nvs_handle_t nvs, const char *key, void *value);
esp_err_t set_u8_safe(nvs_handle_t nvs, const char *key, void *value);
static bool get_u8_safe(nvs_handle_t handle, const char *key, uint8_t *out);
static bool get_i32_safe(nvs_handle_t handle, const char *key, int32_t *out);
static bool validate_and_clamp_settings(void);

void menu_exit();
menu_state_t display_menu_state();
void adjust_factory_reset(button_event_info_t *btn);
void init_menu_system();
void restore_from_deep_sleep();
void enter_deep_sleep(uint32_t sleep_seconds);
void init_deep_sleep(uint64_t wakeup_pin_mask, int wakeup_time_sec);
void save_calibration();
void load_calibration();
void check_protections();
void update_led_status();
void perform_factory_reset();
void show_system_info();
bool system_is_inactive();
void update_activity();
void display_timeout_task(void *arg);
void lcd_power_init();
void LCD_power(bool enable);
void lcd_set_brightness(uint8_t brightness);
void init_system_state();
void handle_critical_error(); // centralized error handling
bool battery_save_configuration(battery_type_t type, voltage_system_t voltage_sys, uint16_t capacity_ah);
void battery_print_profile(const battery_profile_t *profile);
bool battery_load_profile(battery_profile_t *profile);
void log_error_to_nvs(uint8_t error_code);
void perform_system_restart(bool factory_reset);
// deep sleep function
void enable_brownout();
void disable_brownout();
// battery functions
void show_battery_voltage();
void show_temperature();
void display_battery_settings();
// watch dog registration for various tasks
void register_task_to_wdt(TaskHandle_t task);

// adc sampling
float filter_update(MovingAverageFilter *f, float new_value);
void filter_init(MovingAverageFilter *f);
// Calibration menu
void adjust_calibration_setting(button_event_info_t btn);
void log_error_state();
static void cleanup_button_system(void);

RTC_DATA_ATTR rtc_mem_t rtc_mem; // Must be placed in RTC fast memory
static const char *APP_TAG = "BUTTON_APP";
extern void update_activity(); // Activity timestamp updater

static value_edit_context_t value_edit[] = {
    [VALUE_TYPE_VOLTAGE] = {
        .edit_type = VALUE_EDIT_NUMERIC,
        .min_value = 100.0f,
        .max_value = 240.000f,
        .temp_value = 220.0f,
        .increment_small = 1.0f,
        .increment_large = 5.0f,
        .increment_precision = 0.1f,
        .decimal_places = 1,
        .unit = "V",
        .label = "Voltage Threshold",
        .is_critical = true,
        .live_update = true,
        .apply = apply_voltage_threshold,
        .step_size = 1.0f,
        .current_value = 220.0f},

    [VALUE_TYPE_FREQUENCY] = {
        .edit_type = VALUE_EDIT_NUMERIC,
        .min_value = 45.0f,
        .max_value = 65.0f,
        .increment_small = 0.1f,
        .increment_large = 1.0f,
        .increment_precision = 0.01f,
        .step_size = 0.1f,
        .decimal_places = 2,
        .unit = "Hz",
        .label = "Frequency",
        .is_critical = true,
        .live_update = true,
        .current_value = 50.0f,
        .apply = apply_frequency,
    },

    [VALUE_TYPE_CURRENT] = {
        .edit_type = VALUE_EDIT_NUMERIC,
        .min_value = 1.0f,
        .max_value = 50.0f,
        .increment_small = 0.5f,
        .increment_large = 2.0f,
        .increment_precision = 0.1f,
        .step_size = 0.1f,
        .decimal_places = 1,
        .unit = "A",
        .label = "Current Limit",
        .is_critical = true,
        .live_update = true,
        .current_value = 25.0f,
        .apply = apply_current_limit,
    },

    [VALUE_TYPE_TEMPERATURE] = {
        .edit_type = VALUE_EDIT_NUMERIC,
        .min_value = 40.0f,
        .max_value = 85.0f,
        .increment_small = 1.0f,
        .increment_large = 5.0f,
        .increment_precision = 0.5f,
        .step_size = 0.5f,
        .decimal_places = 1,
        .unit = "°C",
        .label = "Temperature Limit",
        .is_critical = true,
        .live_update = true,
        .current_value = 60.0f,
        .apply = apply_temperature_limit,
    },

    [VALUE_TYPE_BATTERY_VOLTAGE] = {
        .edit_type = VALUE_EDIT_NUMERIC,
        .min_value = 10.0f,
        .max_value = 15.0f,
        .increment_small = 0.1f,
        .increment_large = 5.0f,
        .increment_precision = 0.5f,
        .step_size = 0.5f,
        .decimal_places = 2,
        .unit = "V",
        .label = "Battery Cutoff",
        .is_critical = true,
        .live_update = true,
        .current_value = 12.0f,
        .apply = apply_battery_cutoff,
    },

    [VALUE_TYPE_MENU_SELECTION] = {
        .edit_type = VALUE_EDIT_SELECT,
        .min_value = 0,
        .max_value = 0,
        .temp_value = 0,
        .unit = "",
        .label = "Menu Select",
        .is_critical = false,
        .live_update = false,
        .apply = NULL,
    },

    [VALUE_TYPE_BLUETOOTH] = {
        .edit_type = VALUE_EDIT_BOOL,
        .bool_value = false,
        .unit = "",
        .label = "Bluetooth",
        .is_critical = false,
        .live_update = true,
        .apply = apply_bluetooth,
    },

    [VALUE_TYPE_WIFI] = {
        .edit_type = VALUE_EDIT_BOOL,
        .selection_index = 0,
        .bool_value = false,
        .unit = "",
        .label = "WiFi",
        .is_critical = false,
        .live_update = true,
        .apply = apply_wifi,
    },
    [VALUE_TYPE_TIMEOUT] = {
        .edit_type = VALUE_EDIT_NUMERIC,
        .min_value = 5000.0f,
        .max_value = 300000.0f,
        .increment_small = 1000.0f,
        .increment_large = 30000.0f,
        .increment_precision = 100.0f,
        .decimal_places = 0,
        .unit = "ms",
        .label = "System Timeout",
        .is_critical = false,
        .live_update = true,
        .step_size = 1000.0f,
        .current_value = 300000.0f,
        .apply = apply_system_timeout,
    },

    [VALUE_TYPE_AUTO_SHUTDOWN] = {
        .edit_type = VALUE_EDIT_BOOL,
        .current_value = 0.0f,
        .unit = "",
        .label = "Auto Shutdown",
        .is_critical = false,
        .live_update = true,
        .apply = apply_auto_shutdown,
    },

    [VALUE_TYPE_SCROLL_ENABLE] = {
        .edit_type = VALUE_EDIT_BOOL,
        .current_value = 0.0f,
        .unit = "",
        .label = "Scroll Enable",
        .is_critical = false,
        .live_update = true,
        .apply = apply_scroll_enable,
    },

    [VALUE_TYPE_SCROLL_SPEED] = {
        .edit_type = VALUE_EDIT_NUMERIC,
        .min_value = 1.0f,
        .max_value = 10.0f,
        .increment_small = 1.0f,
        .increment_large = 2.0f,
        .increment_precision = 1.0f,
        .decimal_places = 0,
        .unit = "",
        .label = "Scroll Speed",
        .is_critical = false,
        .live_update = true,
        .step_size = 1.0f,
        .current_value = DEFAULT_SCROLL_SPEED,
        .apply = apply_scroll_speed,
    },

    [VALUE_TYPE_BATTERY_TYPE] = {
        .edit_type = VALUE_EDIT_SELECT,
        .selection_index = 0,
        .max_selection = BATTERY_TYPE_COUNT,
        .options = battery_type_names,
        .unit = "",
        .label = "Battery Type",
        .is_critical = true, /* changes charge/cutoff voltages — confirm before applying */
        .live_update = false,
        .apply = apply_battery_type,
    },

    [VALUE_TYPE_BATTERY_VOLTAGE_SYSTEM] = {
        .edit_type = VALUE_EDIT_SELECT,
        .selection_index = 0,
        .max_selection = BATTERY_VOLTAGE_SYSTEM_OPTION_COUNT,
        .options = battery_voltage_system_names,
        .unit = "",
        .label = "Voltage System",
        .is_critical = true, /* rescales every voltage/current field — confirm before applying */
        .live_update = false,
        .apply = apply_battery_voltage_system,
    },

    [VALUE_TYPE_SOUND_ENABLE] = {
        .edit_type = VALUE_EDIT_BOOL,
        .current_value = 1.0f,
        .unit = "",
        .label = "Sound",
        .is_critical = false,
        .live_update = true,
        .apply = apply_sound_enable,
    },
};

// =============== HARDWARE INITIALIZATION ===============
void init_hardware(void)
{
#if CONFIG_USE_LCD
    lcd_init(LCD_ADDR, SDA_PIN, SCL_PIN);
#endif

    // ==========================================================
    // Configure Standard Output GPIOs (Non-PWM)
    // ==========================================================
    static const gpio_num_t output_pins[] =
        {
            GPIO_POWER_RELAY,
            GPIO_FAN_TEST,
        };

    gpio_config_t gpio_conf =
        {
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };

    for (size_t i = 0; i < sizeof(output_pins) / sizeof(output_pins[0]); i++)
    {
        gpio_conf.pin_bit_mask = (1ULL << output_pins[i]);
        ESP_ERROR_CHECK(gpio_config(&gpio_conf));
        gpio_set_level(output_pins[i], 0);
    }

    // ==========================================================
    // Initialize LED Driver
    // ==========================================================
#if CONFIG_USE_LED_PWM
    led_init();
#endif

    // ==========================================================
    // Initialize Buzzer Driver
    // ==========================================================
    buzzer_init();

    // ==========================================================
    // Initialize System State
    // ==========================================================
    sys_state.flags.last_user_activity = xTaskGetTickCount();
    sys_state.flags.last_power_event = xTaskGetTickCount();
    sys_state.display.display_on = true;

#if CONFIG_USE_DEEP_SLEEP
    // ==========================================================
    // Configure Deep Sleep Wakeup Sources
    // ==========================================================
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_14, 1);
    esp_sleep_enable_timer_wakeup(3600ULL * 1000000ULL);
#endif

#if CONFIG_USE_DISPLAY_TIMEOUT_TASK
    // ==========================================================
    // Create Display Timeout Task
    // ==========================================================
    xTaskCreate(display_timeout_task, "Display Timeout", 2048, NULL, 5, NULL);
#endif
}

#define NVS_FLOAT_SCALE 100.0f

typedef struct
{
    const char *key;
    void *field;
    size_t size;
    float default_val;
    bool is_scaled_float;
} nvs_setting_t;

static nvs_setting_t g_settings[] = {
    {"bat_volt_system", &sys_state.inverter.battery_voltage_system, sizeof(uint8_t), 0, false},
    {"inverter_active", &sys_state.inverter.inverter_active, sizeof(uint8_t), 0, false},
    {"bat_type", &sys_state.battery_profile.profile_id, sizeof(uint8_t), BATTERY_AGM, false},
    {"bat_cap_ah", &sys_state.battery_profile.capacity_ah, sizeof(int32_t), 0, true},
    {"bat_charge_cur", &sys_state.battery_profile.max_charge_current_per_100ah, sizeof(int32_t), 0, true},
    {"bat_disc_cur", &sys_state.battery_profile.max_discharge_current_per_100ah, sizeof(int32_t), 0, true},
    {"bat_full_volt", &sys_state.battery_profile.high_battery_voltage_12v, sizeof(int32_t), 0, true},
    {"bat_cutoff_volt", &sys_state.battery_profile.cutoff_voltage_12v, sizeof(int32_t), 10.5f, true},
    {"bat_rech_volt", &sys_state.battery_profile.recharge_voltage_12v, sizeof(int32_t), 14.8f, true},
    {"brightness", &sys_state.display.brightness, sizeof(int32_t), 100, false},
    {"backlight_time", &sys_state.display.backlight_timeout, sizeof(int32_t), 30, false},
    {"auto_shutdown", &sys_state.display.auto_shutdown_enabled, sizeof(uint8_t), 0, false},
    {"scroll_en", &sys_state.display.scroll_enabled, sizeof(uint8_t), 0, false},
    {"sound_en", &sys_state.sound_enabled, sizeof(uint8_t), 1, false},
    {"scroll_spd", &sys_state.display.scroll_speed, sizeof(uint8_t), DEFAULT_SCROLL_SPEED, false},
    {"out_volt", &sys_state.inverter.output_voltage, sizeof(int32_t), 220.0f, true},
    {"out_freq", &sys_state.inverter.output_frequency, sizeof(int32_t), 50.0f, true},
    {"volt_threshold", &sys_state.settings.voltage_threshold, sizeof(int32_t), 12.5f, true},
    {"current_limit", &sys_state.settings.current_limit, sizeof(int32_t), 50.0f, true},
    {"temp_alarm", &sys_state.settings.temperature_alarm, sizeof(int32_t), 70.0f, true},
    {"frequency_range", &sys_state.settings.frequency_range, sizeof(int32_t), 50, false},
    {"system_timeout", &sys_state.settings.system_timeout, sizeof(int32_t), 300, false},
    {"security_en", &sys_state.security.enabled, sizeof(uint8_t), false, false},
};

#define NVS_SETTINGS_COUNT (sizeof(g_settings) / sizeof(g_settings[0]))

esp_err_t nvs_save_all(nvs_handle_t handle)
{
    esp_err_t err = ESP_OK;
    esp_err_t first_err = ESP_OK;
    const char *NVS_SAVING_TAG = "NVS_LOAD";

    for (size_t i = 0; i < NVS_SETTINGS_COUNT; i++)
    {
        nvs_setting_t *s = &g_settings[i];

        if (s->is_scaled_float)
        {
            float *fval = (float *)s->field;
            int32_t scaled = (int32_t)((*fval) * NVS_FLOAT_SCALE);
            err = set_i32_safe(handle, s->key, &scaled);
        }
        else if (s->size == sizeof(uint8_t))
        {
            err = set_u8_safe(handle, s->key, s->field);
        }
        else
        {
            err = set_i32_safe(handle, s->key, s->field);
        }

        if (err != ESP_OK)
        {
            ESP_LOGW(NVS_SAVING_TAG, "Failed to save '%s': %s", s->key, esp_err_to_name(err));
            if (first_err == ESP_OK)
            {
                first_err = err;
            }
        }
    }

    return first_err;
}

esp_err_t nvs_load_all(nvs_handle_t handle)
{
    for (size_t i = 0; i < NVS_SETTINGS_COUNT; i++)
    {
        nvs_setting_t *s = &g_settings[i];

        if (s->is_scaled_float)
        {
            int32_t scaled = (int32_t)(s->default_val * NVS_FLOAT_SCALE);
            bool ok = get_i32_safe(handle, s->key, &scaled);
            if (!ok)
            {
                ESP_LOGW(NVS_LOAD_TAG, "'%s' not found, using default %.2f", s->key, s->default_val);
            }
            *(float *)s->field = (float)scaled / NVS_FLOAT_SCALE;
        }
        else if (s->size == sizeof(uint8_t))
        {
            uint8_t val = (uint8_t)s->default_val;
            bool ok = get_u8_safe(handle, s->key, &val);
            if (!ok)
            {
                ESP_LOGW(NVS_LOAD_TAG, "'%s' not found, using default %u", s->key, val);
            }
            *(uint8_t *)s->field = val;
        }
        else
        {
            int32_t val = (int32_t)s->default_val;
            bool ok = get_i32_safe(handle, s->key, &val);
            if (!ok)
            {
                ESP_LOGW(NVS_LOAD_TAG, "'%s' not found, using default %ld", s->key, (long)val);
            }
            *(int32_t *)s->field = val;
        }
    }

    return ESP_OK;
}

#define DEFAULT_SETTINGS_COUNT (sizeof(g_settings) / sizeof(g_settings[0]))

esp_err_t get_setting_value(const char *key, int32_t default_val, int32_t *out_value)
{
    nvs_handle_t nvs;
    esp_err_t err;

    if (key == NULL || out_value == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(NVS_NS_SYSTEM, NVS_READONLY, &nvs);
    if (err != ESP_OK)
    {
        *out_value = default_val;
        return err;
    }

    err = nvs_get_i32(nvs, key, out_value);
    nvs_close(nvs);

    if (err != ESP_OK)
    {
        ESP_LOGW("NVS_SETTING", "Key '%s' not found or read failed (%s), using default %ld", key, esp_err_to_name(err), (long)default_val);
        *out_value = default_val;
    }

    return err;
}

esp_err_t set_setting_value(const char *key, int32_t value)
{
    nvs_handle_t nvs;
    esp_err_t err;

    if (key == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
    {
        ESP_LOGE("NVS_SETTING", "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_i32(nvs, key, value);
    if (err != ESP_OK)
    {
        ESP_LOGE("NVS_SETTING", "Failed to set '%s': %s", key, esp_err_to_name(err));
        nvs_close(nvs);
        return err;
    }

    err = nvs_commit(nvs);
    if (err != ESP_OK)
    {
        ESP_LOGE("NVS_SETTING", "Failed to commit '%s': %s", key, esp_err_to_name(err));
    }

    nvs_close(nvs);
    return err;
}

void nvs_init(bool erase_on_fail)
{
    const char *TAG = "NVS";
    esp_err_t err;

    // Check if already initialized
    if (nvs_initialized)
    {
        ESP_LOGD(TAG, "NVS already initialized");
        return;
    }

    ESP_LOGI(TAG, "Initializing NVS...");

    // Try to initialize NVS
    err = nvs_flash_init();

    if (err == ESP_OK)
    {
        // Success on first try
        nvs_initialized = true;
        ESP_LOGI(TAG, "NVS initialized successfully");
        return;
    }

    // Handle errors that require erasing NVS
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        if (erase_on_fail)
        {
            ESP_LOGW(TAG, "NVS partition corrupted or version mismatch, erasing...");

            // Erase NVS partition
            err = nvs_flash_erase();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to erase NVS: %s (0x%x)",
                         esp_err_to_name(err), err);
                return;
            }

            // Try to initialize again after erase
            err = nvs_flash_init();
            if (err == ESP_OK)
            {
                nvs_initialized = true;
                ESP_LOGI(TAG, "NVS initialized successfully after erase");
                return;
            }
            else
            {
                ESP_LOGE(TAG, "Failed to initialize NVS after erase: %s (0x%x)",
                         esp_err_to_name(err), err);
                return;
            }
        }
        else
        {
            ESP_LOGE(TAG, "NVS needs erasing but erase_on_fail is false");
            ESP_LOGE(TAG, "Error: %s (0x%x)", esp_err_to_name(err), err);
            return;
        }
    }

    // Other errors
    ESP_LOGE(TAG, "Failed to initialize NVS: %s (0x%x)",
             esp_err_to_name(err), err);
}

/**
 * @brief Check if NVS is initialized
 */
bool nvs_is_initialized(void)
{
    nvs_handle_t test_handle;
    esp_err_t err = nvs_open("test", NVS_READONLY, &test_handle);

    if (err == ESP_OK)
    {
        nvs_close(test_handle);
        return true;
    }

    return false;
}

/**
 * @brief Get NVS statistics
 */
void nvs_print_stats(void)
{
    const char *TAG = "NVS_STAT";
    nvs_stats_t nvs_stats;
    esp_err_t err = nvs_get_stats(NULL, &nvs_stats);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "╔════════════════════════════════════╗");
        ESP_LOGI(TAG, "║      NVS Statistics                ║");
        ESP_LOGI(TAG, "╠════════════════════════════════════╣");
        ESP_LOGI(TAG, "║ Used entries:   %5d              ║", nvs_stats.used_entries);
        ESP_LOGI(TAG, "║ Free entries:   %5d              ║", nvs_stats.free_entries);
        ESP_LOGI(TAG, "║ Total entries:  %5d              ║", nvs_stats.total_entries);
        ESP_LOGI(TAG, "║ Namespace count: %4d              ║", nvs_stats.namespace_count);
        ESP_LOGI(TAG, "╚════════════════════════════════════╝");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to get NVS stats: %s", esp_err_to_name(err));
    }
}

/**
 * @brief Factory reset - erase all NVS data
 */
esp_err_t nvs_factory_reset(void)
{
    const char *TAG = "NVS_RESET";
    ESP_LOGW(TAG, "Performing NVS factory reset...");

    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Reinitialize after erase
    err = nvs_flash_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to reinitialize NVS after erase: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "NVS factory reset completed successfully");
    return ESP_OK;
}

/**
 * @brief Erase a specific namespace
 */
esp_err_t nvs_erase_namespace(const char *namespace_name)
{
    const char *TAG = "NVS_ERASE";
    if (namespace_name == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(namespace_name, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open namespace '%s': %s",
                 namespace_name, esp_err_to_name(err));
        return err;
    }

    err = nvs_erase_all(handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to erase namespace '%s': %s",
                 namespace_name, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Namespace '%s' erased successfully", namespace_name);
    }

    return err;
}

void save_calibration()
{
    const char *NVS_CALIBRATION = "calibration"; // NVS namespace for calibration data
    // Save ADC calibration values to NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE("NVS", "Failed to open NVS for calibration save: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(handle, "adc_cal", adc_calibration, sizeof(adc_calibration));
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
        if (err == ESP_OK)
        {
            ESP_LOGI(NVS_CALIBRATION, "Calibration data saved successfully.");
        }
        else
        {
            ESP_LOGE(NVS_CALIBRATION, "Failed to commit calibration data: %s", esp_err_to_name(err));
        }
    }
    else
    {
        ESP_LOGE(NVS_CALIBRATION, "Failed to set calibration data: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
}

void load_calibration()
{
    const char *TAG_ADC_CALIB = "ADC_CALIBRATION";
    // Load ADC calibration values from NVS
    ESP_LOGI(TAG_ADC_CALIB, "Loading ADC calibration values...");
    // Load calibration values from NVS
    // If not found, use default values
    nvs_handle_t handle;
    // Create required size variable to hold the size of the calibration data
    float calibration_values[ADC2_CHANNEL_COUNTER][2]; // [0]=offset, [1]=gain
    size_t required_size = sizeof(calibration_values);
    // Initialize calibration values to defaults
    for (int i = 0; i < ADC_CHANNEL_USE; i++)
    {
        adc_calibration[i].calibration_values[0] = 0.0f; // Offset
        adc_calibration[i].calibration_values[1] = 1.0f; // Gain
        adc_calibration[i].calibrated = false;           // Mark as not calibrated
        adc_calibration[i].calibration_mode = false;     // Not in calibration mode
        // Initialize calibration values for each channel
    }
    esp_err_t err = nvs_open(NVS_NS_SYSTEM, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGI(TAG_ADC_CALIB, "No calibration data found, using defaults");
        for (int i = 0; i < ADC_CHANNEL_USE; i++)
        {
            adc_calibration[i].calibration_values[0] = 0.0f; // Offset
            adc_calibration[i].calibration_values[1] = 1.0f; // Gain
            adc_calibration[i].calibrated = false;           // Mark as not calibrated
            adc_calibration[i].calibration_mode = false;     // Not in calibration mode
        }
        return;
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG_NVS, "Failed to open NVS for reading calibration: %s", esp_err_to_name(err));
    }
    else
    {
        err = nvs_get_blob(handle, "adc_cal", calibration_values, &required_size);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG_ADC_CALIB, "No calibration data found, using defaults");
            for (int i = 0; i < ADC_CHANNEL_USE; i++)
            {
                adc_calibration[i].calibration_values[0] = 0.0f; // Offset
                adc_calibration[i].calibration_values[1] = 1.0f; // Gain
                adc_calibration[i].calibrated = false;           // Mark as not calibrated
                adc_calibration[i].calibration_mode = false;     // Not in calibration mode
            }
        }
        else
        {
            ESP_LOGI(TAG_ADC_CALIB, "Calibration data loaded successfully");
        }

        nvs_close(handle);
    }

    save_settings(); // Save the settings, possibly updated calibration
}

// load all the nvs settings

esp_err_t set_i32_safe(nvs_handle_t nvs, const char *key, void *value)
{
    return nvs_set_i32(nvs, key, *(int32_t *)value);
}

esp_err_t set_u8_safe(nvs_handle_t nvs, const char *key, void *value)
{
    return nvs_set_u8(nvs, key, *(uint8_t *)value);
}

static bool get_u8_safe(nvs_handle_t handle, const char *key, uint8_t *out)
{
    return nvs_get_u8(handle, key, out) == ESP_OK;
}

static bool get_i32_safe(nvs_handle_t handle, const char *key, int32_t *out)
{
    return nvs_get_i32(handle, key, out) == ESP_OK;
}

bool load_settings()
{
    nvs_handle_t nvs;
    esp_err_t err;
    bool load_error = false;
    const char *NVS_LOADING_TAG = "NVS_LOAD";

    err = nvs_open(NVS_NS_SYSTEM, NVS_READONLY, &nvs);
    if (err != ESP_OK)
    {
        ESP_LOGE(NVS_LOADING_TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }

    nvs_load_all(nvs);

    /* Load battery profile (type and voltage) */
    if (!battery_load_profile(&sys_state.battery_profile))
    {
        ESP_LOGW("BAT_PROFILE", "Failed to load battery profile, using defaults");
        load_error = true;
    }
    sync_battery_protection_thresholds();

    /* Cross-field / range validation — catches corrupted or
     * inconsistent values that a plain nvs_get_* success wouldn't. */
    if (validate_and_clamp_settings())
    {
        ESP_LOGW(NVS_LOADING_TAG, "One or more loaded settings were out of range and were corrected");
        load_error = true; /* forces save_settings() below to persist the fix */
    }

    if (load_error)
    {
        ESP_LOGW(NVS_LOADING_TAG, "Settings loaded with one or more defaults/corrections");
        save_settings();
        return false;
    }

    ESP_LOGI(NVS_LOADING_TAG, "Settings loaded successfully");
    return true;
}

bool save_settings()
{
    esp_err_t err;
    if (!nvs_initialized)
    {
        nvs_init(true);
    }
    nvs_handle_t nvs;
    const char *NVS_SAVE_TAG = "NVS_SAVE";

    err = nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
    {
        ESP_LOGE(NVS_SAVE_TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(NVS_SAVE_TAG, "Saving settings to NVS...");
    err = nvs_save_all(nvs);
    if (err != ESP_OK)
    {
        ESP_LOGE(NVS_SAVE_TAG, "One or more settings failed to save: %s", esp_err_to_name(err));
    }

    battery_save_configuration(sys_state.battery_profile.profile_id, sys_state.battery_profile.nominal_voltage, sys_state.battery_profile.capacity_ah);

    err = nvs_commit(nvs);
    if (err != ESP_OK)
    {
        ESP_LOGE(NVS_SAVE_TAG, "Failed to commit settings: %s", esp_err_to_name(err));
    }
    nvs_close(nvs);
    ESP_LOGI(NVS_SAVE_TAG, "Settings saved successfully");
    return true;
}

const char *get_error_string(uint32_t flags)
{
    // Check critical errors FIRST (highest priority)
    if (flags & ERR_SHORT_CIRCUIT)
        return "Short Circuit   ";
    if (flags & ERR_SYSTEM_FAILURE)
        return "System Failure  ";
    if (flags & ERR_OVER_TEMP)
        return "Overtemperature ";
    if (flags & ERR_UNDER_VOLTAGE)
        return "Under Voltage   ";
    if (flags & ERR_OVER_VOLTAGE)
        return "Over Voltage    ";
    if (flags & ERR_OVERLOAD)
        return "Overload        ";
    if (flags & ERR_LOW_BAT)
        return "Low Battery     ";
    if (flags & ERR_HIGH_BAT)
        return "High Battery    ";
    if (flags & ERR_FAN_FAIL)
        return "Fan Failure     ";
    if (flags & ERR_AC_FAULT)
        return "AC Fault        ";
    if (flags & ERR_INVERTER_VOLTAGE)
        return "Inverter Fault  ";
    if (flags & ERR_BATTERY_VOLTAGE)
        return "Battery Fault   ";
    if (flags & ERR_OVER_UNDER_VOLTAGE)
        return "Voltage Fault   ";
    if (flags & ERR_EEPROM)
        return "EEPROM Error    ";

    return "Unknown Error   ";
}

bool detect_critical_error()
{
    const TickType_t ERROR_WINDOW = pdMS_TO_TICKS(10000); // 10 seconds
    const int ERROR_THRESHOLD = 5;
    const UBaseType_t STACK_THRESHOLD = 128;

    static int consecutive_errors = 0;
    static TickType_t first_error_tick = 0;
    static uint32_t last_error_code = 0;

#define REPORT_ERROR(tag, code)                                 \
    do                                                          \
    {                                                           \
        const char *err_str = get_error_string(code);           \
        log_error_to_nvs(code);                                 \
        ESP_LOGE(tag, "[%s] Critical error occurred", err_str); \
        lcd_show_fault(err_str, "System Halted   ");            \
        return true;                                            \
    } while (0)

    uint32_t current_error_code = ERR_NONE;

    // 1. Scan critical hardware error flags
    uint32_t flags = sys_state.error.error_flags;

    if (flags & ERR_OVER_TEMP)
        current_error_code = ERR_OVER_TEMP;
    else if (flags & ERR_SHORT_CIRCUIT)
        current_error_code = ERR_SHORT_CIRCUIT;
    else if (flags & ERR_FAN_FAIL)
        current_error_code = ERR_FAN_FAIL;
    else if (flags & ERR_SYSTEM_FAILURE)
        current_error_code = ERR_SYSTEM_FAILURE;
    else if (flags & ERR_UNDER_VOLTAGE)
        current_error_code = ERR_UNDER_VOLTAGE;
    else if (flags & ERR_OVER_VOLTAGE)
        current_error_code = ERR_OVER_VOLTAGE;
    else if (flags & ERR_INVERTER_VOLTAGE)
        current_error_code = ERR_INVERTER_VOLTAGE;
    else if (flags & ERR_AC_FAULT)
        current_error_code = ERR_AC_FAULT;

    // 2. ADC sanity check (optional)
    /*
    if (sys_state.inverter.battery_voltage < 0.5f) {
        current_error_code = ERR_BATTERY_VOLTAGE;
    }
    */

    // 3. Watchdog
    if (esp_task_wdt_status(NULL) == ESP_ERR_TIMEOUT)
        current_error_code = ERR_SYSTEM_FAILURE;

    // 4. Stack overflow
    if (uxTaskGetStackHighWaterMark(NULL) < STACK_THRESHOLD)
        current_error_code = ERR_SYSTEM_FAILURE;

    // 5. No error
    if (current_error_code == ERR_NONE)
    {
        consecutive_errors = 0;
        last_error_code = ERR_NONE;
        first_error_tick = 0;
        return false;
    }

    // 6. Consecutive error logic
    TickType_t now = xTaskGetTickCount();

    if (current_error_code == last_error_code)
    {
        if (first_error_tick == 0 || (now - first_error_tick < ERROR_WINDOW))
        {
            consecutive_errors++;
        }
        else
        {
            consecutive_errors = 1;
            first_error_tick = now;
        }
    }
    else
    {
        consecutive_errors = 1;
        first_error_tick = now;
        last_error_code = current_error_code;
    }

    // 7. Trigger report
    if (consecutive_errors >= ERROR_THRESHOLD)
    {
        REPORT_ERROR(TAG_ERROR, current_error_code);
    }

    return false;
}

// ADC channel ID enumeration for application logic
typedef enum
{
    CHANNEL_ID_LOW_BATTERY = 0,
    CHANNEL_ID_OVER_UNDER_VOLTAGE,
    CHANNEL_ID_BATTERY_VOLTAGE,
    CHANNEL_ID_FAN,
    CHANNEL_ID_INVERTER_OUTPUT_VOLTAGE,
    CHANNEL_ID_MAX
} adc_channel_id_t;

// Runtime ADC channel state (tracks calibration per channel)
typedef struct
{
    adc_cali_handle_t cali_handle;
    bool is_calibrated;
} adc_channel_state_t;

// ADC channel configuration structure
typedef struct
{
    adc_channel_t channel;       // ESP-IDF ADC channel (ADC_CHANNEL_0, ADC_CHANNEL_1, etc.)
    adc_channel_id_t channel_id; // Application-specific channel identifier
    float *target_value;
    float threshold_low;
    float threshold_high;
    bool has_high_threshold;
    uint32_t error_flag;
    const char *name;
    float voltage_divider_ratio; // Ratio = (R1 + R2) / R2, where R1 is top resistor, R2 is bottom
} adc_channel_config_t;

// ADC reading context
typedef struct
{
    adc_oneshot_unit_handle_t handle;
    adc_channel_state_t *channel_states; // Array of channel states (one per config entry)
    int channel_count;
} adc_context_t;

// Forward declarations
static bool initialize_adc_unit(adc_oneshot_unit_handle_t *handle, adc_unit_t unit_id);
static bool configure_adc_channels(adc_oneshot_unit_handle_t handle,
                                   const adc_channel_config_t *configs,
                                   adc_channel_state_t *states,
                                   int channel_count,
                                   adc_unit_t unit_id);
static esp_err_t read_adc_with_multisampling(adc_oneshot_unit_handle_t handle,
                                             adc_channel_t channel,
                                             adc_cali_handle_t cali_handle,
                                             bool is_calibrated,
                                             float *out_voltage,
                                             uint8_t samples);
static void process_adc_reading(const adc_channel_config_t *config,
                                const adc_channel_state_t *state,
                                adc_oneshot_unit_handle_t handle);

static void cleanup_adc_resources(adc_oneshot_unit_handle_t handle,
                                  adc_channel_state_t *states,
                                  int channel_count);

// ADC channel configurations

/**
 * @brief Set error flag
 */
void set_error_flag(uint32_t flag)
{
    sys_state.error.error_flags |= flag;
}

/**
 * @brief Clear error flag
 */
void clear_error_flag(uint32_t flag)
{
    sys_state.error.error_flags &= ~flag;
}

static const adc_channel_config_t adc_configs[] = {
    {.channel = ADC_LOW_BATTERY, // Replace with actual ADC channel for low battery
     .channel_id = CHANNEL_ID_LOW_BATTERY,
     .target_value = &sys_state.inverter.low_bat_egs002_signal,
     .threshold_low = LOW_BATTERY_VOLTAGE_THRESHOLD,
     .has_high_threshold = false,
     .error_flag = ERR_LOW_BAT,
     .name = "Low Battery",
     .voltage_divider_ratio = LOW_BATTERY_DIVIDER_RATIO},

    {.channel = ADC_OVER_UNDER_VOLTAGE, // Replace with actual ADC channel for AC voltage
     .channel_id = CHANNEL_ID_OVER_UNDER_VOLTAGE,
     .target_value = &sys_state.inverter.output_voltage,
     .threshold_low = UNDER_VOLTAGE_THRESHOLD,
     .threshold_high = OVER_VOLTAGE_THRESHOLD,
     .has_high_threshold = true,
     .error_flag = ERR_AC_FAULT,
     .name = "AC Voltage",
     .voltage_divider_ratio = AC_VOLTAGE_DIVIDER_RATIO},

    {.channel = ADC_BATTERY_VOLTAGE, // Replace with actual ADC channel for battery voltage
     .channel_id = CHANNEL_ID_BATTERY_VOLTAGE,
     .target_value = &sys_state.inverter.battery.voltage,
     .threshold_low = BATTERY_VOLTAGE_THRESHOLD,
     .threshold_high = 0,
     .has_high_threshold = false,
     .error_flag = ERR_BATTERY_VOLTAGE,
     .name = "Battery Voltage",
     .voltage_divider_ratio = BATTERY_VOLTAGE_DIVIDER_RATIO},

    {
        .channel = ADC_FAN, // Replace with actual ADC channel for fan
        .channel_id = CHANNEL_ID_FAN,
        .target_value = &sys_state.fan.speed,
        .threshold_low = FAN_SPEED_THRESHOLD,
        .has_high_threshold = false,
        .error_flag = ERR_FAN_FAIL,
        .name = "Fan Speed",
        .voltage_divider_ratio = FAN_SPEED_VOLTAGE_DIVIDER_RATIO // Direct connection, no divider
    },
    {.channel = ADC_INVERTER_OUTPUT_VOLTAGE, // Replace with actual ADC channel for inverter voltage
     .channel_id = CHANNEL_ID_INVERTER_OUTPUT_VOLTAGE,
     .target_value = &sys_state.inverter.output_voltage,
     .threshold_low = INVERTER_OUTPUT_VOLTAGE_THRESHOLD,
     .has_high_threshold = false,
     .error_flag = ERR_INVERTER_VOLTAGE,
     .name = "Inverter Voltage",
     .voltage_divider_ratio = INVERTER_VOLTAGE_DIVIDER_RATIO}};

#define ADC_MULTISAMPLING_COUNT 10 // Number of samples to average

#include "freertos/event_groups.h"

EventGroupHandle_t sys_event_group;

/* Event bits */
#define EVT_ADC_READY (1 << 0)
#define EVT_ADC_VALID (1 << 1)
#define EVT_DEEPSLEEP_RESTORED (1 << 2)

void adc_task(void *arg)
{
#if CONFIG_USE_ADC
    ESP_LOGI(TAG_ADC, "ADC Task started");

    // Get number of configured channels
    const int config_count = sizeof(adc_configs) / sizeof(adc_configs[0]);

    // Initialize ADC1
    adc_oneshot_unit_handle_t adc1_handle = NULL;

    // Allocate channel states (one per configured channel)
    adc_channel_state_t *adc1_states = (adc_channel_state_t *)calloc(config_count, sizeof(adc_channel_state_t));
    if (adc1_states == NULL)
    {
        ESP_LOGE(TAG_ADC, "Failed to allocate memory for channel states");
        vTaskDelete(NULL);
        return;
    }

    if (!initialize_adc_unit(&adc1_handle, ADC_UNIT_1))
    {
        ESP_LOGE(TAG_ADC, "Failed to initialize ADC1");
        free(adc1_states);
        vTaskDelete(NULL);
        return;
    }

    if (!configure_adc_channels(adc1_handle, adc_configs, adc1_states,
                                config_count, ADC_UNIT_1))
    {
        ESP_LOGE(TAG_ADC, "Failed to configure ADC channels");
        cleanup_adc_resources(adc1_handle, adc1_states, config_count);
        free(adc1_states);
        vTaskDelete(NULL);
        return;
    }

    adc_context_t adc1_context = {
        .handle = adc1_handle,
        .channel_states = adc1_states,
        .channel_count = config_count};

#if EXAMPLE_USE_ADC2
    // WARNING: ADC2 is shared with WiFi. If WiFi is enabled, ADC2 reads may fail.
    // Ensure WiFi is not active when using ADC2 or handle read failures gracefully.
    adc_oneshot_unit_handle_t adc2_handle = NULL;
    adc_channel_state_t *adc2_states = (adc_channel_state_t *)calloc(config_count, sizeof(adc_channel_state_t));

    if (adc2_states == NULL)
    {
        ESP_LOGE(TAG_ADC, "Failed to allocate memory for ADC2 channel states");
        cleanup_adc_resources(adc1_handle, adc1_states, config_count);
        free(adc1_states);
        vTaskDelete(NULL);
        return;
    }

    if (!initialize_adc_unit(&adc2_handle, ADC_UNIT_2))
    {
        ESP_LOGE(TAG_ADC, "Failed to initialize ADC2");
        cleanup_adc_resources(adc1_handle, adc1_states, config_count);
        free(adc1_states);
        free(adc2_states);
        vTaskDelete(NULL);
        return;
    }

    if (!configure_adc_channels(adc2_handle, adc_configs, adc2_states,
                                config_count, ADC_UNIT_2))
    {
        ESP_LOGE(TAG_ADC, "Failed to configure ADC2 channels");
        cleanup_adc_resources(adc1_handle, adc1_states, config_count);
        cleanup_adc_resources(adc2_handle, adc2_states, config_count);
        free(adc1_states);
        free(adc2_states);
        vTaskDelete(NULL);
        return;
    }
#endif

    ESP_LOGI(TAG_ADC, "ADC initialization complete");
#endif

    bool first_sample = true;
    uint8_t sample_count = 0;
    const uint8_t SAMPLES_BEFORE_ERROR_CHECK = 10; // ← ADD THIS

    ESP_LOGI(TAG_ADC, "Starting ADC sampling and LCD updates");

    while (1)
    {
        for (int i = 0; i < config_count; i++)
        {
            process_adc_reading(&adc_configs[i],
                                &adc1_context.channel_states[i],
                                adc1_context.handle);
        }

        if (sample_count < SAMPLES_BEFORE_ERROR_CHECK)
        {
            sys_state.error.error_flags = 0; // Clear errors during warmup
            sample_count++;
            ESP_LOGI(TAG_ADC, "ADC warmup: %d/%d", sample_count, SAMPLES_BEFORE_ERROR_CHECK);
        }
        xEventGroupSetBits(sys_event_group, EVT_ADC_READY | EVT_ADC_VALID);

        if (sample_count >= SAMPLES_BEFORE_ERROR_CHECK)
        {
            check_protections();
        }

        /* Update main screen data for lcd_task */
        lcd_update_main_data(
            sys_state.inverter.battery.voltage,
            sys_state.inverter.output_voltage,
            sys_state.inverter.output_current,
            sys_state.inverter.output_frequency,
            sys_state.inverter.battery.battery_temperature,
            sys_state.inverter.load_percentage,
            calculate_battery_percentage(sys_state.inverter.battery.voltage,
                                         sys_state.battery_profile.chemistry,
                                         (float)sys_state.battery_profile.nominal_voltage),
            sys_state.inverter.inverter_active,
            sys_state.inverter.connected,
            sys_state.battery_charging);

        /* Show fault screen immediately if error flags are set */
        if (sys_state.error.error_flags && sample_count >= SAMPLES_BEFORE_ERROR_CHECK)
        {
            const char *err = get_error_string(sys_state.error.error_flags);
            char l0[17], l1[17];
            snprintf(l0, 17, "%-16.16s", err);
            snprintf(l1, 17, "%-16s", "Check system    ");
            lcd_show_fault(l0, l1);
        }
        else if (sys_lcd.screen == LCD_SCREEN_FAULT)
        {
            /* Fault cleared — return to main */
            lcd_clear_fault();
        }

        if (first_sample && sample_count >= SAMPLES_BEFORE_ERROR_CHECK)
        {
            first_sample = false;
            ESP_LOGI(TAG_ADC, "First valid sample: Battery=%.2fV",
                     sys_state.inverter.battery.voltage);
            lcd_boot_complete();
            if (lcd_task_handle != NULL)
                xTaskNotifyGive(lcd_task_handle);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Cleanup (unreachable in current implementation, but good practice)
    cleanup_adc_resources(adc1_handle, adc1_states, config_count);
    free(adc1_states);

#if EXAMPLE_USE_ADC2
    cleanup_adc_resources(adc2_handle, adc2_states, config_count);
    free(adc2_states);
#endif
}

static bool initialize_adc_unit(adc_oneshot_unit_handle_t *handle, adc_unit_t unit_id)
{
    if (handle == NULL)
    {
        ESP_LOGE(TAG_ADC, "Invalid handle pointer");
        return false;
    }

    adc_oneshot_unit_init_cfg_t config = {
        .unit_id = unit_id,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t ret = adc_oneshot_new_unit(&config, handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_ADC, "Failed to initialize ADC unit %d: %s", unit_id, esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG_ADC, "ADC unit %d initialized successfully", unit_id);
    return true;
}

/*---------------------------------------------------------------
        ADC Calibration Function (Advanced)
---------------------------------------------------------------*/

bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    // Check if the handle pointer is valid
    if (out_handle == NULL)
    {
        ESP_LOGE("ADC_CALIB", "Invalid handle pointer");
        return false;
    }

    const char *ADC_CALIB_TAG = "ADC_CALIB";
    esp_err_t ret = ESP_FAIL;
    adc_cali_handle_t handle = NULL;
    bool calibrated = false; // FIXED: Removed 'static' - each channel needs its own calibration

    ESP_LOGI(ADC_CALIB_TAG, "Initializing ADC calibration for unit %d, channel %d, attenuation %d",
             unit, channel, atten);

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    // Try Curve Fitting calibration first
    ESP_LOGI(ADC_CALIB_TAG, "Attempting Curve Fitting calibration scheme");
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
    if (ret == ESP_OK)
    {
        calibrated = true;
        *out_handle = handle;
        ESP_LOGI(ADC_CALIB_TAG, "Curve Fitting calibration created successfully for channel %d", channel);
        return true;
    }
    else
    {
        ESP_LOGW(ADC_CALIB_TAG, "Curve Fitting calibration failed: %s", esp_err_to_name(ret));
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    // If Curve Fitting is not supported or failed, try Line Fitting
    if (!calibrated)
    {
        ESP_LOGI(ADC_CALIB_TAG, "Attempting Line Fitting calibration scheme");
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };

        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK)
        {
            calibrated = true;
            *out_handle = handle;
            ESP_LOGI(ADC_CALIB_TAG, "Line Fitting calibration created successfully for channel %d", channel);
            return true;
        }
        else
        {
            ESP_LOGW(ADC_CALIB_TAG, "Line Fitting calibration failed: %s", esp_err_to_name(ret));
        }
    }
#endif

    // If we get here, calibration failed
    *out_handle = NULL;

    // Log specific error
    switch (ret)
    {
    case ESP_ERR_INVALID_ARG:
        ESP_LOGE(ADC_CALIB_TAG, "Invalid argument for calibration scheme");
        break;
    case ESP_ERR_NO_MEM:
        ESP_LOGE(ADC_CALIB_TAG, "No memory for calibration scheme");
        break;
    case ESP_ERR_NOT_FOUND:
        ESP_LOGE(ADC_CALIB_TAG, "Calibration data not found");
        break;
    case ESP_ERR_NOT_SUPPORTED:
        ESP_LOGE(ADC_CALIB_TAG, "Calibration scheme not supported on this chip");
        break;
    case ESP_ERR_INVALID_STATE:
        ESP_LOGE(ADC_CALIB_TAG, "Invalid state for calibration scheme");
        break;
    default:
        ESP_LOGE(ADC_CALIB_TAG, "Calibration failed with error: %s (0x%x)",
                 esp_err_to_name(ret), ret);
        break;
    }

    return false;
}

static void adc_calibration_deinit(adc_cali_handle_t handle)
{
    if (handle == NULL)
    {
        return;
    }

    const char *TAG = "ADC_CALIBRATION";

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    ESP_LOGI(TAG, "Deregistering Curve Fitting calibration scheme");
    esp_err_t ret = adc_cali_delete_scheme_curve_fitting(handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to delete Curve Fitting scheme: %s", esp_err_to_name(ret));
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    ESP_LOGI(TAG, "Deregistering Line Fitting calibration scheme");
    esp_err_t ret = adc_cali_delete_scheme_line_fitting(handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to delete Line Fitting scheme: %s", esp_err_to_name(ret));
    }
#endif
}

static bool configure_adc_channels(adc_oneshot_unit_handle_t handle,
                                   const adc_channel_config_t *configs,
                                   adc_channel_state_t *states,
                                   int channel_count,
                                   adc_unit_t unit_id)
{
    if (handle == NULL || configs == NULL || states == NULL)
    {
        ESP_LOGE(TAG_ADC, "Invalid parameters for channel configuration");
        return false;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_USED};

    bool at_least_one_success = false;

    for (int i = 0; i < channel_count; i++)
    {
        adc_channel_t channel = configs[i].channel;

        // Initialize state
        states[i].cali_handle = NULL;
        states[i].is_calibrated = false;

        // Configure the channel
        esp_err_t ret = adc_oneshot_config_channel(handle, channel, &chan_cfg);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG_ADC, "Failed to configure %s (channel %d): %s",
                     configs[i].name, channel, esp_err_to_name(ret));
            continue;
        }

        // Try to initialize calibration for successfully configured channels
        states[i].is_calibrated = adc_calibration_init(unit_id, channel, ADC_ATTEN_USED,
                                                       &states[i].cali_handle);

        if (states[i].is_calibrated)
        {
            ESP_LOGI(TAG_ADC, "%s (channel %d) configured and calibrated successfully",
                     configs[i].name, channel);
        }
        else
        {
            ESP_LOGW(TAG_ADC, "%s (channel %d) configured but calibration failed - using raw conversion",
                     configs[i].name, channel);
        }

        at_least_one_success = true;
    }

    return at_least_one_success;
}

static esp_err_t read_adc_with_multisampling(adc_oneshot_unit_handle_t handle,
                                             adc_channel_t channel,
                                             adc_cali_handle_t cali_handle,
                                             bool is_calibrated,
                                             float *out_voltage,
                                             uint8_t samples)
{
    if (handle == NULL || out_voltage == NULL || samples == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t sum_raw = 0;
    int32_t sum_voltage = 0;
    uint8_t valid_samples = 0;

    for (uint8_t i = 0; i < samples; i++)
    {
        int raw_value = 0;
        esp_err_t ret = adc_oneshot_read(handle, channel, &raw_value);

        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG_ADC, "ADC read failed for channel %d: %s", channel, esp_err_to_name(ret));
            continue;
        }

        if (is_calibrated && cali_handle != NULL)
        {
            int voltage_mv = 0;
            ret = adc_cali_raw_to_voltage(cali_handle, raw_value, &voltage_mv);
            if (ret == ESP_OK)
            {
                sum_voltage += voltage_mv;
                valid_samples++;
            }
            else
            {
                ESP_LOGW(TAG_ADC, "Calibration conversion failed: %s", esp_err_to_name(ret));
            }
        }
        else
        {
            ESP_LOGI(TAG_ADC, "Raw ADC value for channel %d: %d", channel, raw_value);
            sum_raw += raw_value;
            valid_samples++;
        }
    }

    if (valid_samples == 0)
    {
        ESP_LOGE(TAG_ADC, "No valid samples obtained for channel %d", channel);
        return ESP_ERR_INVALID_STATE;
    }

    // Calculate average and convert to volts
    if (is_calibrated && cali_handle != NULL)
    {
        // Use calibrated voltage in millivolts, convert to volts
        *out_voltage = (float)(sum_voltage / valid_samples) / 1000.0f;
    }
    else
    {
        // Fallback: approximate conversion without calibration
        // For 12-bit ADC with 11dB attenuation, rough estimate is ~3.3V / 4095
        int avg_raw = sum_raw / valid_samples;
        *out_voltage = (float)avg_raw * 3.3f / 4095.0f;
        ESP_LOGW(TAG_ADC, "Using uncalibrated ADC reading for channel %d", channel);
    }

    return ESP_OK;
}

/*==============================================================================
  ESP32 ADC VOLTAGE RANGE - WHY 0.4V to 3.12V instead of 0V to 3.3V?
==============================================================================*/

/* THE PROBLEM:
   Potentiometer connected to 3.3V reads:
   - Minimum: 0.4V (should be 0V)
   - Maximum: 3.12V (should be 3.3V)

   WHY? ESP32 ADC at 11dB attenuation has limited linear range:
   - Theoretical: 0-3.3V
   - Actual usable: ~0.15V to 2.45-3.1V (varies by chip)
   - Below 0.15V and above 3.0V: Non-linear and inaccurate
*/

// These values are based on your actual measurements
// Adjust if your specific ESP32 chip shows different values
#define ADC_MEASURED_MIN 0.4f  // Minimum voltage ADC can read accurately
#define ADC_MEASURED_MAX 3.12f // Maximum voltage ADC can read accurately
#define ADC_TARGET_MIN 0.0f    // Target minimum (what you want)
#define ADC_TARGET_MAX 3.3f    // Target maximum (what you want)

// Enable/disable mapping per channel (in case some channels don't need it)
#define ENABLE_ADC_RANGE_MAPPING 1

/*------------------------------------------------------------------------------
  QUICK FIX: Software Mapping (No Hardware Changes)
------------------------------------------------------------------------------*/

/**
 * @brief Maps limited ADC range to full target range
 *
 * ESP32 ADC at 11dB attenuation reads 0.4V-3.12V instead of 0-3.3V.
 * This function maps the limited range to the full 0-3.3V range.
 *
 * @param adc_voltage Raw voltage from ADC (0.4-3.12V)
 * @return Mapped voltage (0-3.3V)
 */
static float map_adc_to_full_range(float adc_voltage)
{
#if ENABLE_ADC_RANGE_MAPPING
    // Clamp input to measured range
    if (adc_voltage < ADC_MEASURED_MIN)
    {
        adc_voltage = ADC_MEASURED_MIN;
    }
    if (adc_voltage > ADC_MEASURED_MAX)
    {
        adc_voltage = ADC_MEASURED_MAX;
    }

    // Linear mapping: y = (x - x_min) / (x_max - x_min) * (y_max - y_min) + y_min
    float mapped = ((adc_voltage - ADC_MEASURED_MIN) /
                    (ADC_MEASURED_MAX - ADC_MEASURED_MIN)) *
                       (ADC_TARGET_MAX - ADC_TARGET_MIN) +
                   ADC_TARGET_MIN;

    return mapped;
#else
    return adc_voltage; // No mapping
#endif
}

static void process_adc_reading(const adc_channel_config_t *config,
                                const adc_channel_state_t *state,
                                adc_oneshot_unit_handle_t handle)
{

    if (config == NULL || state == NULL)
    {
        ESP_LOGE(TAG_ADC, "Invalid parameters for ADC processing");
        return;
    }

    // Read ADC with multisampling
    float adc_voltage = 0.0f;
    esp_err_t ret = read_adc_with_multisampling(
        handle,
        config->channel,
        state->cali_handle,
        state->is_calibrated,
        &adc_voltage,
        ADC_MULTISAMPLING_COUNT);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_ADC, "Failed to read %s: %s", config->name, esp_err_to_name(ret));
        return;
    }

    // Validate voltage divider ratio
    if (config->voltage_divider_ratio <= 0.0f)
    {
        ESP_LOGE(TAG_ADC, "%s: Invalid voltage divider ratio: %.4f",
                 config->name, config->voltage_divider_ratio);
        return;
    }

    adc_voltage = map_adc_to_full_range(adc_voltage); // Apply software mapping to correct non-linearity

    // Calculate actual voltage
    float actual_voltage = adc_voltage * config->voltage_divider_ratio;

    // IMPORTANT: Write the value to sys_state
    *(config->target_value) = actual_voltage;

    // Check thresholds and set error flags
    bool error_detected = false;

    if (config->has_high_threshold)
    {
        // Check both low and high thresholds
        error_detected = (actual_voltage < config->threshold_low ||
                          actual_voltage > config->threshold_high);
    }
    else
    {
        // Only check low threshold
        error_detected = (actual_voltage < config->threshold_low);
    }

    // Update error flags
    if (error_detected)
    {
        sys_state.error.error_flags |= config->error_flag;

        char threshold_high_str[16];
        if (config->has_high_threshold)
        {
            snprintf(threshold_high_str, sizeof(threshold_high_str), "%.2f", config->threshold_high);
        }
        else
        {
            snprintf(threshold_high_str, sizeof(threshold_high_str), "N/A");
        }
    }
    else
    {
        sys_state.error.error_flags &= ~config->error_flag;
    }
}

static void cleanup_adc_resources(adc_oneshot_unit_handle_t handle,
                                  adc_channel_state_t *states,
                                  int channel_count)
{
    if (states != NULL)
    {
        for (int i = 0; i < channel_count; i++)
        {
            if (states[i].cali_handle != NULL)
            {
                adc_calibration_deinit(states[i].cali_handle);
                states[i].cali_handle = NULL;
                states[i].is_calibrated = false;
            }
        }
    }

    if (handle != NULL)
    {
        adc_oneshot_del_unit(handle);
    }
}

void power_task(void *arg)
{
    static bool last_relay_state = false;
    static TickType_t last_state_change = 0;
    const TickType_t DEBOUNCE_TIME = pdMS_TO_TICKS(2000);

    while (1)
    {
        // Update activity tracking
        if (sys_state.inverter.inverter_active || sys_state.inverter.connected)
            sys_state.flags.last_power_event = xTaskGetTickCount();

        // Check for inactivity timeout
        if ((xTaskGetTickCount() - sys_state.flags.last_power_event) >
            pdMS_TO_TICKS(30 * 60 * 1000))
        {
            ESP_LOGI("POWER_TASK", "Entering deep sleep");
            enter_deep_sleep(3600);
            // Never returns from here
        }

        // ✅ CRITICAL TEMP CHECK - Separate from relay logic
        // Note: This is for later version of the code
        // Reason: No temperature sensor currently, but when implemented, we want to ensure that the relay control logic is not nested inside the temperature check. This allows the system to still switch to battery mode if AC fails, even if the temperature is high (but not critical). The critical temp check will only trigger a shutdown if it exceeds the threshold, but won't prevent relay switching in non-critical conditions.
        // if (sys_state.inverter.temperature > 90.0f)
        // {
        //     ESP_LOGE("POWER_TASK", "CRITICAL TEMP: %.1f°C - SHUTTING DOWN!",
        //              sys_state.inverter.temperature);
        //     lcd_show_fault("Critical Temp!  ", "Shutting Down...");
        //     vTaskDelay(pdMS_TO_TICKS(3000));
        //     perform_system_restart(false);
        //     // Execution never reaches here
        // }

        // ✅ RELAY CONTROL - Now at correct level (NOT nested in temp check)
        bool new_relay_state;
        if (sys_state.inverter.connected &&
            !(sys_state.error.error_flags & ERR_AC_FAULT))
        {
            new_relay_state = true; // AC source available
        }
        else
        {
            if (!(sys_state.error.error_flags &
                  (ERR_OVER_TEMP | ERR_OVERLOAD | ERR_LOW_BAT | ERR_HIGH_BAT)))
            {
                new_relay_state = false; // Use inverter (no errors)
            }
            else
            {
                new_relay_state = last_relay_state; // Keep current state
            }
        }

        // Toggle relay if state changed
        if (new_relay_state != last_relay_state)
        {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_state_change) > DEBOUNCE_TIME)
            {
                gpio_set_level(GPIO_POWER_RELAY, new_relay_state ? 1 : 0);
                sys_state.inverter.inverter_active = !new_relay_state;
                last_relay_state = new_relay_state;
                last_state_change = now;

                ESP_LOGI("POWER_TASK", "Relay: %s (%s)",
                         new_relay_state ? "AC" : "INVERTER",
                         new_relay_state ? "Grid connected" : "Battery mode");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void check_protections(void)
{
    uint32_t now_ms =
        xTaskGetTickCount() * portTICK_PERIOD_MS;

    sys_state.error.error_flags &=
        (ERR_EEPROM | ERR_FAN_FAIL);

    protection_update(
        PROT_QUANTITY_AC_VOLTAGE,
        sys_state.inverter.output_voltage,
        now_ms);

    protection_update(
        PROT_QUANTITY_OUTPUT_CURRENT,
        sys_state.inverter.output_current,
        now_ms);

    protection_update(
        PROT_QUANTITY_TEMPERATURE,
        sys_state.inverter.temperature,
        now_ms);

    protection_update(
        PROT_QUANTITY_BATTERY_VOLTAGE,
        sys_state.inverter.battery.voltage,
        now_ms);

    if (sys_state.inverter.temperature > 70.0f &&
        !sys_state.fan.connected)
    {
        sys_state.error.error_flags |=
            ERR_FAN_FAIL;
    }
    else
    {
        sys_state.error.error_flags &=
            ~ERR_FAN_FAIL;
    }
}

void update_led_status()
{
    // Status LED (green) – indicate whether inverter is active or AC connected
    if (sys_state.inverter.inverter_active)
    {
        update_led(LED_STATUS, 100); // Green: Inverter active
    }
    else if (sys_state.inverter.connected)
    {
        update_led(LED_STATUS, 100); // Green: AC connected
    }
    else
    {
        update_led(LED_STATUS, 0); // Off: Neither active
    }

    // Error LEDs

    // Temperature or Fan Failure Error LED (Yellow/Orange)
    if (sys_state.error.error_flags & (ERR_OVER_TEMP | ERR_FAN_FAIL))
    {
        blink_led(LED_ERROR, 200, 200, 3); // Blink: Temp or Fan error
    }
    else
    {
        update_led(LED_ERROR, 0); // Off
    }

    // Battery Voltage Error LED (Blue)
    if (sys_state.error.error_flags & (ERR_LOW_BAT | ERR_HIGH_BAT))
    {
        blink_led(LED_ERROR, 200, 200, 3); // Blue: Battery error
    }
    else
    {
        update_led(LED_ERROR, 0); // Off
    }

    // Optional: Blinking or flashing logic for critical errors
    // E.g., if temperature is too high, blink TEMP LED
    if (sys_state.error.error_flags & (ERR_OVER_TEMP | ERR_FAN_FAIL))
    {
        static uint32_t last_blink_time = 0;
        if ((xTaskGetTickCount() - last_blink_time) > pdMS_TO_TICKS(500))
        {
            blink_led(LED_ERROR, 200, 200, 3); // Toggle LED every 500ms
            last_blink_time = xTaskGetTickCount();
        }
    }
}

/**
 * @brief Initialize click detection system
 */
void click_detection_init()
{
    for (int i = 0; i < BUTTON_MAX_CLICK_COUNT; i++)
    {
        g_click_detectors[i].click_count = 0;
        g_click_detectors[i].first_click_time = 0;
        g_click_detectors[i].last_release_time = 0;
        g_click_detectors[i].waiting_for_timeout = false;
        g_click_detectors[i].button_id = (button_id_t)i;
    }
    printf("Click detection system initialized\n");
}
// Timing constants for click detection
#define CLICK_TIMEOUT_MS 400 // Max time between clicks for multi-click detection

// Update menu screen header for 16x2 LCD - MAIN UPDATE FUNCTION
/**
 * @brief Update 16x2 LCD with current menu and selection
 */
/* ── lcd_update_menu_screen() ───────────────────────────────────────────── */
void lcd_update_menu_screen(void)
{
    show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
}

void update_lcd_activity_state(void)
{
    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);

    /* Copy activity state to LCD render state */
    sys_lcd.last_user_activity = sys_state.flags.last_user_activity;
    sys_lcd.inverter_active = sys_state.inverter.inverter_active;
    sys_lcd.inverter_connected = sys_state.inverter.connected;

    /* Determine if we should cycle sub-pages */
    bool on_main_screen = (sys_lcd.screen == LCD_SCREEN_MAIN);
    bool inverter_idle = !sys_state.inverter.inverter_active &&
                         !sys_state.inverter.connected;

    TickType_t now_ticks = xTaskGetTickCount();
    uint32_t idle_time_ms = (now_ticks - sys_state.flags.last_user_activity) *
                            portTICK_PERIOD_MS;
    bool user_idle = (idle_time_ms > 5000);

    sys_lcd.should_cycle_subpages = on_main_screen && inverter_idle && user_idle;

    xSemaphoreGive(sys_state_mutex);
}

// Special characters for 16x2 LCD
#define CHAR_SELECTED 0x3E // >

// Get menu items for current state
static const menu_item_t *get_menu_items(menu_state_t state, int *item_count)
{
    if (!item_count)
        return NULL;
    switch (state)
    {
    case MAIN_MENU:
        *item_count = sizeof(main_menu_items) / sizeof(main_menu_items[0]);
        return main_menu_items;

    case MENU_SETTINGS:
        *item_count = sizeof(settings_items) / sizeof(settings_items[0]);
        return settings_items;

    case MENU_MONITORING:
        *item_count = sizeof(monitoring_items) / sizeof(monitoring_items[0]);
        return monitoring_items;

    case MENU_DIAGNOSTIC:
        *item_count = sizeof(diagnostic_items) / sizeof(diagnostic_items[0]);
        return diagnostic_items;

    case MENU_WIFI_CONFIG:
        *item_count = sizeof(wifi_items) / sizeof(wifi_items[0]);
        return wifi_items;

    case MENU_FACTORY_RESET:
        *item_count = sizeof(factory_reset_items) / sizeof(factory_reset_items[0]);
        return factory_reset_items;
    case MENU_SECURITY:
        *item_count = sizeof(security_items) / sizeof(security_items[0]);
        return security_items;
    default:
        *item_count = 0;
        return NULL;
    }
}

/* ── lcd_show_monitoring_detail() ──────────────────────────────────────── */
void lcd_show_monitoring_detail(const char *label, float value,
                                const char *unit)
{
    char l[17], v[17];
    snprintf(l, 17, "%-16.16s", label);
    snprintf(v, 17, "%.2f %-6.6s", value, unit ? unit : "");
    lcd_show_monitor_detail(l, v);
}

void menu_navigate_down(void)
{
    int num_items = 0;
    const menu_item_t *items = get_menu_items(sys_state.menu_state, &num_items);
    if (!items || num_items == 0)
        return;

    sys_state.menu_selection = (sys_state.menu_selection + 1) % num_items;
    if (sys_state.menu_selection >= num_items)
        sys_state.menu_selection = 0;
    sys_state.last_activity_time = esp_timer_get_time() / 1000;
}

/*==============================================================================
  CONVENIENCE: switch to menu screen with freshly-built rows
==============================================================================*/
static void show_menu_screen(menu_state_t menu_st, int selection)
{
    const uint8_t security_menu_count = 3;

    if (menu_st == MENU_SECURITY)
    {
        if (selection < 0)
            selection = 0;
        if (selection >= security_menu_count)
            selection = security_menu_count - 1;

        LCD_LOCK();
        sys_lcd.screen = LCD_SCREEN_SECURITY;
        atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_IDLE);
        atomic_store(&sys_lcd.security.action, SECURITY_ACTION_NONE);
        atomic_store(&sys_lcd.security.menu_selection, (uint8_t)selection);
        LCD_UNLOCK();
        return;
    }

    char r0[17], r1[17];

    int item_count = 0;
    const menu_item_t *items = get_menu_items(menu_st, &item_count);

    if (!items || item_count == 0)
    {
        snprintf(r0, 17, "%-16s", "(empty menu)");
        snprintf(r1, 17, "%-16s", "");
        return;
    }

    if (selection < 0)
        selection = 0;
    if (selection >= item_count)
        selection = item_count - 1;

    /* Row 0: ">Label           " */
    snprintf(r0, 17, "%c%-15.15s", MENU_ARROW, items[selection].label);

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
        snprintf(r1, 17, "%c%-*.*s%s",
                 MENU_INDENT, label_w, label_w,
                 items[next].label, ind);
    }
    else
    {
        snprintf(r1, 17, "%c%-15.15s", MENU_INDENT, items[next].label);
    }
    lcd_show_menu(r0, r1);
}

/*==============================================================================
  CONVENIENCE: switch to main screen (wraps lcd_show_main)
==============================================================================*/
static void go_to_main_screen(void)
{
    lcd_show_main();
}

void menu_navigate_up(void)
{
    int num_items = 0;
    const menu_item_t *items = get_menu_items(sys_state.menu_state, &num_items);
    if (!items || num_items == 0)
        return;

    sys_state.menu_selection = (sys_state.menu_selection - 1 + num_items) % num_items;
    sys_state.last_activity_time = esp_timer_get_time() / 1000;
}

void menu_go_back(void)
{
    switch (sys_state.menu_state)
    {
    case MENU_NONE:
        // Already at main screen, nothing to do
        break;
    case MENU_SETTINGS:
    case MENU_MONITORING:
    case MENU_DIAGNOSTIC:
    case MENU_WIFI_CONFIG:
        sys_state.menu_state = MAIN_MENU;
        sys_state.menu_selection = 0;
        sys_state.max_menu_items = sizeof(main_menu_items) / sizeof(main_menu_items[0]);
        break;
    default:
        sys_state.menu_state = MENU_NONE;
        break;
    }
    printf("Menu back to: %d\n", sys_state.menu_state);
}

float esp_cpu_get_usage_percent()
{
    // Do the mathematics here
    // return CPU usage percentage
    return 42.0f; // Placeholder value
}

void diagnostic_update_task(void *pv)
{
    while (1)
    {
        diag_data.uptime_seconds++;
        diag_data.cpu_load = esp_cpu_get_usage_percent(); // If you have CPU metrics
        diag_data.temperature = sys_state.inverter.temperature;
        diag_data.ram_usage = esp_get_free_heap_size() / 1024.0;
        diag_data.system_ok = (diag_data.temperature < 75.0);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#define MAX_AP_NUM 10
static const char *WIFI_TAG = "WiFiScan";

static wifi_ap_record_t ap_records[MAX_AP_NUM];
static uint16_t ap_count = 0;

/**
 * @brief Initialize WiFi stack (STA mode)
 */
static void wifi_init_sta(void)
{
    static bool initialized = false;
    if (initialized)
        return;

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    initialized = true;
}

/**
 * @brief Start WiFi scan and store results globally
 */
void start_wifi_scan(void)
{
    ESP_LOGI(WIFI_TAG, "Starting WiFi Scan...");

    wifi_init_sta();

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true)); // Block until complete
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    if (ap_count > MAX_AP_NUM)
        ap_count = MAX_AP_NUM;

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_records));

    ESP_LOGI(WIFI_TAG, "Found %d access points", ap_count);
    for (int i = 0; i < ap_count; i++)
    {
        ESP_LOGI(WIFI_TAG, "SSID: %-32s RSSI: %d dBm", ap_records[i].ssid, ap_records[i].rssi);
    }
}

/**
 * @brief Display WiFi scan results on LCD
 */

#define WIFI_SCAN_TIMEOUT_MS 30000 // 30s inactivity timeout

/**
 * @brief Display WiFi scan results on 16x2 LCD with scrolling and signal bars
 */
/* ── lcd_show_wifi_scan_screen() ─────────────────────────────────────────── */
void lcd_show_wifi_scan_screen(void)
{
    if (ap_count == 0)
    {
        lcd_flash_info("No Networks     ", "Found           ", 2000);
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
            lcd_flash_info("Scan Timeout    ", "                ", 1200);
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
            case BTN_ENTER:
                strncpy((char *)sys_state.wifi.ssid,
                        (char *)ap_records[index].ssid,
                        sizeof(sys_state.wifi.ssid) - 1);
                lcd_show_wifi_connecting(sys_state.wifi.ssid);
                start_wifi_connection();
                vTaskDelay(pdMS_TO_TICKS(2000));
                return;
            case BTN_BACK:
                lcd_flash_info("Exiting Scan    ", "                ", 1000);
                return;
            default:
                break;
            }
        }
    }
}

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static EventGroupHandle_t s_wifi_event_group;

#define MAX_RETRY 5

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

void reload_default_settings(void)
{
    const char *TAG = "Default_config";
    sys_state.inverter.output_voltage = 230.0f;
    sys_state.inverter.output_frequency = 50.0f;
    sys_state.current_limit = 10.0f;
    sys_state.temperature_limit = 70.0f;
    sys_state.system_timeout = 60;

    sys_state.wifi.enabled = false;

    snprintf(sys_state.wifi.ssid, sizeof(sys_state.wifi.ssid), "%s", "default_ssid");
    snprintf(sys_state.wifi.password, sizeof(sys_state.wifi.password), "%s", "password123");

    ESP_LOGI(TAG, "Default configuration loaded into RAM.");
}

#define LOG_TAG "LOG_MANAGER"
#define MAX_ERROR_LOG_ENTRIES 10

static error_log_entry_t error_log_ring[MAX_ERROR_LOG_ENTRIES];
static uint8_t error_log_head = 0;  // next write index (wraps)
static uint8_t error_log_count = 0; // total entries (capped at MAX_ERROR_LOG_ENTRIES)

/**
 * @brief Record a fault in the ring buffer and update the singleton description.
 *
 * Call this wherever log_error_to_nvs() is called so the in-RAM log stays
 * in sync with NVS. Overwrites the oldest entry once the buffer is full.
 *
 * @param code        system_errors_t cast to uint8_t
 * @param description Short human-readable string (truncated to 31 chars)
 */
void error_log_record(uint8_t code, const char *description)
{
    // Write into ring buffer
    error_log_entry_t *slot = &error_log_ring[error_log_head];
    slot->error_code = code;
    slot->timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    strncpy(slot->description, description ? description : "?", sizeof(slot->description) - 1);
    slot->description[sizeof(slot->description) - 1] = '\0';

    error_log_head = (error_log_head + 1) % MAX_ERROR_LOG_ENTRIES;
    if (error_log_count < MAX_ERROR_LOG_ENTRIES)
        error_log_count++;

    // Mirror into singleton for quick access
    strncpy(sys_state.error.last_error_msg, slot->description,
            sizeof(sys_state.error.last_error_msg) - 1);
    sys_state.error.last_error_msg[sizeof(sys_state.error.last_error_msg) - 1] = '\0';
}

/**
 * @brief Get the most recent log entry, or NULL if none exist.
 */
const error_log_entry_t *error_log_get_latest(void)
{
    if (error_log_count == 0)
        return NULL;

    // head points to the NEXT write slot, so latest = head - 1 (wrapped)
    uint8_t latest = (error_log_head + MAX_ERROR_LOG_ENTRIES - 1) % MAX_ERROR_LOG_ENTRIES;
    return &error_log_ring[latest];
}

/**
 * @brief Clear all log entries (call from erase_all_logs()).
 */
void error_log_clear(void)
{
    memset(error_log_ring, 0, sizeof(error_log_ring));
    error_log_head = 0;
    memset(sys_state.error.last_error_msg, 0, sizeof(sys_state.error.last_error_msg));
    strncpy(sys_state.error.last_error_msg, "No logs",
            sizeof(sys_state.error.last_error_msg) - 1);
}

/* ── calibration_reset() ────────────────────────────────────────────────── */
void calibration_reset(void)
{
    ESP_LOGI(TAG_SYS, "Resetting ADC calibration to defaults");

    atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_PROGRESS);
    atomic_store(&sys_lcd.factory_reset.progress_pct, 0);

    /* Step 1: reset in-RAM calibration values (fast, no I/O) */
    for (int i = 0; i < ADC_CHANNEL_USE; i++)
    {
        adc_calibration[i].calibration_values[0] = 0.0f; // Offset
        adc_calibration[i].calibration_values[1] = 1.0f; // Gain
        adc_calibration[i].calibrated = false;
        adc_calibration[i].calibration_mode = false;
    }
    atomic_store(&sys_lcd.factory_reset.progress_pct, 40);
    vTaskDelay(pdMS_TO_TICKS(150));

    /* Step 2: persist the reset calibration blob to NVS.
     * Uses the same "adc_cal" key under NVS_NS_SYSTEM as save_calibration(),
     * so this does not touch settings or battery profile keys stored in
     * the same namespace. */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &h);
    if (err == ESP_OK)
    {
        err = nvs_set_blob(h, "adc_cal", adc_calibration, sizeof(adc_calibration));
        if (err == ESP_OK)
        {
            err = nvs_commit(h);
        }
        nvs_close(h);

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_SYS, "Failed to save reset calibration: %s", esp_err_to_name(err));
            atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_CONFIRM);
            lcd_flash_info("Reset Failed!   ", "                ", 1500);
            return;
        }
    }
    else
    {
        ESP_LOGW(TAG_SYS, "Failed to open NVS for calibration reset: %s", esp_err_to_name(err));
        atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_CONFIRM);
        lcd_flash_info("Reset Failed!   ", "                ", 1500);
        return;
    }
    atomic_store(&sys_lcd.factory_reset.progress_pct, 100);
    vTaskDelay(pdMS_TO_TICKS(200));

    update_buzzer(1000, 50);
    vTaskDelay(pdMS_TO_TICKS(150));
    buzzer_off();

    sys_state.power_button_sequence_count = 0;
    /* Only the calling task decides DONE, and only after the work above
     * has actually completed — not the draw path. */
    atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_DONE);
    ESP_LOGI(TAG_SYS, "ADC calibration reset to defaults");
}

typedef struct
{
    int last_selection;
    int visible_start;
    uint32_t last_scroll_time;
    uint32_t last_anim_time;
    uint8_t text_offset[LCD_ROWS];
} lcd_menu_render_t;

static inline uint32_t time_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

#define MENU_INDICATOR_MIN_ITEMS 3 // show "cur/total" only when 3+ items
#define MENU_INDICATOR_MAX_LEN 6   // max chars for indicator e.g. "10/10\0"
#define MENU_ARROW '>'
#define MENU_INDENT ' '

/**
 * @brief Draw current and next menu item on a 16×2 LCD with position indicator.
 *
 * Layout (16 chars per row):
 *   Row 0: ">Label__________"   (selected item, left-justified, arrow prefix)
 *   Row 1: " Label______2/5 "   (next item + position counter when ≥3 items)
 *
 * @param menu_state  Active menu whose item list will be looked up.
 * @param selection   Index of the currently highlighted item (0-based).
 *                    Clamped internally if out of range.
 */
/* ── lcd_draw_menu_scroll() ─────────────────────────────────────────────── */
/* All callers of lcd_draw_menu_scroll() replaced by show_menu_screen().    */
/* This wrapper keeps any remaining direct calls compiling.                  */
void lcd_draw_menu_scroll(menu_state_t menu_st, int selection)
{
    show_menu_screen(menu_st, selection);
}

// == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == ==
// VALUE EDIT MODE DISPLAY
// ============================================================================

// Display value edit mode screen while editing
/* ── lcd_show_value_edit_screen() ──────────────────────────────────────── */
void lcd_show_value_edit_screen(void)
{
    value_edit_context_t *config = get_current_value_config();
    if (!config)
    {
        lcd_show_value_edit("Error: No param ", "                ", false);
        return;
    }

    char v[17];
    switch (config->edit_type)
    {
    case VALUE_EDIT_NUMERIC:
        snprintf(v, 17, "%.*f %-11.11s", config->decimal_places,
                 config->current_value, config->unit ? config->unit : "");
        break;
    case VALUE_EDIT_BOOL:
        snprintf(v, 17, "%-16s", config->current_value != 0.0f ? "ON" : "OFF");
        break;
    case VALUE_EDIT_SELECT:
        snprintf(v, 17, "%-16.16s", config->options[config->selection_index]);
        break;
    default:
        snprintf(v, 17, "%-16s", "");
        break;
    }
    lcd_show_value_edit(config->label ? config->label : "Param",
                        v, sys_state.pending_confirmation);
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
                case BTN_ENTER:
                    lcd_show_factory_progress(0);
                    perform_factory_reset();
                    lcd_show_factory_done();
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    sys_state.menu_state = MAIN_MENU;
                    show_menu_screen(MAIN_MENU, 0);
                    waiting = false;
                    break;
                case BTN_BACK:
                    lcd_flash_info("Cancelled       ", "                ", 800);
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
            lcd_flash_info("Timeout         ", "                ", 800);
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

// ============================================================================
// COMPLETE MENU REFRESH (Call this from button handlers)
// ============================================================================

// Enter detail view (call when showing monitoring/diagnostic details)
void enter_detail_view(menu_state_t parent_menu, int parent_selection)
{
    /* Snapshot the real inverter state now, before enter_diagnostic_mode()
     * or anything else overwrites it with INVERTER_DIAGNOSTIC. */
    sys_state.pre_detail_inverter_state = sys_state.inverter.inverter_state;

    sys_state.in_detail_view = true;
    sys_state.detail_parent_menu = parent_menu;
    sys_state.detail_parent_selection = parent_selection;

    ets_printf("enter_detail_view: menu=%d sel=%d saved_inv=%d\n",
               parent_menu, parent_selection,
               sys_state.pre_detail_inverter_state);
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
void handle_power_button_event(button_event_info_t *event_info,
                               void *user_data)
{
    if (!sys_state.system_ready)
        return;

    if (event_info->event == BUTTON_EVENT_PRESS)
    {
        post_button_click_event();
    }
    int64_t current_time = event_info->timestamp_us / 1000;

    /* Never let the power button interrupt an in-progress factory reset.
     * The erase/format sequence must run to completion undisturbed. */
    if (atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_PHASE_PROGRESS)
    {
        return;
    }

    if (sys_state.menu_state == MENU_FACTORY_RESET &&
        atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_RESET_PIN_ENTRY)
    {
        if (event_info->event == BUTTON_EVENT_CLICK)
        {
            factory_reset_handle_pin_entry(&sys_state.factory_reset, BTN_POWER);
        }
        return;
    }

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
            sys_state.current_value_type = NULL;
            lcd_flash_info("Edit Cancelled  ", "                ", 600);
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
                /* Re-enter diagnostic display with the real, live value —
                 * no more reconstructing a blank row via a compound literal. */
                lcd_draw_diagnostics_screen(sys_state.menu_selection);
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
        /* P5: close menu (also clears any pending factory-reset action
         * so we don't leave stale state behind) */
        if (sys_state.menu_state != MENU_NONE)
        {
            atomic_store(&sys_lcd.factory_reset.action, FACTORY_ACTION_NONE);
            atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_IDLE);
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
            uint8_t pct = calculate_battery_percentage(
                sys_state.inverter.battery.voltage,
                sys_state.battery_profile.chemistry,
                (float)sys_state.battery_profile.nominal_voltage);
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

    case BUTTON_EVENT_LONG_PRESS:
    {

        if (sys_state.value_edit_mode)
        {
            lcd_flash_info("Save/Cancel     ", "value first     ", 1200);
            break;
        }
        if (sys_state.inverter.inverter_state == INVERTER_DIAGNOSTIC)
        {
            exit_diagnostic_mode();
            break;
        }
        /* Confirmed factory reset via triple-click + hold */
        if (sys_state.menu_state == MENU_FACTORY_RESET &&
            sys_state.power_button_sequence_count > 0)
        {
            sys_state.power_button_sequence_count = 0;
            clear_menu_history();
            lcd_show_factory_progress(0);
            vTaskDelay(pdMS_TO_TICKS(500));
            perform_factory_reset();
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
            /* shutdown_inverter() already sets menu_state = MENU_NONE and
             * redraws the main screen — don't stomp on that afterward. */
            shutdown_inverter();
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
                post_buzzer_event(false);

                vTaskDelay(pdMS_TO_TICKS(2000));
                go_to_main_screen();
            }
            break;
        default:
            break;
        }
        break;
    }

    case BUTTON_EVENT_DOUBLE_CLICK:
    {
        ESP_LOGI("POWER BUTTON", "Button clicked twice");
        if (sys_state.value_edit_mode)
            break;
        if (sys_state.inverter.inverter_state == INVERTER_ON ||
            sys_state.inverter.inverter_state == INVERTER_STARTING)
        {
            /* was 10000ms — every other flash in this file is ~1500ms */
            lcd_flash_info("Stop inverter   ", "before diag!    ", 1500);
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
        ESP_LOGI("POWER BUTTON", "Clicked three times");
        if (sys_state.inverter.inverter_state == INVERTER_ON ||
            sys_state.inverter.inverter_state == INVERTER_STARTING)
        {
            lcd_flash_info("Stop inverter   ", "before reset!   ", 1500);
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
        sys_state.menu_state = MENU_FACTORY_RESET;
        sys_state.menu_selection = 0;
        sys_state.power_button_sequence_count = 1;
        sys_state.power_sequence_start_time = current_time;
        lcd_show_confirm("FACTORY RESET?  ", "Hold=Yes Back=No");
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
    case MENU_FACTORY_RESET:
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

// ENTER/MENU BUTTON - Navigate menus and enter/confirm values
/* ── handle_enter_menu_button_event() ──────────────────────────────────── */

void handle_enter_menu_button_event(button_event_info_t *event_info,
                                    void *user_data)
{

    if (!sys_state.system_ready)
        return;

    if (event_info->event == BUTTON_EVENT_PRESS)
    {
        post_button_click_event();
    }

    if (atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_PHASE_PROGRESS)
    {
        return;
    }

    /* ── Factory reset PIN gate (intercept before everything else) ── */
    if (sys_state.menu_state == MENU_FACTORY_RESET &&
        atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_RESET_PIN_ENTRY)
    {
        if (event_info->event == BUTTON_EVENT_CLICK)
        {
            factory_reset_handle_pin_entry(&sys_state.factory_reset, BTN_ENTER);
        }
        return;
    }

    if (sys_state.menu_state == MENU_SECURITY)
    {
        security_phase_t phase = atomic_load(&sys_lcd.security.phase);

        if (phase == SECURITY_PHASE_VIEW_STATUS)
        {
            atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_IDLE);
            atomic_store(&sys_lcd.security.action, SECURITY_ACTION_NONE);
            return;
        }

        if (phase == SECURITY_PHASE_PIN_FLOW)
        {
            if (event_info->event != BUTTON_EVENT_CLICK)
            {
                return;
            }

            bool flow_done = false;

            xSemaphoreTake(change_pin_mutex, portMAX_DELAY);
            switch (atomic_load(&sys_lcd.security.action))
            {
            case SECURITY_ACTION_CHANGE_PIN:
            case SECURITY_ACTION_RESET_PIN:
                flow_done = change_pin_handle_button(&change_pin_ctx, BTN_ENTER);
                break;
            default:
                flow_done = true;
                break;
            }
            xSemaphoreGive(change_pin_mutex);

            if (flow_done)
            {
                atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_IDLE);
                atomic_store(&sys_lcd.security.action, SECURITY_ACTION_NONE);
                sys_lcd.screen = LCD_SCREEN_SECURITY;
            }
            return;
        }
    }

    switch (event_info->event)
    {

    case BUTTON_EVENT_CLICK:
        /* Value edit save */

        if (sys_state.value_edit_mode)
        {
            if (sys_state.pending_confirmation)
            {
                handle_value_confirmation();
            }
            else
            {
                exit_value_edit_mode(true);
            }
            break;
        }

        if (sys_state.menu_state == MENU_FACTORY_RESET &&
            atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_PHASE_CONFIRM)
        {
            const char *done_msg = "                ";
            switch (sys_state.factory_reset.action)
            {
            case FACTORY_ACTION_RESET_ALL:
                ESP_LOGI(TAG_SYS, "Factory reset confirmed by user");
                atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_PROGRESS);
                atomic_store(&sys_lcd.factory_reset.progress_pct, 0);
                perform_factory_reset();
                atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_DONE);
                break;

            case FACTORY_ACTION_CLEAR_SETTINGS:
                ESP_LOGI(TAG_SYS, "Clear Settings confirmed by user");
                atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_PROGRESS);
                atomic_store(&sys_lcd.factory_reset.progress_pct, 0);
                clear_settings();
                reload_default_settings();
                save_settings();
                atomic_store(&sys_lcd.factory_reset.progress_pct, 100);
                post_buzzer_event(true);
                sys_state.power_button_sequence_count = 0;
                atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_DONE);
                done_msg = "Settings Cleared";
                break;

            case FACTORY_ACTION_ERASE_LOGS:
                ESP_LOGI(TAG_SYS, "Erase Logs confirmed by user");
                atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_PROGRESS);
                atomic_store(&sys_lcd.factory_reset.progress_pct, 0);
                erase_logs();
                atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_DONE);
                done_msg = "Logs Erased     ";
                break;

            default:
                break;
            }

            atomic_store(&sys_lcd.factory_reset.action, FACTORY_ACTION_NONE);
            atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_IDLE);
            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
            clear_menu_history();
            go_to_main_screen();
            lcd_flash_info("Done            ", done_msg, 1500);
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
                next = MENU_FACTORY_RESET;
                break;
            case 5:
                next = MENU_SECURITY;
                break;
            }
            if (next != MENU_NONE)
            {
                push_menu_history(sys_state.menu_state, sys_state.menu_selection);
                sys_state.menu_state = next;
                sys_state.menu_selection = 0;
                show_menu_screen(next, sys_state.menu_selection);
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
            case 5:
                edit_auto_shutdown();
                break;
            case 6:
                edit_scroll_enable();
                break;
            case 7:
                edit_scroll_speed();
                break;
            case 8:
                edit_battery_type();
                break;
            case 9:
                edit_battery_voltage_system();
                break;
            case 10:
                edit_sound_enable();
                break;
            }
            break;

        case MENU_MONITORING:

            if (sys_state.menu_selection < 6)
            {
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
                }
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
            case 1:
                lcd_show_wifi_connecting("Scanning...");
                start_wifi_scan();
                lcd_show_wifi_scan_screen();
                break;
            case 2:
                start_wifi_connection();
                break;
            default:
                show_menu_screen(MENU_WIFI_CONFIG, sys_state.menu_selection);
                break;
            }
            break;

        case MENU_FACTORY_RESET:
            if (atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_PHASE_DONE)
            {
                sys_lcd.screen = LCD_SCREEN_MAIN;
                atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_IDLE);
                break;
            }
            else if (atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_PHASE_IDLE)
            {
                sys_lcd.screen = LCD_SCREEN_FACTORY_RESET;
            }

            switch (sys_state.menu_selection)
            {
            case 0:
                factory_reset_begin(&sys_lcd.factory_reset, FACTORY_ACTION_RESET_ALL);
                break;
            case 1:
                factory_reset_begin(&sys_lcd.factory_reset, FACTORY_ACTION_CLEAR_SETTINGS);
                break;
            case 2:
                factory_reset_begin(&sys_lcd.factory_reset, FACTORY_ACTION_ERASE_LOGS);
                break;
            }
            pin_entry_reset(&sys_lcd.factory_reset.pin_ctx);
            atomic_store(&sys_lcd.factory_reset.phase, FACTORY_RESET_PIN_ENTRY);
            sys_lcd.screen = LCD_SCREEN_FACTORY_RESET;
            break;

        case MENU_SECURITY:
        {

            switch (sys_state.menu_selection)
            {
            case 0: /* Change PIN */
            {
                xSemaphoreTake(change_pin_mutex, portMAX_DELAY);
                change_pin_start_ex(&change_pin_ctx, CHANGE_PIN_MODE_SET_NEW);
                xSemaphoreGive(change_pin_mutex);

                atomic_store(&sys_lcd.security.action, SECURITY_ACTION_CHANGE_PIN);
                atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_PIN_FLOW);

                /* Force-change flag: if PIN is still factory default, flash a
                 * reminder so the user knows why they're being prompted. */
                if (security_pin_change_required())
                {
                    lcd_flash_info("Set your PIN    ", "Default is 0000 ", 1500);
                }
                break;
            }

            case 1: /* View PIN status */
            {
                atomic_store(&sys_lcd.security.action, SECURITY_ACTION_VIEW_STATUS);
                atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_VIEW_STATUS);
                break;
            }

            case 2: /* Reset PIN to factory default (0000) */
            {
                xSemaphoreTake(change_pin_mutex, portMAX_DELAY);
                change_pin_start_ex(&change_pin_ctx, CHANGE_PIN_MODE_RESET_DEFAULT);
                xSemaphoreGive(change_pin_mutex);

                atomic_store(&sys_lcd.security.action, SECURITY_ACTION_RESET_PIN);
                atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_PIN_FLOW);
                break;
            }

            default:
                break;
            }
            break; /* end case MENU_SECURITY */
        }
        default:
            break;
        }
        break;
    case BUTTON_EVENT_LONG_PRESS:
        switch (sys_state.menu_state)
        {
        case MENU_NONE:
            /* Always opens the Main Menu, regardless of inverter state. */
            sys_state.menu_state = MAIN_MENU;
            sys_state.menu_selection = 0;
            show_menu_screen(MAIN_MENU, 0);
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
                sys_lcd.screen = LCD_SCREEN_MONITORING_DETAIL;
                break;
            case 2:
                next = MENU_DIAGNOSTIC;
                sys_lcd.screen = LCD_SCREEN_DIAGNOSTIC;
                break;
            case 3:
                next = MENU_FACTORY_RESET;
                sys_lcd.screen = LCD_SCREEN_FACTORY_RESET;
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

        case MENU_MONITORING:
            if (sys_state.menu_selection < 6)
            {
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
                }
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
            case 1:
                lcd_show_wifi_connecting("Scanning...");
                start_wifi_scan();
                lcd_show_wifi_scan_screen();
                break;
            case 2:
                start_wifi_connection();
                break;
            default:
                break;
            }
            break;
        case MENU_FACTORY_RESET:
            atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_CONFIRM);
            lcd_show_factory_reset_screen();
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
    } // switch(event_info->event)
    ESP_LOGI("CONFIRM", "End  of program");
    sys_state.last_activity_time = event_info->timestamp_us / 1000;
}

/* ── acceleration tiers ──────────────────────────────────────────────────
 * Returns which increase_value() flags to use based on how long
 * the button has been continuously held.
 */
static void get_accel_step(int64_t hold_duration_ms, bool *fast, bool *big)
{
    if (hold_duration_ms < 800)
    {
        *fast = false;
        *big = false; /* normal speed */
    }
    else if (hold_duration_ms < 2500)
    {
        *fast = true;
        *big = false; /* ramping up   */
    }
    else
    {
        *fast = true;
        *big = true; /* max speed    */
    }
}

// Up button event handler - Advanced value increase
/* ── handle_up_button_event() ──────────────────────────────────────────── */
void handle_up_button_event(button_event_info_t *event_info,
                            void *user_data)
{
    if (!sys_state.system_ready)
        return;

    if (event_info->event == BUTTON_EVENT_PRESS)
    {
        post_button_click_event();
    }
    int64_t current_time = event_info->timestamp_us / 1000;
    value_edit_context_t *config = get_current_value_config();

    // handle_up_button_event():
    if (sys_state.menu_state == MENU_FACTORY_RESET &&
        atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_RESET_PIN_ENTRY)
    {
        if (event_info->event == BUTTON_EVENT_CLICK)
        {
            factory_reset_handle_pin_entry(&sys_state.factory_reset, BTN_UP);
        }
        return;
    }

    // When in security mode
    if (sys_state.menu_state == MENU_SECURITY &&
        atomic_load(&sys_lcd.security.phase) == SECURITY_PHASE_PIN_FLOW)
    {
        if (event_info->event == BUTTON_EVENT_CLICK)
        {
            xSemaphoreTake(change_pin_mutex, portMAX_DELAY);
            change_pin_handle_button(&change_pin_ctx, BTN_UP);
            xSemaphoreGive(change_pin_mutex);
        }
        return; // Up just adjusts the current PIN digit -- never finishes the flow
    }
    switch (event_info->event)
    {
    case BUTTON_EVENT_CLICK:
        // Reset flashed info if any
        if (sys_state.value_edit_mode)
        {
            switch (config->edit_type)
            {
            case VALUE_EDIT_NUMERIC:
                ESP_LOGI("VALUE EDIT", "Increasing numeric value");
                increase_value(false, false);
                break;
            case VALUE_EDIT_SELECT:
                if (config->max_selection > 0)
                    config->selection_index =
                        (config->selection_index + 1) % config->max_selection;
                break;
            case VALUE_EDIT_BOOL:
                config->current_value = (config->current_value != 0.0f) ? 0.0f : 1.0f;
                break;
            case VALUE_EDIT_LIST:
                config->list_index =
                    (config->list_index + 1) % config->list_size;
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
        else if (sys_state.menu_state == MENU_FACTORY_RESET && !sys_state.in_detail_view)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(MENU_FACTORY_RESET, &n);
            if (!items || n == 0)
                break;
            sys_state.menu_selection =
                (sys_state.menu_selection == 0) ? n - 1
                                                : sys_state.menu_selection - 1;
            sys_state.detail_parent_selection = sys_state.menu_selection;
            sys_state.detail_parent_menu = MENU_FACTORY_RESET;
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
            sys_state.hold_start_time = current_time;
            sys_state.repeat_count = 0;
            sys_state.fast_increment_active = false;
            increase_value(false, false);
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
            if (sys_state.hold_start_time == 0)
                sys_state.hold_start_time = current_time;

            int64_t held_ms = current_time - sys_state.hold_start_time;
            bool fast, big;
            get_accel_step(held_ms, &fast, &big);

            sys_state.repeat_count++;
            sys_state.fast_increment_active = fast;
            increase_value(fast, big);
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
        sys_state.hold_start_time = 0;
        break;

    default:
        break;
    }

    sys_state.last_activity_time = current_time;
    sys_state.last_increment_time = current_time;
}

// Down button event handler - Advanced value decrease
/* ── handle_down_button_event() ─────────────────────────────────────────── */
void handle_down_button_event(button_event_info_t *event_info,
                              void *user_data)
{
    if (!sys_state.system_ready)
        return;

    if (event_info->event == BUTTON_EVENT_PRESS)
    {
        post_button_click_event();
    }
    int64_t current_time = event_info->timestamp_us / 1000;
    value_edit_context_t *config = get_current_value_config();

    if (sys_state.menu_state == MENU_FACTORY_RESET &&
        atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_RESET_PIN_ENTRY)
    {
        if (event_info->event == BUTTON_EVENT_CLICK)
        {
            factory_reset_handle_pin_entry(&sys_state.factory_reset, BTN_DOWN);
        }
        return;
    }

    // When the user is in security mode
    if (sys_state.menu_state == MENU_SECURITY &&
        atomic_load(&sys_lcd.security.phase) == SECURITY_PHASE_PIN_FLOW)
    {
        if (event_info->event == BUTTON_EVENT_CLICK)
        {
            xSemaphoreTake(change_pin_mutex, portMAX_DELAY);
            change_pin_handle_button(&change_pin_ctx, BTN_DOWN);
            xSemaphoreGive(change_pin_mutex);
        }
        return;
    }

    switch (event_info->event)
    {
    case BUTTON_EVENT_CLICK:
        if (sys_state.value_edit_mode)
        {
            switch (config->edit_type)
            {
            case VALUE_EDIT_NUMERIC:
                decrease_value(false, false);
                break;
            case VALUE_EDIT_SELECT:
                if (config->max_selection > 0)
                    config->selection_index =
                        (config->selection_index > 0)
                            ? config->selection_index - 1
                            : config->max_selection - 1;
                break;
            case VALUE_EDIT_BOOL:
                config->current_value = (config->current_value != 0.0f) ? 0.0f : 1.0f;
                break;
            case VALUE_EDIT_LIST:
                config->list_index =
                    (config->list_index > 0)
                        ? config->list_index - 1
                        : config->list_size - 1;
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
        else if (sys_state.menu_state == MENU_FACTORY_RESET && !sys_state.in_detail_view)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(MENU_FACTORY_RESET, &n);
            if (!items || n == 0)
                break;
            sys_state.menu_selection =
                (sys_state.menu_selection + 1) % n;
            sys_state.detail_parent_selection = sys_state.menu_selection;
            sys_state.detail_parent_menu = MENU_FACTORY_RESET;
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
            sys_state.hold_start_time = current_time;
            sys_state.repeat_count = 0;
            sys_state.fast_increment_active = false;
            decrease_value(false, false);
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
            if (sys_state.hold_start_time == 0)
                sys_state.hold_start_time = current_time;

            int64_t held_ms = current_time - sys_state.hold_start_time;
            bool fast, big;
            get_accel_step(held_ms, &fast, &big);

            sys_state.repeat_count++;
            sys_state.fast_increment_active = fast;
            decrease_value(fast, big);
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
        sys_state.hold_start_time = 0;
        break;

    default:
        break;
    }

    sys_state.last_activity_time = current_time;
    sys_state.last_increment_time = current_time;
}

/* ── handle_back_button_event() ─────────────────────────────────────────── */
void handle_back_button_event(button_event_info_t *event_info,
                              void *user_data)
{
    if (!sys_state.system_ready)
        return;

    if (event_info->event == BUTTON_EVENT_PRESS)
    {
        post_button_click_event();
    }

    // User is in Security mode

    if (sys_state.menu_state == MENU_FACTORY_RESET)
    {
        factory_reset_phase_t phase =
            atomic_load(&sys_lcd.factory_reset.phase);

        if (phase == FACTORY_RESET_PIN_ENTRY)
        {
            if (event_info->event == BUTTON_EVENT_CLICK)
            {
                factory_reset_handle_pin_entry(&sys_state.factory_reset, BTN_BACK);
                show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
                sys_lcd.screen = LCD_SCREEN_MENU;
            }
            return;
        }
    }

    if (sys_state.menu_state == MENU_SECURITY)
    {

        if (atomic_load(&sys_lcd.security.phase) == SECURITY_PHASE_PIN_FLOW)

        {
            if (event_info->event != BUTTON_EVENT_CLICK)
            {
                return;
            }

            xSemaphoreTake(change_pin_mutex, portMAX_DELAY);
            bool flow_done = change_pin_handle_button(&change_pin_ctx, BTN_BACK);
            xSemaphoreGive(change_pin_mutex);

            if (flow_done)
            {
                ESP_LOGI("DISPLAY MODE", "DISPLAYING MODE");
                atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_IDLE);
                atomic_store(&sys_lcd.security.action, SECURITY_ACTION_NONE);
                sys_lcd.screen = LCD_SCREEN_SECURITY;
            }
            return;
        }

        if (
            atomic_load(&sys_lcd.security.phase) == SECURITY_PHASE_VIEW_STATUS)
        {
            atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_IDLE);
            atomic_store(&sys_lcd.security.action, SECURITY_ACTION_NONE);
            return;
        }
    }

    switch (event_info->event)
    {
    case BUTTON_EVENT_CLICK:
        /* P1 cancel factory reset */
        ESP_LOGI("BACK BUT", "rEACHED HERE FOR BACK");
        if (sys_state.menu_state == MENU_FACTORY_RESET)
        {
            ESP_LOGI("FACTORY_RESET", "We are at factory reset screen");
            factory_reset_phase_t phase = atomic_load(&sys_lcd.factory_reset.phase);

            if (phase == FACTORY_PHASE_PROGRESS)
            {
                return;
            }

            if (phase == FACTORY_PHASE_CONFIRM)
            {
                atomic_store(&sys_lcd.factory_reset.action, FACTORY_ACTION_NONE);
                atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_IDLE);
                sys_state.menu_state = MENU_NONE;
                sys_state.menu_selection = 0;
                clear_menu_history();
                go_to_main_screen();
                return;
            }

            /* phase == FACTORY_PHASE_IDLE: exit the list back to the main menu */
            atomic_store(&sys_lcd.factory_reset.action, FACTORY_ACTION_NONE);
            atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_IDLE);
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            sys_state.menu_selection = 0;
            sys_state.menu_state = MAIN_MENU;
            show_menu_screen(MAIN_MENU, sys_state.menu_selection);
            return;
        }

        else if (atomic_load(&sys_lcd.factory_reset.action) == FACTORY_ACTION_NONE && sys_state.menu_state == MAIN_MENU)
        {
            sys_lcd.screen = LCD_SCREEN_MAIN;
        }

        /* P2 cancel value edit */
        if (sys_state.value_edit_mode)
        {
            exit_value_edit_mode(false);
            lcd_show_value_canceled_screen();
            sys_state.menu_state = MAIN_MENU;
            sys_state.menu_selection = 0;
            sys_state.pending_confirmation = false;
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            return;
        }
        /* P3 exit detail view */
        if (sys_state.in_detail_view)
        {
            sys_state.in_detail_view = false;
            sys_state.inverter.inverter_state = sys_state.pre_detail_inverter_state;
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            return;
        }
        /* P4 exit confirm */
        if (sys_state.in_confirmation_screen)
        {
            sys_state.in_confirmation_screen = false;
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            return;
        }
        /* P5 exit info screen */
        if (sys_state.in_info_screen)
        {
            sys_state.in_info_screen = false;
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            go_to_main_screen();
            return;
        }

        /* P6 pop history */
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
        /* P7 fallback */
        switch (sys_state.menu_state)
        {
        case MENU_FACTORY_RESET:
        case MENU_SETTINGS:
        case MENU_MONITORING:
        case MENU_DIAGNOSTIC:
        case MENU_WIFI_CONFIG:
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

    default:
        break;
    }
}

// Helper functions for menu history management
static void push_menu_history(menu_state_t state, uint8_t selection)
{
    if (menu_history.depth < MAX_MENU_HISTORY)
    {
        menu_history.stack[menu_history.depth].state = state;
        menu_history.stack[menu_history.depth].selection = selection;
        menu_history.depth++;
        ets_printf("Menu history pushed: state=%d, selection=%d, depth=%d\n",
                   state, selection, menu_history.depth);
    }
    else
    {
        ets_printf("Menu history stack full!\n");
    }
}

static bool pop_menu_history(menu_state_t *state, int *selection)
{
    if (menu_history.depth > 0)
    {
        menu_history.depth--;
        *state = menu_history.stack[menu_history.depth].state;
        *selection = menu_history.stack[menu_history.depth].selection;
        ets_printf("Menu history popped: state=%d, selection=%d, depth=%d\n",
                   *state, *selection, menu_history.depth);
        return true;
    }
    ets_printf("Menu history empty\n");
    return false;
}

static void clear_menu_history(void)
{
    menu_history.depth = 0;
    ets_printf("Menu history cleared\n");
}

/* ── inverter_power_on() ────────────────────────────────────────────────── */
static const char *INV_TAG = "INVERTER";

static void post_button_click_event(void)
{
    system_event_t evt = {0};
    evt.category = EVENT_CATEGORY_BUTTON;
    evt.action = EVENT_ACTION_PRESSED;
    evt.source = EVENT_SOURCE_BUTTON;
    evt.priority = EVENT_PRIORITY_LOW;
    evt.timestamp = xTaskGetTickCount();
    system_event_post(&evt);
}

static void post_inverter_power_event(bool powered_on)
{
    system_event_t evt = {0};
    evt.category = EVENT_CATEGORY_SYSTEM;
    evt.action = powered_on ? EVENT_ACTION_ON : EVENT_ACTION_OFF;
    evt.source = EVENT_SOURCE_SYSTEM;
    evt.priority = EVENT_PRIORITY_NORMAL;
    evt.timestamp = xTaskGetTickCount();
    system_event_post(&evt);
}

static void post_inverter_fault_event(void)
{
    system_event_t evt = {0};
    evt.category = EVENT_CATEGORY_SYSTEM;
    evt.action = EVENT_ACTION_ERROR;
    evt.source = EVENT_SOURCE_SYSTEM;
    evt.priority = EVENT_PRIORITY_CRITICAL;
    evt.timestamp = xTaskGetTickCount();
    system_event_post(&evt);
}

static void post_inverter_success_event(void)
{
    system_event_t evt = {0};
    evt.category = EVENT_CATEGORY_SYSTEM;
    evt.action = EVENT_ACTION_SUCCESS;
    evt.source = EVENT_SOURCE_SYSTEM;
    evt.priority = EVENT_PRIORITY_NORMAL;
    evt.timestamp = xTaskGetTickCount();
    system_event_post(&evt);
}

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
    post_inverter_power_event(true);
    ESP_LOGI(INV_TAG, "Inverter powered on");
}

/* ── shutdown_inverter() ────────────────────────────────────────────────── */
void shutdown_inverter(void)
{
    if (sys_state.inverter.inverter_state == INVERTER_OFF)
    {
        ESP_LOGI(INV_TAG, "shutdown_inverter() called but inverter already off, ignoring");
        return;
    }

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
        post_inverter_fault_event();
        ESP_LOGE(INV_TAG, "Relay open failed: %s", esp_err_to_name(err));
        lcd_show_fault("** RELAY FAULT  ", "Restarting...   ");
        vTaskDelay(pdMS_TO_TICKS(1500));
        perform_system_restart(false);
        return;
    }

    post_inverter_power_event(false);
    sys_state.inverter.inverter_state = INVERTER_OFF;
    sys_state.inverter.inverter_active = false;
    sys_state.menu_state = MENU_NONE;
    sys_state.error.error_flags &= ~SYSTEM_FAILURE_ERROR;
    go_to_main_screen();
    ESP_LOGI(INV_TAG, "Inverter powered off");
}

// Helper functions to apply settings
static void apply_voltage_threshold(float v) { inverter_set_output_voltage(v); }
static void apply_frequency(float v) { inverter_set_output_frequency(v); }
static void apply_current_limit(float v) { inverter_set_current_limit(v); }
static void apply_temperature_limit(float v) { thermal_protection_set_limit(v); }
static void apply_battery_cutoff(float v) { battery_monitor_set_cutoff(v); }
static void apply_wifi(float v) { sys_state.wifi.enabled = (bool)v; }
static void apply_bluetooth(float v) { sys_state.bluetooth.enabled = (bool)v; } /* adjust to your actual field */
static void apply_auto_shutdown(float v) { sys_state.display.auto_shutdown_enabled = (uint8_t)v; }
static void apply_scroll_enable(float v) { sys_state.display.scroll_enabled = (uint8_t)v; }

static void apply_sound_enable(float v)
{
    sys_state.sound_enabled = (v != 0.0f);
    if (!sys_state.sound_enabled)
    {
        buzzer_off();
    }
}
static void apply_scroll_speed(float v) { sys_state.display.scroll_speed = (uint8_t)v; }
static void apply_system_timeout(float v) { set_system_timeout((uint32_t)v); }

/* Battery type change regenerates the active profile at the currently
 * configured voltage/capacity — it must not just overwrite profile_id
 * and leave every derived voltage/current field stale. */
/* Derive protection.c's graduated BATTERY_VOLTAGE thresholds from the
 * active battery profile, which is already scaled for the currently
 * configured chemistry and voltage system (12/24/48/96V). Without this,
 * protection.c's own load_defaults() leaves BATTERY_VOLTAGE pinned at
 * its hardcoded 24V-class numbers (22.5/21.5/20.5V) regardless of what
 * the inverter is actually configured for. Call this any time
 * sys_state.battery_profile changes: after battery_load_profile() at
 * boot, and after apply_battery_type()/apply_battery_voltage_system()
 * regenerate the profile at runtime. */
static void sync_battery_protection_thresholds(void)
{
    const battery_profile_t *p = &sys_state.battery_profile;
    protection_thresholds_t t = {
        .warning_high = p->high_battery_voltage_12v,
        .derate_high = p->high_battery_voltage_12v,
        .fault_high = p->overvoltage_protection_12v,
        .hysteresis_high = 0.5f,
        .warning_low = p->low_voltage_warning_12v,
        .derate_low = p->low_voltage_alarm_12v,
        .fault_low = p->cutoff_voltage_12v,
        .hysteresis_low = 0.5f,
        .has_low_bound = true,
    };
    protection_set_thresholds(PROT_QUANTITY_BATTERY_VOLTAGE, &t);
}

static void apply_battery_type(float v)
{
    battery_type_t new_type = (battery_type_t)v;
    battery_profile_t regenerated;
    if (battery_generate_profile(new_type,
                                 (voltage_system_t)sys_state.battery_profile.nominal_voltage,
                                 sys_state.battery_profile.capacity_ah,
                                 &regenerated))
    {
        sys_state.battery_profile = regenerated;
        sys_state.battery_profile.profile_id = new_type;
        sync_battery_protection_thresholds();
    }
}

/* Voltage system change regenerates the active profile at the currently
 * configured battery type/capacity, scaled to the new nominal voltage.
 * 'v' is the select's 0..N-1 index, not the raw voltage -- must go
 * through voltage_system_values[] rather than being cast directly. */
static void apply_battery_voltage_system(float v)
{
    int index = (int)v;
    if (index < 0 || index >= BATTERY_VOLTAGE_SYSTEM_OPTION_COUNT)
        return;

    voltage_system_t new_voltage = voltage_system_values[index];
    battery_profile_t regenerated;
    if (battery_generate_profile((battery_type_t)sys_state.battery_profile.profile_id,
                                 new_voltage,
                                 sys_state.battery_profile.capacity_ah,
                                 &regenerated))
    {
        sys_state.battery_profile = regenerated;
        sys_state.battery_profile.nominal_voltage = new_voltage;
        sync_battery_protection_thresholds();
    }
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
    lcd_flash_info("Entering Diag.  ", "Please wait.    ", 500);
    vTaskDelay(pdMS_TO_TICKS(500));
    lcd_flash_info("Entering Diag.  ", "Please wait..   ", 500);
    vTaskDelay(pdMS_TO_TICKS(500));
    lcd_flash_info("Entering Diag.  ", "Please wait...  ", 500);
    vTaskDelay(pdMS_TO_TICKS(500));

    show_menu_screen(MENU_DIAGNOSTIC, 0);
}

/* ── exit_diagnostic_mode() ─────────────────────────────────────────────── */
void exit_diagnostic_mode(void)
{
    lcd_flash_info(" Exiting Diag.. ", "                ", 1500);
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
    case 0: /* System Status */
    {
        /*
         * Row 1 layout (16 chars):
         *   "OK  HB:12345    "   <- system ok,  heartbeat count
         *   "FLT HB:12345   !"   <- fault,      heartbeat count
         */
        uint32_t hb = lcd_watchdog_get_heartbeat();
        uint32_t last_ms = lcd_watchdog_last_feed_ms();
        uint32_t now_ms_val = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t age_ms = (last_ms > 0) ? (now_ms_val - last_ms) : 0;

        bool lcd_alive = (age_ms < LCD_HEARTBEAT_TIMEOUT_MS);

        snprintf(row1, 17, "%s HB:%-6lu%s",
                 (diag_data.system_ok && lcd_alive) ? "OK " : "FLT",
                 (unsigned long)(hb % 999999),
                 lcd_alive ? " " : "!");
        break;
    }

    case 1: /* Latest Error */
    {
        const error_log_entry_t *latest = error_log_get_latest();
        if (!latest)
            snprintf(row1, 17, "%-16s", "No errors logged");
        else
            snprintf(row1, 17, "%-16.16s", latest->description);
        break;
    }

    case 2: /* CPU Load */
        snprintf(row1, 17, "Load:%6.1f%%    ", diag_data.cpu_load);
        break;

    case 3: /* Firmware Version */
        snprintf(row1, 17, "%-16.16s", "C-01 Rev A");
        break;

    case 4: /* Uptime */
    {
        unsigned long s = (unsigned long)diag_data.uptime_seconds;
        unsigned long d = s / 86400UL;
        unsigned long h = (s % 86400UL) / 3600UL;
        unsigned long m = (s % 3600UL) / 60UL;
        unsigned long sec = s % 60UL;

        if (d > 0)
            snprintf(row1, 17, "%lud %02lu:%02lu:%02lu ", d, h, m, sec);
        else
            snprintf(row1, 17, "   %02lu:%02lu:%02lu    ", h, m, sec);
        break;
    }

    case 5: /* RAM usage */
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

    lcd_show_factory_progress();

    /* Reset runtime values */
    sys_state.inverter.output_voltage = 220.0f;
    sys_state.inverter.output_frequency = 50.0f;
    sys_state.current_limit = 20.0f;
    sys_state.temperature_limit = 70.0f;
    sys_state.cutoff_voltage = 11.5f;
    sys_state.display.scroll_speed = DEFAULT_SCROLL_SPEED;

    error_log_clear();
    calibration_reset();

    post_buzzer_event(true);
    post_led_event(true);
    sys_state.menu_state = MENU_NONE;
    sys_state.power_button_sequence_count = 0;

    lcd_show_factory_done();
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Perform NVS erase, reinitialize, update progress and restart */
    factory_reset();
}

bool check_safety_conditions(void)
{
    battery_profile_t battery;
    float measured_voltage = 12.0f; // Simulated measurements
    float measured_current = 10.2f;
    float measured_temp = 3.0f;
    sys_state.insulation_resistance = 500.0f; // Simulated insulation resistance in kOhms

    // Load battery profile
    if (!battery_load_profile(&battery))
    {
        printf("Failed to load battery profile!\n");
        return false;
    }

    printf("Battery: %s\n", battery.name_prefix);
    printf("Measured: %.2fV, %.2fA, %.1f°C\n\n",
           measured_voltage, measured_current, measured_temp);

    bool all_checks_passed = true;

    // Check overvoltage
    if (measured_voltage > battery.overvoltage_protection_12v)
    {
        printf("❌ OVERVOLTAGE! %.2fV > %.2fV\n",
               measured_voltage, battery.overvoltage_protection_12v);
        all_checks_passed = false;
    }
    else
    {
        printf("✓ Voltage OK (%.2fV <= %.2fV)\n",
               measured_voltage, battery.overvoltage_protection_12v);
    }

    // Check undervoltage
    if (measured_voltage < battery.undervoltage_protection_12v)
    {
        printf("❌ UNDERVOLTAGE! %.2fV < %.2fV\n",
               measured_voltage, battery.undervoltage_protection_12v);
        all_checks_passed = false;
    }
    else
    {
        printf("✓ Voltage above minimum (%.2fV >= %.2fV)\n",
               measured_voltage, battery.undervoltage_protection_12v);
    }

    // Check overcurrent
    if (measured_current > battery.max_charge_current_per_100ah)
    {
        printf("❌ OVERCURRENT! %.2fA > %.2fA\n",
               measured_current, battery.max_charge_current_per_100ah);
        all_checks_passed = false;
    }
    else
    {
        printf("✓ Current OK (%.2fA <= %.2fA)\n",
               measured_current, battery.max_charge_current_per_100ah);
    }

    // Check temperature
    if (measured_temp < battery.charge_temp_min ||
        measured_temp > battery.charge_temp_max)
    {
        printf("❌ TEMPERATURE OUT OF RANGE! %.1f°C (range: %.1f°C to %.1f°C)\n",
               measured_temp, battery.charge_temp_min, battery.charge_temp_max);
        all_checks_passed = false;
    }
    else
    {
        printf("✓ Temperature OK (%.1f°C within %.1f°C to %.1f°C)\n",
               measured_temp, battery.charge_temp_min, battery.charge_temp_max);
    }

    // Check low battery warning
    if (measured_voltage < battery.low_voltage_warning_12v)
    {
        printf("⚠️  LOW BATTERY WARNING! %.2fV < %.2fV\n",
               measured_voltage, battery.low_voltage_warning_12v);
    }

// Check grid voltage (if grid-tied or hybrid)
#ifdef GRID_TIED_MODE
    if (sys_state.grid_voltage < GRID_VOLTAGE_MIN ||
        sys_state.grid_voltage > GRID_VOLTAGE_MAX)
    {
        printf("ERROR: Grid voltage out of range: %.2fV\n", sys_state.grid_voltage);
        return false;
    }

    // Check grid frequency
    if (sys_state.grid_frequency < GRID_FREQ_MIN ||
        sys_state.grid_frequency > GRID_FREQ_MAX)
    {
        printf("ERROR: Grid frequency out of range: %.2fHz\n", sys_state.grid_frequency);
        return false;
    }
#endif

    // ============ CURRENT CHECKS ============

    // Check DC input current
    if (sys_state.dc_input_current > DC_CURRENT_MAX)
    {
        printf("ERROR: DC input current too high: %.2fA (max: %.2fA)\n",
               sys_state.dc_input_current, DC_CURRENT_MAX);
        return false;
    }

    // Check AC output current
    if (sys_state.ac_output_current > AC_CURRENT_MAX)
    {
        printf("ERROR: AC output current exceeds limit: %.2fA (max: %.2fA)\n",
               sys_state.ac_output_current, AC_CURRENT_MAX);
        return false;
    }

    // Check for overcurrent condition
    if (sys_state.fault_flags & FAULT_OVERCURRENT)
    {
        printf("ERROR: Overcurrent protection triggered!\n");
        return false;
    }

    // ============ TEMPERATURE CHECKS ============

    // Check heatsink/MOSFET temperature
    if (sys_state.heatsink_temperature > HEATSINK_TEMP_MAX)
    {
        printf("ERROR: Heatsink temperature too high: %.1f°C (max: %.1f°C)\n",
               sys_state.heatsink_temperature, HEATSINK_TEMP_MAX);
        return false;
    }

    // Check transformer temperature
    if (sys_state.transformer_temperature > TRANSFORMER_TEMP_MAX)
    {
        printf("ERROR: Transformer temperature too high: %.1f°C (max: %.1f°C)\n",
               sys_state.transformer_temperature, TRANSFORMER_TEMP_MAX);
        return false;
    }

    // Check ambient temperature
    if (sys_state.ambient_temperature > AMBIENT_TEMP_MAX ||
        sys_state.ambient_temperature < AMBIENT_TEMP_MIN)
    {
        printf("ERROR: Ambient temperature out of range: %.1f°C\n",
               sys_state.ambient_temperature);
        return false;
    }

    // Check if cooling fan is operational (if temperature requires it)
    if (sys_state.heatsink_temperature > FAN_START_TEMP && !sys_state.fan_running)
    {
        printf("WARNING: Fan should be running at this temperature!\n");
        // Depending on design, this could be a warning or error
    }

    // ============ PROTECTION CHECKS ============

    // Check for short circuit
    if (sys_state.fault_flags & FAULT_SHORT_CIRCUIT)
    {
        printf("ERROR: Short circuit detected!\n");
        return false;
    }

    // Check for ground fault (GFCI)
    if (sys_state.fault_flags & FAULT_GROUND_FAULT)
    {
        printf("ERROR: Ground fault detected!\n");
        return false;
    }

    // ============ COMPONENT STATUS CHECKS ============

    // Check gate drivers
    if (sys_state.fault_flags & FAULT_GATE_DRIVER)
    {
        printf("ERROR: Gate driver fault detected!\n");
        return false;
    }

    // Check DC bus capacitor voltage balance
    if (fabs(sys_state.dc_bus_positive - sys_state.dc_bus_negative) > DC_BUS_IMBALANCE_MAX)
    {
        printf("ERROR: DC bus voltage imbalance detected!\n");
        return false;
    }

    // Check pre-charge circuit (if applicable)
    if (sys_state.inverter.inverter_state == INVERTER_STANDBY && !sys_state.precharge_complete)
    {
        printf("ERROR: Pre-charge not complete!\n");
        return false;
    }

    // ============ LOAD CHECKS ============

    // Check if load is connected (optional, depends on design)
    if (sys_state.load_connected && sys_state.ac_output_current < 0.1f)
    {
        // This might indicate a problem with load sensing
        printf("WARNING: Load indicated but no current detected\n");
    }

    // ============ BATTERY CHECKS (for battery-backed systems) ============

#ifdef BATTERY_BACKED
    // Check battery state of charge
    if (sys_state.battery_soc < BATTERY_SOC_MIN)
    {
        printf("ERROR: Battery SOC too low: %.1f%% (min: %.1f%%)\n",
               sys_state.battery_soc, BATTERY_SOC_MIN);
        return false;
    }

    // Check battery health
    if (sys_state.battery_health < BATTERY_HEALTH_MIN)
    {
        printf("WARNING: Battery health degraded: %.1f%%\n", sys_state.battery_health);
        // Warning, not necessarily a blocker
    }

    // Check for reverse polarity
    if (sys_state.fault_flags & FAULT_REVERSE_POLARITY)
    {
        printf("ERROR: Battery reverse polarity detected!\n");
        return false;
    }
#endif

    // ============ TIMING AND SEQUENCING ============

    // Check minimum off-time before restart (prevents rapid cycling)
    float time_since_last_off = esp_timer_get_time() / 1000 - sys_state.last_power_off_time;
    if (time_since_last_off < MIN_OFF_TIME_MS)
    {
        printf("ERROR: Minimum off-time not met (%.0fms remaining)\n",
               MIN_OFF_TIME_MS - time_since_last_off);
        return false;
    }

    // Check emergency stop status
    if (sys_state.emergency_stop_active)
    {
        printf("ERROR: Emergency stop is active!\n");
        return false;
    }

// Check door/enclosure interlock (if applicable)
#ifdef ENCLOSURE_INTERLOCK
    if (!sys_state.enclosure_closed)
    {
        printf("ERROR: Enclosure door open!\n");
        return false;
    }
#endif

    // ============ FIRMWARE AND CALIBRATION ============

    // Check watchdog status
    if (sys_state.fault_flags & FAULT_WATCHDOG)
    {
        printf("ERROR: Watchdog timeout detected!\n");
        return false;
    }

    // ============ FINAL CONSOLIDATED CHECK ============

    // Check overall safety flag (if you're still using it)
    if (!all_checks_passed)
    {
        printf("ERROR: One or more safety checks failed!\n");
        return false;
    }

    // All checks passed
    printf("INFO: All safety checks passed. System ready for startup.\n");
    return true;
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

/*------------------------------------------------------------------------------
  DISPLAY SCREEN TYPES
------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------
  HELPER FUNCTIONS
------------------------------------------------------------------------------*/

static inline float clamp_float(float value, float min, float max)
{
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

typedef struct
{
    float display_voltage;
    float display_load;
    uint32_t startup_start_time;
    bool startup_complete;
    bool first_update;
} lcd_animation_state_t;

static lcd_animation_state_t lcd_anim = {
    .display_voltage = 0.0f,
    .display_load = 0.0f,
    .startup_start_time = 0,
    .startup_complete = false,
    .first_update = true};

/*------------------------------------------------------------------------------
  RESET ANIMATION (Call when system restarts)
------------------------------------------------------------------------------*/
void lcd_animation_reset(void)
{
    lcd_anim.display_voltage = 0.0f;
    lcd_anim.display_load = 0.0f;
    lcd_anim.startup_start_time = 0;
    lcd_anim.startup_complete = false;
    lcd_anim.first_update = true;
}

/* ── lcd_display_confirmation_screen() ──────────────────────────────────── */
void lcd_display_confirmation_screen(void)
{
    lcd_show_confirm("Save Changes?   ", "Enter=Yes Back=N");
}

// Advanced value adjustment implementation — handles all edit_type variants
void increase_value(bool fast_mode, bool precision_mode)
{
    if (!sys_state.value_edit_mode)
        return;
    value_edit_context_t *ctx = get_current_value_config();
    if (!ctx)
        return;

    switch (ctx->edit_type)
    {
    case VALUE_EDIT_NUMERIC:
    {
        float *current_value = get_current_value_pointer();
        if (!current_value)
            return;

        ESP_LOGI("Current-Value", "Current value: %.3f", *current_value);

        float increment = calculate_increment(fast_mode, precision_mode);

        // Apply acceleration based on repeat count
        if (sys_state.repeat_count > 10)
            increment *= 3;
        else if (sys_state.repeat_count > 5)
            increment *= 2;

        float new_value = *current_value + increment;

        ESP_LOGI("NEW_VALUE", "Attempting to increase value: %.3f -> %.3f (increment: %.3f)",
                 *current_value, new_value, increment);

        if (validate_value_range(new_value))
        {
            *current_value = new_value;
            sys_state.value_changed = true;

            if (ctx->live_update && !ctx->is_critical)
            {
                ESP_LOGI("NEW_VALUE", "Applying live update: %.3f", new_value);
                update_system_parameter(ctx, new_value);
            }
            if (ctx->is_critical)
                sys_state.pending_confirmation = true;

            ESP_LOGI("NEW_VALUE", "Value increased to: %.3f %s", new_value, ctx->unit);
        }
        else
        {
            ESP_LOGI("NEW_VALUE", "Value at maximum limit: %.3f %s", ctx->max_value, ctx->unit);
        }
        break;
    }

    case VALUE_EDIT_BOOL:
        ctx->current_value = (ctx->current_value != 0.0f) ? 0.0f : 1.0f;
        sys_state.value_changed = true;

        if (ctx->live_update && !ctx->is_critical)
            update_system_parameter(ctx, ctx->current_value);
        if (ctx->is_critical)
            sys_state.pending_confirmation = true;

        ESP_LOGI("NEW_VALUE", "%s toggled to: %s", ctx->label,
                 ctx->current_value != 0.0f ? "ON" : "OFF");
        break;

    case VALUE_EDIT_SELECT:
        if (ctx->max_selection > 0)
        {
            ctx->selection_index = (ctx->selection_index + 1) % ctx->max_selection;
            sys_state.value_changed = true;

            if (ctx->live_update && !ctx->is_critical)
                update_system_parameter(ctx, (float)ctx->selection_index);
            if (ctx->is_critical)
                sys_state.pending_confirmation = true;

            ESP_LOGI("NEW_VALUE", "%s selection: %d (%s)", ctx->label,
                     ctx->selection_index, ctx->options[ctx->selection_index]);
        }
        break;

    case VALUE_EDIT_LIST:
        if (ctx->list_size > 0)
        {
            ctx->list_index = (ctx->list_index + 1) % ctx->list_size;
            sys_state.value_changed = true;

            if (ctx->live_update && !ctx->is_critical)
                update_system_parameter(ctx, (float)ctx->list_index);
            if (ctx->is_critical)
                sys_state.pending_confirmation = true;

            ESP_LOGI("NEW_VALUE", "%s list index: %d", ctx->label, ctx->list_index);
        }
        break;

    default:
        break;
    }
}

void decrease_value(bool fast_mode, bool precision_mode)
{
    if (!sys_state.value_edit_mode)
        return;
    value_edit_context_t *ctx = get_current_value_config();
    if (!ctx)
        return;

    switch (ctx->edit_type)
    {
    case VALUE_EDIT_NUMERIC:
    {
        float *current_value = get_current_value_pointer();
        if (!current_value)
            return;

        float increment = calculate_increment(fast_mode, precision_mode);

        if (sys_state.repeat_count > 10)
            increment *= 3;
        else if (sys_state.repeat_count > 5)
            increment *= 2;

        float new_value = *current_value - increment;

        if (validate_value_range(new_value))
        {
            ESP_LOGI("NEW_VALUE", "Value change valid: %.3f -> %.3f", *current_value, new_value);
            *current_value = new_value;
            sys_state.value_changed = true;

            if (ctx->live_update && !ctx->is_critical)
            {
                ESP_LOGI("NEW_VALUE", "Applying live update: %.3f", new_value);
                update_system_parameter(ctx, new_value);
            }
            if (ctx->is_critical)
                sys_state.pending_confirmation = true;

            ESP_LOGI("NEW_VALUE", "Value decreased to: %.3f %s", new_value, ctx->unit);
        }
        else
        {
            ESP_LOGI("NEW_VALUE", "Value at minimum limit: %.3f %s", ctx->min_value, ctx->unit);
        }
        break;
    }

    case VALUE_EDIT_BOOL:
        /* Toggle is symmetric — Up and Down both flip it. */
        ctx->current_value = (ctx->current_value != 0.0f) ? 0.0f : 1.0f;
        sys_state.value_changed = true;

        if (ctx->live_update && !ctx->is_critical)
            update_system_parameter(ctx, ctx->current_value);
        if (ctx->is_critical)
            sys_state.pending_confirmation = true;

        ESP_LOGI("NEW_VALUE", "%s toggled to: %s", ctx->label,
                 ctx->current_value != 0.0f ? "ON" : "OFF");
        break;

    case VALUE_EDIT_SELECT:
        if (ctx->max_selection > 0)
        {
            ctx->selection_index = (ctx->selection_index > 0)
                                       ? ctx->selection_index - 1
                                       : ctx->max_selection - 1;
            sys_state.value_changed = true;

            if (ctx->live_update && !ctx->is_critical)
                update_system_parameter(ctx, (float)ctx->selection_index);
            if (ctx->is_critical)
                sys_state.pending_confirmation = true;

            ESP_LOGI("NEW_VALUE", "%s selection: %d (%s)", ctx->label,
                     ctx->selection_index, ctx->options[ctx->selection_index]);
        }
        break;

    case VALUE_EDIT_LIST:
        if (ctx->list_size > 0)
        {
            ctx->list_index = (ctx->list_index > 0)
                                  ? ctx->list_index - 1
                                  : ctx->list_size - 1;
            sys_state.value_changed = true;

            if (ctx->live_update && !ctx->is_critical)
                update_system_parameter(ctx, (float)ctx->list_index);
            if (ctx->is_critical)
                sys_state.pending_confirmation = true;

            ESP_LOGI("NEW_VALUE", "%s list index: %d", ctx->label, ctx->list_index);
        }
        break;

    default:
        break;
    }
}

void enter_value_edit_mode(value_edit_context_t *value_type)
{
    if (sys_state.value_edit_mode)
        return;

    sys_state.current_value_type = value_type;
    sys_state.value_edit_mode = true;
    sys_state.value_changed = false;
    sys_state.pending_confirmation = false;
    sys_state.repeat_count = 0;

    // Backup current value
    float *current_value = get_current_value_pointer();
    if (current_value)
    {
        sys_state.edit_backup_value = *current_value;
    }

    value_edit_context_t *ctx = get_current_value_config();
    printf("Entering edit mode for %s (%.3f %s)\n",
           ctx->label, *current_value, ctx->unit);
    printf("Use UP/DOWN to adjust, ENTER to save, BACK to cancel\n");
}

void exit_value_edit_mode(bool save_changes)
{
    if (!sys_state.value_edit_mode)
        return;

    value_edit_context_t *ctx = get_current_value_config();
    float *current_value = get_current_value_pointer();

    if (!ctx || !current_value)
    {
        /* Nothing valid to save/restore — just clear state and bail */
        sys_state.value_edit_mode = false;
        sys_state.current_value_type = NULL;
        sys_state.value_changed = false;
        sys_state.pending_confirmation = false;
        return;
    }

    if (save_changes && sys_state.value_changed)
    {
        apply_value_change();
        printf("Value saved successfully\n");
    }
    else if (!save_changes && sys_state.value_changed)
    {
        reset_value_to_backup();
        printf("Changes cancelled, value restored\n");
    }

    sys_state.value_edit_mode = false;
    sys_state.current_value_type = NULL;
    sys_state.value_changed = false;
    sys_state.pending_confirmation = false;
}

void apply_value_change(void)
{
    if (!sys_state.value_edit_mode || !sys_state.value_changed)
        return;

    value_edit_context_t *ctx = get_current_value_config();
    float *current_value = get_current_value_pointer();

    if (ctx && current_value)
    {
        update_system_parameter(ctx, *current_value);
        sys_state.pending_confirmation = false;
        printf("Applied %s: %.*f %s\n",
               ctx->label, ctx->decimal_places,
               *current_value, ctx->unit);
    }
}

void reset_value_to_backup(void)
{
    if (!sys_state.value_edit_mode)
        return;

    float *current_value = get_current_value_pointer();
    value_edit_context_t *ctx = get_current_value_config();

    if (current_value && ctx)
    {
        *current_value = sys_state.edit_backup_value;
        ctx->current_value = sys_state.edit_backup_value;
        sys_state.value_changed = false;
        sys_state.pending_confirmation = false;

        printf("Value reset to: %.*f %s\n",
               ctx->decimal_places, sys_state.edit_backup_value, ctx->unit);
    }
}

float *get_current_value_pointer(void)
{
    value_edit_context_t *ctx = get_current_value_config();
    if (ctx)
    {
        return &ctx->current_value;
    }
    return NULL;
}

value_edit_context_t *get_current_value_config(void)
{
    if (!sys_state.value_edit_mode || !sys_state.current_value_type)
        return NULL;
    if (sys_state.current_value_type->label == NULL)
        return NULL;
    return sys_state.current_value_type;
}

float calculate_increment(bool fast_mode, bool precision_mode)
{
    value_edit_context_t *ctx = get_current_value_config();
    if (!ctx)
        return 0.0f;

    if (precision_mode)
    {
        return ctx->increment_precision;
    }
    else if (fast_mode)
    {
        return ctx->increment_large;
    }
    else
    {
        return ctx->increment_small;
    }
}

bool validate_value_range(float new_value)
{
    value_edit_context_t *ctx = get_current_value_config();
    if (!ctx)
    {
        ESP_LOGI("VALIDATE", "Invalid value configuration");
        return false;
    }
    ESP_LOGI("VALIDATE", "Validating new value: %.3f (min: %.3f, max: %.3f)",
             new_value, ctx->min_value, ctx->max_value);
    return (new_value >= ctx->min_value && new_value <= ctx->max_value);
}

void handle_value_confirmation(void)
{
    if (!sys_state.value_edit_mode || !sys_state.pending_confirmation)
        return;

    value_edit_context_t *ctx = get_current_value_config();
    void *current_value_ptr = get_current_value_pointer();

    if (!ctx || !current_value_ptr)
    {
        printf("Error: Invalid value configuration\n");
        return;
    }

    int64_t current_time = esp_timer_get_time() / 1000; // ms
    if (current_time - sys_state.last_activity_time > VALUE_CONFIRM_TIMEOUT_MS)
    {
        printf("Confirmation timeout - reverting to original value\n");
        reset_value_to_backup();
        exit_value_edit_mode(false);
        return;
    }

    bool safety_check_passed = true;

    // ======== Handle based on edit type ========
    switch (ctx->edit_type)
    {
    case VALUE_EDIT_NUMERIC:
    {
        float *current_value = (float *)current_value_ptr;

        // Perform category-based safety checks
        if (strstr(ctx->label, "Voltage"))
        {
            if (*current_value < 10.0f || *current_value > 260.0f)
            {
                printf("Voltage out of safe range: %.2fV\n", *current_value);
                safety_check_passed = false;
            }
        }
        else if (strstr(ctx->label, "Current"))
        {
            if (*current_value < 0.1f || *current_value > 50.0f)
            {
                printf("Current limit unsafe: %.1fA\n", *current_value);
                safety_check_passed = false;
            }
        }
        else if (strstr(ctx->label, "Temperature"))
        {
            if (*current_value < 20.0f || *current_value > 80.0f)
            {
                printf("Temperature alarm outside reasonable range: %.1f°C\n", *current_value);
                safety_check_passed = false;
            }
        }
        else if (strstr(ctx->label, "Timeout"))
        {
            if (*current_value < 1000.0f || *current_value > 600000.0f)
            {
                printf("System timeout invalid (%.0f ms)\n", *current_value);
                safety_check_passed = false;
            }
        }

        if (safety_check_passed)
        {
            update_system_parameter(ctx, *current_value);
            printf("Numeric value confirmed: %s = %.3f %s\n",
                   ctx->label, *current_value, ctx->unit);
        }
        break;
    }

    case VALUE_EDIT_SELECT:
    {
        int selected_index = *(int *)current_value_ptr;
        printf("Selected option for %s: %d (%s)\n",
               ctx->label, selected_index, ctx->options[selected_index]);

        update_system_parameter(ctx, (float)selected_index);
        break;
    }

    case VALUE_EDIT_BOOL:
    {
        bool state = *(bool *)current_value_ptr;
        printf("%s set to: %s\n", ctx->label, state ? "ON" : "OFF");

        update_system_parameter(ctx, (float)state);
        if (strcmp(ctx->label, "WiFi") == 0)
            start_wifi_scan();
        break;
    }

    case VALUE_EDIT_LIST:
    {
        const char *selected_str = (const char *)current_value_ptr;
        printf("%s selected: %s\n", ctx->label, selected_str);

        update_system_parameter(ctx, 0); // store index if needed
        // eeprom_save_string(config->eeprom_addr, selected_str);
        break;
    }

    default:
        printf("Unknown value edit type for %s\n", ctx->label);
        safety_check_passed = false;
        break;
    }

    // ======== Post-confirmation handling ========
    if (safety_check_passed)
    {
        sys_state.value_changed = false;
        exit_value_edit_mode(true);

        /* Persist immediately -- without this, a confirmed change (e.g.
         * Battery Type / Voltage System) only survives if the board
         * happens to sleep, restart, or fault before losing power. */
        save_settings();

        show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
        lcd_flash_info_to(ctx->label, "Value Saved!    ", 1000, LCD_SCREEN_MENU);

        printf("AUDIT: Parameter changed - %s\n", ctx->label);
    }
    else
    {
        reset_value_to_backup();
        sys_state.value_changed = false;
        exit_value_edit_mode(false);

        show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
        lcd_flash_info_to("Change Rejected ", "                ", 1000, LCD_SCREEN_MENU);
    }
    sys_state.pending_confirmation = false;
}

void update_system_parameter(value_edit_context_t *ctx, float value)
{
    if (!ctx->apply)
    {
        printf("System: '%s' has no live-apply handler\n", ctx->label);
        return;
    }
    ctx->apply(value);
}

/**
 * @brief Set the inverter AC output voltage
 * @param voltage_setpoint Desired output voltage (RMS) in volts
 * @return true if successful, false otherwise
 */

// this is a mock implementation - replace with actual hardware using digit
bool inverter_set_output_voltage(float voltage_setpoint)
{
    if (voltage_setpoint < 100.0f || voltage_setpoint > 240.0f)
    {
        printf("ERROR: Voltage setpoint out of range: %.1f V\n", voltage_setpoint);
        return false;
    }
    printf("HAL: Setting output voltage to %.1f V\n", voltage_setpoint);
    // Update the sys_state output voltage
    sys_state.current_value_type->current_value = voltage_setpoint;
    sys_state.inverter.output_voltage = voltage_setpoint;
    return true;
}

/**
 * @brief Set the inverter AC output frequency
 * @param frequency_setpoint Desired output frequency in Hz
 * @return true if successful, false otherwise
 */
bool inverter_set_output_frequency(float frequency_setpoint)
{
    if (frequency_setpoint < 45.0f || frequency_setpoint > 65.0f)
    {
        printf("ERROR: Frequency setpoint out of range: %.2f Hz\n", frequency_setpoint);
        return false;
    }

    printf("HAL: Setting output frequency to %.2f Hz\n", frequency_setpoint);

    // Update the sys_state output frequency
    sys_state.inverter.output_frequency = frequency_setpoint;
    return true;
}

/**
 * @brief Set the current limit for overcurrent protection
 * @param current_limit_amps Maximum allowed current in Amps
 * @return true if successful, false otherwise
 */
bool inverter_set_current_limit(float current_limit_amps)
{
    if (current_limit_amps < 1.0f || current_limit_amps > 50.0f)
    {
        printf("ERROR: Current limit out of range: %.1f A\n", current_limit_amps);
        return false;
    }

    printf("HAL: Setting current limit to %.1f A\n", current_limit_amps);

    // Use the idea of ohms law to set current limit
    // Assuming a fixed resistance placeholder for load
    float load_resistance = 10.0f; // Ohms
    float voltage_setpoint = sys_state.inverter.output_voltage;
    float required_voltage = current_limit_amps * load_resistance;
    if (required_voltage > voltage_setpoint)
    {
        printf("WARNING: Current limit too high for current voltage, adjusting voltage\n");
        voltage_setpoint = required_voltage;
        inverter_set_output_voltage(voltage_setpoint);
    }
    sys_state.current_limit = current_limit_amps;

    return true;
}

/**
 * @brief Set the thermal protection temperature limit
 * @param temperature_limit_celsius Maximum allowed temperature in °C
 * @return true if successful, false otherwise
 */
bool thermal_protection_set_limit(float temperature_limit_celsius)
{
    if (temperature_limit_celsius < 40.0f || temperature_limit_celsius > 85.0f)
    {
        printf("ERROR: Temperature limit out of range: %.1f °C\n", temperature_limit_celsius);
        return false;
    }

    printf("HAL: Setting temperature limit to %.1f °C\n", temperature_limit_celsius);
    sys_state.temperature_limit = temperature_limit_celsius;

    return true;
}

/**
 * @brief Set the battery low voltage cutoff threshold
 * @param cutoff_voltage Battery voltage threshold for shutdown in Volts
 * @return true if successful, false otherwise
 */
bool battery_monitor_set_cutoff(float cutoff_voltage)
{
    if (cutoff_voltage < 10.0f || cutoff_voltage > 15.0f)
    {
        printf("ERROR: Battery cutoff voltage out of range: %.2f V\n", cutoff_voltage);
        return false;
    }
    printf("HAL: Setting battery cutoff voltage to %.2f V\n", cutoff_voltage);
    sys_state.cutoff_voltage = cutoff_voltage;

    return true;
}

// ============================================================================
// MONITORING AND PROTECTION TASKS
// ============================================================================

/**
 * @brief Thermal monitoring task - runs periodically to check temperature
 */
void thermal_monitoring_task(void *pvParameters)
{
    const TickType_t xFrequency = pdMS_TO_TICKS(500); // Check every 500ms
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1)
    {
        // Read temperature from ADC
        // uint16_t adc_value = adc1_get_raw(ADC1_CHANNEL_3);

        // Convert ADC to temperature
        // float voltage = (adc_value / 4095.0f) * 3.3f;
        // Calculate temperature using Steinhart-Hart equation

        // Check against threshold
        // if (temperature > g_system.temperature_limit) {
        //     printf("THERMAL SHUTDOWN: %.1f °C\n", temperature);
        //     g_hw_state.overtemp_flag = true;
        //     shutdown_inverter();
        // }

        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

void set_system_timeout(uint32_t timeout_ms)
{
    sys_state.system_timeout = timeout_ms;
    printf("System timeout set to %lu ms\n", timeout_ms);
}

/**
 * @brief Battery monitoring task - checks battery voltage continuously
 */
void battery_monitoring_task(void *pvParameters)
{
    const TickType_t xFrequency = pdMS_TO_TICKS(1000); // Check every 1 second
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1)
    {
        // Read battery voltage from ADC
        uint16_t adc_value = 23; // dummy value//adc1_get_raw(ADC_CHANNEL_6);

        // Convert to actual voltage
        float voltage = (adc_value / 4095.0f) * 3.3f * (110.0f / 10.0f); // Voltage divider ratio

        // Update state
        sys_state.inverter.battery.voltage = voltage;

        // Check against cutoff threshold
        if (voltage < sys_state.battery_cutoff && sys_state.inverter.inverter_state == INVERTER_ON)
        {
            // Generate error flag and shutdown
            printf("BATTERY LOW: %.2f V - SHUTTING DOWN\n", voltage);
            shutdown_inverter();
        }

        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

/**
 * @brief Emergency shutdown - immediately disable all outputs
 */
void inverter_emergency_shutdown(void)
{
    gpio_set_level(GPIO_POWER_RELAY, 0);
    sys_state.inverter.inverter_state = INVERTER_OFF;
    sys_state.inverter.inverter_active = false;
    sys_state.system_ready = false;
    lcd_show_fault("EMERGENCY HALT  ", "All outputs off ");
}

/*
 * =============================================================================
 * STEP 2: INDIVIDUAL BUTTON INITIALIZATION FUNCTIONS
 * =============================================================================
 */

/**
 * @brief Initialize Power Button (GPIO 0)
 */
static esp_err_t init_power_button(void)
{
    ESP_LOGI(APP_TAG, "🔋 Initializing Power Button on GPIO 0...");

    // Step 2.1: Get default configuration
    button_config_t config;
    button_controller_get_default_config(&config);
    config.gpio_pin = GPIO_BUTTON_POWER;
    config.debounce_ms = DEBOUNCE_THRESHOLD_MS;
    config.long_press_ms = LONG_PRESS_MS; // Quick long press for volume
    config.double_click_ms = 500;
    config.controller_name = "Power Button";
    config.active_low = true;
    config.enable_pullup = true;
    config.hold_repeat_ms = 100; // Fast repeat for smooth volume control
    config.enable_multi_click = true;

    // Step 2.3: Validate configuration
    esp_err_t ret = button_controller_validate_config(&config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔋 Power button config validation failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGE(APP_TAG, "🔋 Power button configuration validated ✓");

    // Step 2.4: Create controller instance
    ret = button_controller_create(&config, &g_app_state.power_button);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔋 Failed to create power button controller: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(APP_TAG, "🔋 Power button controller created ✓");

    // Step 2.5: Register event callback
    ret = button_controller_register_event_callback(g_app_state.power_button,
                                                    handle_power_button_event,
                                                    "PowerButton");
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔋 Failed to register power button event callback: %s", esp_err_to_name(ret));
        return ret;
    }

    // Step 2.7: Start button monitoring
    ret = button_controller_start(g_app_state.power_button);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔋 Failed to start power button controller: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(APP_TAG, "🔋 Power button controller started and monitoring ✓");

    return ESP_OK;
}

/**
 * @brief Initialize Menu Button (GPIO 2)
 */
static esp_err_t init_menu_button(void)
{
    ESP_LOGI(APP_TAG, "📱 Initializing Menu Button on GPIO 19...");

    // Step 2.1: Get and customize configuration
    button_config_t config;
    button_controller_get_default_config(&config);
    config.gpio_pin = GPIO_BUTTON_ENTER_MENU;
    config.debounce_ms = DEBOUNCE_THRESHOLD_MS;
    config.long_press_ms = LONG_PRESS_MS; // Quick long press for volume
    config.double_click_ms = 400;
    config.controller_name = "Menu Button";
    config.active_low = true;
    config.enable_pullup = true;
    config.hold_repeat_ms = 100;      // Fast repeat for smooth volume control
    config.enable_multi_click = true; // Enable multi-click detection

    // Step 2.2: Validate and create
    esp_err_t ret = button_controller_validate_config(&config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "📱 Menu button config validation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = button_controller_create(&config, &g_app_state.menu_button);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "📱 Failed to create menu button controller: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(APP_TAG, "📱 Menu button controller created ✓");

    // Step 2.3: Register callbacks
    ret = button_controller_register_event_callback(g_app_state.menu_button,
                                                    handle_enter_menu_button_event,
                                                    "MenuButton");
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "📱 Failed to register menu button event callback: %s", esp_err_to_name(ret));
        return ret;
    }

    // Step 2.4: Start monitoring
    ret = button_controller_start(g_app_state.menu_button);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "📱 Failed to start menu button controller: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(APP_TAG, "📱 Menu button controller started and monitoring ✓");

    return ESP_OK;
}

/**
 * @brief Initialize Volume Up Button (GPIO 4)
 */
static esp_err_t init_button_up(void)
{
    ESP_LOGI(APP_TAG, "🔊 Initializing Volume Up Button on GPIO 4...");

    button_config_t config;
    button_controller_get_default_config(&config);
    config.gpio_pin = GPIO_BUTTON_UP;
    config.debounce_ms = DEBOUNCE_THRESHOLD_MS;
    config.long_press_ms = LONG_PRESS_MS; // Quick long press for volume
    config.double_click_ms = 400;
    config.controller_name = "Volume Up Button";
    config.active_low = true;
    config.enable_pullup = true;
    config.hold_repeat_ms = 100;       // Fast repeat for smooth volume control
    config.enable_multi_click = false; // Enable multi-click detection
    esp_err_t ret = button_controller_validate_config(&config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔊 Volume up config validation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = button_controller_create(&config, &g_app_state.button_up);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔊 Failed to create volume up controller: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(APP_TAG, "🔊 Volume up controller created ✓");

    ret = button_controller_register_event_callback(g_app_state.button_up,
                                                    handle_up_button_event,
                                                    "VolumeUpButton");
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔊 Failed to register volume up event callback: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = button_controller_start(g_app_state.button_up);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔊 Failed to start volume up controller: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(APP_TAG, "🔊 Volume up controller started and monitoring ✓");

    return ESP_OK;
}

/**
 * @brief Initialize Volume Down Button (GPIO 5)
 */
static esp_err_t init_down_button(void)
{
    ESP_LOGI(APP_TAG, "🔉 Initializing Volume Down Button on GPIO 5...");

    button_config_t config;
    button_controller_get_default_config(&config);
    config.gpio_pin = GPIO_BUTTON_DOWN;
    config.debounce_ms = DEBOUNCE_THRESHOLD_MS;
    config.long_press_ms = LONG_PRESS_MS; // Quick long press for volume
    config.double_click_ms = 400;
    config.controller_name = "Volume Down Button";
    config.active_low = true;
    config.enable_pullup = true;
    config.hold_repeat_ms = 100;       // Fast repeat for smooth volume control
    config.enable_multi_click = false; // Enable multi-click detection
    esp_err_t ret = button_controller_validate_config(&config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔉 Volume down config validation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = button_controller_create(&config, &g_app_state.button_down);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔉 Failed to create volume down controller: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(APP_TAG, "🔉 Volume down controller created ✓");

    ret = button_controller_register_event_callback(g_app_state.button_down,
                                                    handle_down_button_event,
                                                    "VolumeDownButton");
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔉 Failed to register volume down event callback: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = button_controller_start(g_app_state.button_down);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔉 Failed to start volume down controller: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(APP_TAG, "🔉 Volume down controller started and monitoring ✓");

    return ESP_OK;
}

/**
 * @brief Initialize back button (GPIO 4)
 */
static esp_err_t init_back_button(void)
{
    ESP_LOGI(APP_TAG, "🔙 Initializing Back Button on GPIO 4...");

    button_config_t config;
    button_controller_get_default_config(&config);
    config.gpio_pin = GPIO_BUTTON_BACK;
    config.debounce_ms = DEBOUNCE_THRESHOLD_MS;
    config.long_press_ms = LONG_PRESS_MS; // Quick long press for volume
    config.double_click_ms = 400;
    config.controller_name = "Back Button";
    config.active_low = true;
    config.enable_pullup = true;
    config.hold_repeat_ms = 100;       // Fast repeat for smooth volume control
    config.enable_multi_click = false; // Enable multi-click detection

    esp_err_t ret = button_controller_validate_config(&config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔙 Back button config validation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = button_controller_create(&config, &g_app_state.button_back);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔙 Failed to create back button controller: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(APP_TAG, "🔙 Back button controller created ✓");

    ret = button_controller_register_event_callback(g_app_state.button_back,
                                                    handle_back_button_event,
                                                    "BackButton");
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔙 Failed to register back button event callback: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = button_controller_start(g_app_state.button_back);
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔙 Failed to start back button controller: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(APP_TAG, "🔙 Back button controller started and monitoring ✓");

    return ESP_OK;
}

/*
 * =============================================================================
 * STEP 3: SYSTEM INITIALIZATION AND STATUS MONITORING
 * =============================================================================
 */

/**
 * @brief Initialize basic ESP32 system components
 */
static esp_err_t init_system_basics(void)
{
    ESP_LOGI(APP_TAG, "🚀 Initializing ESP32 system basics...");

    // Step 3.1: Initialize NVS Flash
    ESP_LOGI(APP_TAG, "🚀 Initializing NVS Flash...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(APP_TAG, "🚀 NVS Flash needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🚀 Failed to initialize NVS Flash: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(APP_TAG, "🚀 NVS Flash initialized ✓");

    // Step 3.2: Set log levels for different components
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("BUTTON_CTRL", ESP_LOG_INFO);
    esp_log_level_set("BUTTON_APP", ESP_LOG_INFO);
    ESP_LOGI(APP_TAG, "🚀 Log levels configured ✓");

    // Step 3.3: Initialize application state
    memset(&g_app_state, 0, sizeof(app_state_t));
    g_app_state.system_ready = false;
    ESP_LOGI(APP_TAG, "🚀 Application state initialized ✓");

    ESP_LOGI(APP_TAG, "🚀 System basics initialization complete ✓");
    return ESP_OK;
}

/**
 * @brief Print detailed system status
 */
static void print_system_status(void)
{
    ESP_LOGI(APP_TAG, "\n"
                      "┌─────────────────────────────────────────────────────────────┐\n"
                      "│                    BUTTON SYSTEM STATUS                    │\n"
                      "├─────────────────────────────────────────────────────────────┤");

    ESP_LOGI(APP_TAG, "│ Free Heap: %lu bytes                                    │",
             esp_get_free_heap_size());

    ESP_LOGI(APP_TAG, "│ Min Free Heap: %lu bytes                               │",
             esp_get_minimum_free_heap_size());

    ESP_LOGI(APP_TAG, "├─────────────────────────────────────────────────────────────┤");

    // Button status
    bool power_pressed = false, menu_pressed = false;
    bool vol_up_pressed = false, vol_down_pressed = false;
    button_state_t power_state, menu_state, vol_up_state, vol_down_state;

    if (g_app_state.power_button)
    {
        button_controller_is_pressed(g_app_state.power_button, &power_pressed);
        button_controller_get_state(g_app_state.power_button, &power_state);
    }
    if (g_app_state.menu_button)
    {
        button_controller_is_pressed(g_app_state.menu_button, &menu_pressed);
        button_controller_get_state(g_app_state.menu_button, &menu_state);
    }
    if (g_app_state.button_up)
    {
        button_controller_is_pressed(g_app_state.button_up, &vol_up_pressed);
        button_controller_get_state(g_app_state.button_up, &vol_up_state);
    }
    if (g_app_state.button_down)
    {
        button_controller_is_pressed(g_app_state.button_down, &vol_down_pressed);
        button_controller_get_state(g_app_state.button_down, &vol_down_state);
    }

    ESP_LOGI(APP_TAG, "│ 🔋 Power Button (GPIO 0):  %s | %s    │",
             power_pressed ? "PRESSED " : "RELEASED",
             power_pressed ? button_state_to_string(power_state) : "IDLE     ");

    ESP_LOGI(APP_TAG, "│ 📱 Menu Button (GPIO 2):   %s | %s    │",
             menu_pressed ? "PRESSED " : "RELEASED",
             menu_pressed ? button_state_to_string(menu_state) : "IDLE     ");

    ESP_LOGI(APP_TAG, "│ 🔊 Vol Up Button (GPIO 4): %s | %s    │",
             vol_up_pressed ? "PRESSED " : "RELEASED",
             vol_up_pressed ? button_state_to_string(vol_up_state) : "IDLE     ");

    ESP_LOGI(APP_TAG, "│ 🔉 Vol Down Button (GPIO 5): %s | %s  │",
             vol_down_pressed ? "PRESSED " : "RELEASED",
             vol_down_pressed ? button_state_to_string(vol_down_state) : "IDLE     ");

    ESP_LOGI(APP_TAG, "├─────────────────────────────────────────────────────────────┤");
    ESP_LOGI(APP_TAG, "│ Power Button Presses: %lu                               │", g_app_state.power_press_count);
    ESP_LOGI(APP_TAG, "│ Menu Button Presses: %lu                                │", g_app_state.menu_press_count);
    ESP_LOGI(APP_TAG, "└─────────────────────────────────────────────────────────────┘");
}

/**
 * @brief Status monitoring task
 */
static void status_monitoring_task(void *parameter)
{
    ESP_LOGI(APP_TAG, "📊 Status monitoring task started");

    const TickType_t status_interval = pdMS_TO_TICKS(15000); // Every 15 seconds
    const TickType_t stats_interval = pdMS_TO_TICKS(60000);  // Every 60 seconds

    TickType_t last_status_time = 0;
    TickType_t last_stats_time = 0;

    while (g_app_state.system_ready)
    {
        TickType_t current_time = xTaskGetTickCount();

        // Print status every 15 seconds
        if (current_time - last_status_time >= status_interval)
        {
            // print_system_status();
            last_status_time = current_time;
        }

        // Print detailed statistics every 60 seconds
        if (current_time - last_stats_time >= stats_interval)
        {
            last_stats_time = current_time;
        }

        // Check for low memory condition
        uint32_t free_heap = esp_get_free_heap_size();
        if (free_heap < 10000)
        { // Less than 10KB free
            ESP_LOGW(APP_TAG, "⚠️ Low memory warning: %lu bytes free", free_heap);
        }

        vTaskDelay(pdMS_TO_TICKS(1000)); // Check every second
    }

    ESP_LOGI(APP_TAG, "📊 Status monitoring task ended");
    vTaskDelete(NULL);
}

/*
 * =============================================================================
 * STEP 4: COMPLETE INITIALIZATION SEQUENCE
 * =============================================================================
 */

/**
 * @brief Complete button system initialization
 */
static esp_err_t initialize_button_system(void)
{
    ESP_LOGI(APP_TAG, "\n"
                      "╔═══════════════════════════════════════════════════════════╗\n"
                      "║              BUTTON SYSTEM INITIALIZATION                ║\n"
                      "║                    STARTING...                           ║\n"
                      "╚═══════════════════════════════════════════════════════════╝");

    esp_err_t ret;

    // Step 4.1: Initialize basic system components
    ESP_LOGI(APP_TAG, "⚙️ Step 1: Initializing system basics...");
    ret = init_system_basics();
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "❌ System basics initialization failed!");
        return ret;
    }
    ESP_LOGI(APP_TAG, "✅ Step 1 complete: System basics initialized");

    // Step 4.2: Initialize button controller system
    ESP_LOGI(APP_TAG, "⚙️ Step 2: Initializing button controller system...");
    ret = button_controller_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "❌ Button controller system initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(APP_TAG, "✅ Step 2 complete: Button controller system initialized");

    // Step 4.3: Initialize individual buttons sequentially
    ESP_LOGI(APP_TAG, "⚙️ Step 3: Initializing individual button controllers...");

    // Sub-step 3.1: Power Button
    ESP_LOGE(APP_TAG, "⚙️ Step 3.1: Initializing Power Button...");
    ret = init_power_button();
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "❌ Power button initialization failed!");
        goto cleanup;
    }
    ESP_LOGI(APP_TAG, "✅ Step 3.1 complete: Power button ready");
    vTaskDelay(pdMS_TO_TICKS(100)); // Small delay between initializations

    // Sub-step 3.2: Menu Button
    ESP_LOGE(APP_TAG, "⚙️ Step 3.2: Initializing Menu Button...");
    ret = init_menu_button();
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "❌ Menu button initialization failed!");
        goto cleanup;
    }
    ESP_LOGI(APP_TAG, "✅ Step 3.2 complete: Menu button ready");
    vTaskDelay(pdMS_TO_TICKS(100));

    // Sub-step 3.3: Up Button
    ESP_LOGI(APP_TAG, "⚙️ Step 3.3: Initializing Up Button...");
    ret = init_button_up();
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "❌ Up button initialization failed!");
        goto cleanup;
    }
    ESP_LOGI(APP_TAG, "✅ Step 3.3 complete: Up button ready");
    vTaskDelay(pdMS_TO_TICKS(100));

    // Sub-step 3.4: Down Button
    ESP_LOGI(APP_TAG, "⚙️ Step 3.4: Initializing Down Button...");
    ret = init_down_button();
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "❌ Down button initialization failed!");
        goto cleanup;
    }

    // Sub-step 3.5: Back Button
    ESP_LOGI(APP_TAG, "⚙️ Step 3.5: Initializing Back Button...");
    ret = init_back_button();
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "❌ Back button initialization failed!");
        goto cleanup;
    }
    ESP_LOGI(APP_TAG, "✅ Step 3.5 complete: Back button ready");

    ESP_LOGI(APP_TAG, "✅ Step 3 complete: All button controllers initialized");

    // Step 4.4: Start monitoring task
    ESP_LOGI(APP_TAG, "⚙️ Step 4: Starting system monitoring...");
    g_app_state.system_ready = true;

    TaskHandle_t monitor_task_handle;
    BaseType_t task_created = xTaskCreate(status_monitoring_task,
                                          "status_monitor",
                                          4096,
                                          NULL,
                                          3,
                                          &monitor_task_handle);

    if (task_created != pdPASS)
    {
        ESP_LOGE(APP_TAG, "❌ Failed to create monitoring task");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    ESP_LOGI(APP_TAG, "✅ Step 4 complete: System monitoring started");

    // Step 4.5: Final system verification
    ESP_LOGI(APP_TAG, "⚙️ Step 5: Performing final system verification...");

    // Verify all controllers are running
    bool all_running = true;
    button_state_t state;

    if (button_controller_get_state(g_app_state.power_button, &state) != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "❌ Power button controller not responding!");
        all_running = false;
    }

    if (button_controller_get_state(g_app_state.menu_button, &state) != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "❌ Menu button controller not responding!");
        all_running = false;
    }

    if (button_controller_get_state(g_app_state.button_up, &state) != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "❌ Volume up button controller not responding!");
        all_running = false;
    }

    if (button_controller_get_state(g_app_state.button_down, &state) != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "❌ Volume down button controller not responding!");
        all_running = false;
    }

    if (!all_running)
    {
        ESP_LOGE(APP_TAG, "❌ System verification failed!");
        ret = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    ESP_LOGI(APP_TAG, "✅ Step 5 complete: System verification passed");

    // Success! Print final status
    ESP_LOGI(APP_TAG, "\n"
                      "╔═══════════════════════════════════════════════════════════╗\n"
                      "║           BUTTON SYSTEM INITIALIZATION COMPLETE          ║\n"
                      "║                      SUCCESS! ✅                         ║\n"
                      "╚═══════════════════════════════════════════════════════════╝");

    print_system_status();

    ESP_LOGI(APP_TAG, "\n🎉 SYSTEM READY! Button patterns to try:");
    ESP_LOGI(APP_TAG, "🔋 Power Button (GPIO 0):");
    ESP_LOGI(APP_TAG, "   • Single press: Toggle power");
    ESP_LOGI(APP_TAG, "   • Double click: Sleep mode");
    ESP_LOGI(APP_TAG, "   • Long press (3s): Force shutdown");
    ESP_LOGI(APP_TAG, "📱 Menu Button (GPIO 2):");
    ESP_LOGI(APP_TAG, "   • Single press: Next menu");
    ESP_LOGI(APP_TAG, "   • Double click: Settings");
    ESP_LOGI(APP_TAG, "   • Triple click: Diagnostics");
    ESP_LOGI(APP_TAG, "   • Long press (1.5s): Main menu");
    ESP_LOGI(APP_TAG, "🔊 Volume Buttons (GPIO 4 & 5):");
    ESP_LOGI(APP_TAG, "   • Single press: Volume +/- 5%%");
    ESP_LOGI(APP_TAG, "   • Hold: Continuous volume change");

    return ESP_OK;

cleanup:
    ESP_LOGE(APP_TAG, "\n"
                      "╔═══════════════════════════════════════════════════════════╗\n"
                      "║         BUTTON SYSTEM INITIALIZATION FAILED! ❌           ║\n"
                      "║                   CLEANING UP...                          ║\n"
                      "╚═══════════════════════════════════════════════════════════╝");

    cleanup_button_system();
    return ret;
}

static void cleanup_button_system(void)
{
    ESP_LOGI(APP_TAG, "🧹 Cleaning up button system...");
    lcd_watchdog_deinit();
    g_app_state.system_ready = false;

    // Stop and destroy all button controllers
    if (g_app_state.power_button)
    {
        button_controller_stop(g_app_state.power_button);
        button_controller_destroy(g_app_state.power_button);
        g_app_state.power_button = NULL;
        ESP_LOGI(APP_TAG, "🧹 Power button controller cleaned up");
    }

    if (g_app_state.menu_button)
    {
        button_controller_stop(g_app_state.menu_button);
        button_controller_destroy(g_app_state.menu_button);
        g_app_state.menu_button = NULL;
        ESP_LOGI(APP_TAG, "🧹 Menu button controller cleaned up");
    }

    if (g_app_state.button_up)
    {
        button_controller_stop(g_app_state.button_up);
        button_controller_destroy(g_app_state.button_up);
        g_app_state.button_up = NULL;
        ESP_LOGI(APP_TAG, "🧹 Volume up button controller cleaned up");
    }

    if (g_app_state.button_down)
    {
        button_controller_stop(g_app_state.button_down);
        button_controller_destroy(g_app_state.button_down);
        g_app_state.button_down = NULL;
        ESP_LOGI(APP_TAG, "🧹 Volume down button controller cleaned up");
    }

    // Deinitialize button controller system
    button_controller_deinit();
    ESP_LOGI(APP_TAG, "🧹 Button controller system deinitialized");

    ESP_LOGI(APP_TAG, "🧹 Button system cleanup complete");
}

/*
 * =============================================================================
 * STEP 5: MAIN APPLICATION ENTRY POINT
 * =============================================================================
 */

/**
 * @brief Main application task
 */

/*
 * =============================================================================
 * STEP 6: RUNTIME EXAMPLE AND TESTING FUNCTIONS
 * =============================================================================
 */

/**
 * @brief Demonstrate button testing sequence
 */
void demonstrate_button_testing(void)
{
    if (!g_app_state.system_ready)
    {
        ESP_LOGE(APP_TAG, "System not ready for testing!");
        return;
    }

    ESP_LOGI(APP_TAG, "\n"
                      "┌─────────────────────────────────────────────────────────────┐\n"
                      "│                  BUTTON TESTING SEQUENCE                   │\n"
                      "└─────────────────────────────────────────────────────────────┘");

    // Test each button's current state
    bool pressed;
    button_state_t state;

    ESP_LOGI(APP_TAG, "🔍 Testing button states...");

    if (button_controller_is_pressed(g_app_state.power_button, &pressed) == ESP_OK &&
        button_controller_get_state(g_app_state.power_button, &state) == ESP_OK)
    {
        ESP_LOGI(APP_TAG, "🔋 Power Button: %s, State: %s",
                 pressed ? "PRESSED" : "RELEASED",
                 button_state_to_string(state));
    }

    if (button_controller_is_pressed(g_app_state.menu_button, &pressed) == ESP_OK &&
        button_controller_get_state(g_app_state.menu_button, &state) == ESP_OK)
    {
        ESP_LOGI(APP_TAG, "📱 Menu Button: %s, State: %s",
                 pressed ? "PRESSED" : "RELEASED",
                 button_state_to_string(state));
    }

    if (button_controller_is_pressed(g_app_state.button_up, &pressed) == ESP_OK &&
        button_controller_get_state(g_app_state.button_up, &state) == ESP_OK)
    {
        ESP_LOGI(APP_TAG, "🔊 Volume Up: %s, State: %s",
                 pressed ? "PRESSED" : "RELEASED",
                 button_state_to_string(state));
    }

    if (button_controller_is_pressed(g_app_state.button_down, &pressed) == ESP_OK &&
        button_controller_get_state(g_app_state.button_down, &state) == ESP_OK)
    {
        ESP_LOGI(APP_TAG, "🔉 Volume Down: %s, State: %s",
                 pressed ? "PRESSED" : "RELEASED",
                 button_state_to_string(state));
    }

    ESP_LOGI(APP_TAG, "🔍 Button testing complete");
}

/**
 * @brief Reset all button statistics
 */
void reset_all_button_statistics(void)
{
    ESP_LOGI(APP_TAG, "📊 Resetting all button statistics...");

    if (g_app_state.power_button)
    {
        button_controller_reset_stats(g_app_state.power_button);
    }
    if (g_app_state.menu_button)
    {
        button_controller_reset_stats(g_app_state.menu_button);
    }
    if (g_app_state.button_up)
    {
        button_controller_reset_stats(g_app_state.button_up);
    }
    if (g_app_state.button_down)
    {
        button_controller_reset_stats(g_app_state.button_down);
    }

    // Reset application statistics
    g_app_state.power_press_count = 0;
    g_app_state.menu_press_count = 0;

    ESP_LOGI(APP_TAG, "📊 All statistics reset to zero");
}

void log_all_error_flags(uint32_t flags)
{
    if (flags == 0)
    {
        ESP_LOGI("ERROR_FLAGS", "No errors set");
        return;
    }

    ESP_LOGW("ERROR_FLAGS", "Active error flags: 0x%02X", (unsigned int)flags);

    if (flags & ERR_OVER_TEMP)
        ESP_LOGW("ERROR_FLAGS", "  ✓ ERR_OVER_TEMP (0x01)");
    if (flags & ERR_OVERLOAD)
        ESP_LOGW("ERROR_FLAGS", "  ✓ ERR_OVERLOAD (0x02)");
    if (flags & ERR_BATTERY_VOLTAGE)
        ESP_LOGW("ERROR_FLAGS", "  ✓ ERR_BATTERY_VOLTAGE (0x03)");
    if (flags & ERR_LOW_BAT)
        ESP_LOGW("ERROR_FLAGS", "  ✓ ERR_LOW_BAT (0x04)");
    if (flags & ERR_UNDER_VOLTAGE)
        ESP_LOGW("ERROR_FLAGS", "  ✓ ERR_UNDER_VOLTAGE (0x05)");
    if (flags & ERR_OVER_VOLTAGE)
        ESP_LOGW("ERROR_FLAGS", "  ✓ ERR_OVER_VOLTAGE (0x06)");
    if (flags & ERR_INVERTER_VOLTAGE)
        ESP_LOGW("ERROR_FLAGS", "  ✓ ERR_INVERTER_VOLTAGE (0x07)");
    if (flags & ERR_AC_FAULT)
        ESP_LOGW("ERROR_FLAGS", "  ✓ ERR_AC_FAULT (0x08)");
    if (flags & ERR_FAN_FAIL)
        ESP_LOGW("ERROR_FLAGS", "  ✓ ERR_FAN_FAIL (0x10)");
    if (flags & ERR_EEPROM)
        ESP_LOGW("ERROR_FLAGS", "  ✓ ERR_EEPROM (0x20)");
    if (flags & ERR_HIGH_BAT)
        ESP_LOGW("ERROR_FLAGS", "  ✓ ERR_HIGH_BAT (0x40)");
    if (flags & ERR_SHORT_CIRCUIT)
        ESP_LOGW("ERROR_FLAGS", "  ✓ ERR_SHORT_CIRCUIT (0x80)");
    if (flags & ERR_SYSTEM_FAILURE)
        ESP_LOGW("ERROR_FLAGS", "  ✓ ERR_SYSTEM_FAILURE (0x90)");
    if (flags & ERR_OVER_UNDER_VOLTAGE)
        ESP_LOGW("ERROR_FLAGS", "  ✓ ERR_OVER_UNDER_VOLTAGE (0x50)");
}

// ================== UNIFIED MENU INPUT HANDLER ==================

// Helper functions for menu handling
void toggle_display()
{
    sys_state.display.display_on = !sys_state.display.display_on;
    LCD_power(sys_state.display.display_on);
    update_led(LED_STATUS, sys_state.display.display_on ? 100 : 0);
}

/* ── navigate_to_menu() ─────────────────────────────────────────────────── */
void navigate_to_menu(menu_state_t menu)
{
    sys_state.display.current_menu = menu;
    show_menu_screen(menu, 0);
}

// battery sub menu voltage setting
#define MAIN_MENU_ITEM_COUNT 3 // Type, Voltage, Cutoff

#define VOLTAGE_TYPE_COUNT 4
static int voltage_levels[VOLTAGE_TYPE_COUNT] = {12, 24, 48, 96}; // Supported voltage levels

int get_voltage_index(int voltage)
{
    // Find the index of the given voltage in the voltage_levels array
    // Returns 0 for 12V, 1 for 24V, 2 for 48V, or defaults to 0 if not found
    if (voltage < 12 || voltage > 48)
    {
        return 0; // default to 12V if out of range
    }
    // Check for code to match each voltage level
    // Recall that the voltage levels might differ with some values e.g. 12V, 24V, 48V, 96V
    for (int i = 0; i < VOLTAGE_TYPE_COUNT; i++)
    {
        if (voltage_levels[i] == voltage)
        {
            return i; // Return the index of the matching voltage
        }
    }
    return 0; // default to 12V
}

/**
 * @brief Validate and clamp settings after loading from NVS.
 *
 * Runs range checks that a single nvs_get_* success/failure can't catch —
 * a value can be technically present in flash and still be dangerous
 * (wrong order relative to another setting, or corrupted into an
 * out-of-range number without being an ESP_ERR_NVS_NOT_FOUND).
 *
 * @return true if every value needed a value changed from what was loaded
 *         (caller should persist the corrected values back to NVS).
 */
static bool validate_and_clamp_settings(void)
{
    bool corrected = false;

    if (sys_state.battery_profile.profile_id >= BATTERY_TYPE_COUNT)
    {
        ESP_LOGE(TAG_SYS, "Invalid battery type %d loaded — resetting to AGM",
                 sys_state.battery_profile.profile_id);
        battery_profile_t regenerated;
        battery_generate_profile(BATTERY_AGM, VOLTAGE_SYSTEM_12V, 100, &regenerated);
        sys_state.battery_profile = regenerated;
        sys_state.battery_profile.profile_id = BATTERY_AGM;
        corrected = true;
    }

    /* ---- Battery cutoff voltage ---- */
    float cutoff_floor = sys_state.battery_profile.cutoff_voltage_min_12v;
    if (sys_state.battery_profile.cutoff_voltage_12v < cutoff_floor)
    {
        ESP_LOGW(TAG_SYS, "Cutoff %.2fV below hw floor %.2fV — clamping",
                 sys_state.battery_profile.cutoff_voltage_12v, cutoff_floor);
        sys_state.battery_profile.cutoff_voltage_12v = cutoff_floor;
        corrected = true;
    }

    /* ---- Battery recharge voltage vs its own ceiling ---- */
    float recharge_ceiling = sys_state.battery_profile.high_battery_voltage_12v;
    if (sys_state.battery_profile.recharge_voltage_12v > recharge_ceiling)
    {
        ESP_LOGW(TAG_SYS, "Recharge %.2fV above hw ceiling %.2fV — clamping",
                 sys_state.battery_profile.recharge_voltage_12v, recharge_ceiling);
        sys_state.battery_profile.recharge_voltage_12v = recharge_ceiling;
        corrected = true;
    }

    /* ---- Cross-check: cutoff must stay below recharge by a safety
     * margin, otherwise the system could oscillate between "shut down,
     * low battery" and "resume, still low battery" every few seconds. ---- */
    const float CUTOFF_RECHARGE_MARGIN_V = 0.3f;
    if (sys_state.battery_profile.cutoff_voltage_12v >=
        sys_state.battery_profile.recharge_voltage_12v - CUTOFF_RECHARGE_MARGIN_V)
    {
        ESP_LOGE(TAG_SYS,
                 "Cutoff (%.2fV) too close to/above recharge (%.2fV) — "
                 "forcing recharge = cutoff + %.1fV",
                 sys_state.battery_profile.cutoff_voltage_12v,
                 sys_state.battery_profile.recharge_voltage_12v,
                 CUTOFF_RECHARGE_MARGIN_V);
        sys_state.battery_profile.recharge_voltage_12v =
            sys_state.battery_profile.cutoff_voltage_12v + CUTOFF_RECHARGE_MARGIN_V;

        if (sys_state.battery_profile.recharge_voltage_12v > recharge_ceiling)
        {
            sys_state.battery_profile.recharge_voltage_12v = recharge_ceiling;
            sys_state.battery_profile.cutoff_voltage_12v =
                recharge_ceiling - CUTOFF_RECHARGE_MARGIN_V;
        }
        corrected = true;
    }

    /* ---- Current Limit---- */
    if (sys_state.current_limit < 1.0f || sys_state.current_limit > 50.0f)
    {
        ESP_LOGW(TAG_SYS, "Current limit %.1fA out of range — clamping",
                 sys_state.current_limit);
        sys_state.current_limit = clamp_float(sys_state.current_limit, 1.0f, 50.0f);
        corrected = true;
    }

    /* ---- Temperature alarm ---- */
    if (sys_state.temperature_limit < 40.0f || sys_state.temperature_limit > HEATSINK_TEMP_MAX)
    {
        ESP_LOGW(TAG_SYS, "Temp alarm %.1fC out of range — clamping",
                 sys_state.temperature_limit);
        sys_state.temperature_limit = clamp_float(sys_state.temperature_limit, 40.0f, HEATSINK_TEMP_MAX);
        corrected = true;
    }

    /* ---- Output voltage / voltage threshold ---- */
    if (sys_state.inverter.output_voltage < 100.0f || sys_state.inverter.output_voltage > 240.0f)
    {
        ESP_LOGW(TAG_SYS, "Output voltage %.1fV out of range — clamping",
                 sys_state.inverter.output_voltage);
        sys_state.inverter.output_voltage = clamp_float(sys_state.inverter.output_voltage, 100.0f, 240.0f);
        corrected = true;
    }
    if (sys_state.settings.voltage_threshold < 100.0f || sys_state.settings.voltage_threshold > 240.0f)
    {
        sys_state.settings.voltage_threshold = clamp_float(sys_state.settings.voltage_threshold, 100.0f, 240.0f);
        corrected = true;
    }

    /* ---- Output frequency ---- */
    if (sys_state.inverter.output_frequency < 45.0f || sys_state.inverter.output_frequency > 65.0f)
    {
        ESP_LOGW(TAG_SYS, "Output frequency %.2fHz out of range — clamping",
                 sys_state.inverter.output_frequency);
        sys_state.inverter.output_frequency = clamp_float(sys_state.inverter.output_frequency, 45.0f, 65.0f);
        corrected = true;
    }
    if (sys_state.settings.frequency_range < MIN_FREQUENCY || sys_state.settings.frequency_range > MAX_FREQUENCY)
    {
        sys_state.settings.frequency_range = (sys_state.settings.frequency_range < MIN_FREQUENCY)
                                                 ? MIN_FREQUENCY
                                                 : MAX_FREQUENCY;
        corrected = true;
    }

    /* ---- System timeout: floor matches MIN_OFF_TIME_MS (rapid-cycle
     * protection already used elsewhere in the file); ceiling is a
     * sane upper bound so a corrupted value can't leave the menu open
     * "forever". ---- */
    if (sys_state.system_timeout < MIN_OFF_TIME_MS || sys_state.system_timeout > 600000)
    {
        ESP_LOGW(TAG_SYS, "System timeout %lu ms out of range — clamping",
                 (unsigned long)sys_state.system_timeout);
        sys_state.system_timeout = (sys_state.system_timeout < MIN_OFF_TIME_MS)
                                       ? MIN_OFF_TIME_MS
                                       : 600000;
        corrected = true;
    }

    /* ---- Scroll speed / enable ---- */
    if (sys_state.display.scroll_speed < 1 || sys_state.display.scroll_speed > 10)
    {
        sys_state.display.scroll_speed = DEFAULT_SCROLL_SPEED;
        corrected = true;
    }
    if (sys_state.display.scroll_enabled && sys_state.display.scroll_speed == 0)
    {
        sys_state.display.scroll_speed = DEFAULT_SCROLL_SPEED;
        corrected = true;
    }
    sys_state.display.scroll_enabled = sys_state.display.scroll_enabled ? 1 : 0;
    sys_state.display.auto_shutdown_enabled = sys_state.display.auto_shutdown_enabled ? 1 : 0;

    /* ---- Backlight timeout: 0 is a valid "never dim" sentinel, but
     * 1–4 seconds is almost certainly a corrupted/garbage value rather
     * than an intentional setting. ---- */
    if (sys_state.display.backlight_timeout > 0 && sys_state.display.backlight_timeout < 5)
    {
        sys_state.display.backlight_timeout = 5;
        corrected = true;
    }

    /* ---- Brightness ---- */
    if (sys_state.display.brightness < 0 || sys_state.display.brightness > 255)
    {
        sys_state.display.brightness = clamp_float(sys_state.display.brightness, 0, 255);
        corrected = true;
    }

    return corrected;
}

/* ── menu_exit() ────────────────────────────────────────────────────────── */
void menu_exit(void)
{
    lcd_flash_info("Exiting menu... ", "                ", 1000);
    sys_state.display.current_menu = MAIN_MENU;
}

#define MAX_PROFILES 3

/* ── show_profile_on_lcd() ──────────────────────────────────────────────── */
void show_profile_on_lcd(battery_profile_t *profile)
{
    char l[17], v[17];
    snprintf(l, 17, "Battery:%4.1fV  ", (float)profile->nominal_voltage * 10);
    snprintf(v, 17, "Cutoff:%5.1fV   ", profile->cutoff_voltage_12v * 10);
    lcd_show_monitor_detail(l, v);
}

// Assume that these are the possible frequency options or ranges
#define MIN_FREQUENCY 50  // Minimum frequency in Hz
#define MAX_FREQUENCY 200 // Maximum frequency in Hz
#define FREQUENCY_STEP 1  // Frequency step size (1 Hz)

// Function to save the frequency setting to NVS
void save_frequency_to_nvs(int frequency)
{
    nvs_handle_t nvs_handler;
    esp_err_t err = nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &nvs_handler);
    if (err == ESP_OK)
    {
        nvs_set_i32(nvs_handler, FREQUENCY_SETTING_KEY, frequency);
        nvs_commit(nvs_handler);
        nvs_close(nvs_handler);
    }
}

// ================== INITIALIZATION ==================
// =============== INITIALIZE MENU SYSTEM ===============
void init_menu_system()
{
    // Set the initial menu state to the main menu
    sys_state.menu_state = MENU_NONE;
    sys_state.system_ready = true;
    sys_state.inverter.inverter_state = INVERTER_OFF;
    sys_state.menu_selection = 0;
    sys_state.pending_confirmation = false;
    sys_state.security.enabled = false;
    // lcd_display_state
    sys_state.lcd_state.blink_state = false;
    sys_state.lcd_boot_state.boot_screen_timestamp_ms = 0;
    sys_state.inverter.battery.battery_last_update_tick = 0;
    // Clear any previous menu editing context
    memset(&menu_edit, 0, sizeof(menu_edit)); // Zero out edit state
    sys_state.system_ready = true;
    battery_profile_t *profile = &sys_state.battery_profile;
    battery_system_init(profile);
    battery_profile_load_from_nvs(profile);
    printf("Loading battery profile from NVS...\n");
    if (!load_settings())
    {
        ESP_LOGW(TAG_SYS, "Failed to load settings from NVS, applying defaults");
        sys_state.battery_profile = battery_profiles[BATTERY_CHEMISTRY_LITHIUM_ION];
    }
    // Program reached here indication
    printf("System initialization complete. Ready for operation.\n");
}

// ================== RESTORE STATE ON WAKE =================
void restore_from_deep_sleep()
{
    // RTC restore is now handled in init_system_state()
    // This function can be called for additional setup if needed
    ESP_LOGI("RTC", "RTC memory magic: 0x%08lX", rtc_mem.magic_flag);
    ESP_LOGI("RTC", "Wake count: %lu", rtc_mem.wake_count);
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
    lcd_flash_info("Entering Sleep  ", v, 2000);

    save_settings();
    post_buzzer_event(true);
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

// ================== DEEP SLEEP INITIALIZATION ==================
void init_deep_sleep(uint64_t wakeup_pin_mask, int wakeup_time_sec)
{
    // Configure wakeup sources
    if (wakeup_pin_mask != 0)
    {
        esp_sleep_enable_ext0_wakeup(wakeup_pin_mask, ESP_EXT1_WAKEUP_ALL_LOW); // Configure specific pin
        // OR for multiple pins:
        // esp_sleep_enable_ext1_wakeup(wakeup_pin_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
    }

    if (wakeup_time_sec > 0)
    {
        esp_sleep_enable_timer_wakeup(wakeup_time_sec * 1000000); // Convert seconds to microseconds
    }

    // Configure power domains
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_ON);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_OFF);
    // Isolate GPIO pins to reduce power consumption
    for (int i = 0; i < GPIO_PIN_COUNT; i++)
    {
        if ((wakeup_pin_mask & (1ULL << i)) == 0)
        {
            rtc_gpio_isolate(GPIO_NUM_0 + (i));
        };
    }

// Optional: Disable brownout detector for lower power consumption
#ifdef CONFIG_ESP32_BROWNOUT_DET
    disable_brownout();
#endif
};

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
        snprintf(r1, 17, "Slept %lds", (long)dur);
    }
    if (rtc_mem.last_error)
    {
        snprintf(r1, 17, "Recovered err   ");
        sys_state.error.error_flags |= rtc_mem.last_error;
    }

    lcd_flash_info(r0, r1, 3000);
    vTaskDelay(pdMS_TO_TICKS(3000));
}

/* ── adjust_calibration_setting() ───────────────────────────────────────── */
void adjust_calibration_setting(button_event_info_t btn)
{
    static uint8_t calib_step = 0;
    button_id_t button_id = gpio_to_button_id(btn.button_id);

    switch (calib_step)
    {
    case 0:
        lcd_show_menu("Calibration Menu", "1.Bat 2.Current ");
        if (button_id == BTN_ENTER)
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
        if (button_id == BTN_ENTER)
        {
            float known = 12.0f;
            sys_state.inverter.battery_voltage_calibration =
                known - sys_state.inverter.battery.voltage;
            sys_state.inverter.battery.voltage +=
                sys_state.inverter.battery_voltage_calibration;
            save_settings();
            lcd_flash_info("Calibration Done", "                ", 1000);
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

bool system_is_inactive()
{
    // 1. check for recent user input
    if (xTaskGetTickCount() - sys_state.flags.last_user_activity < pdMS_TO_TICKS(DISPLAY_TIMEOUT * 1000))
    {
        return false;
    }
    // 2. Check for power events
    if (xTaskGetTickCount() - sys_state.flags.last_power_event < pdMS_TO_TICKS(DISPLAY_TIMEOUT * 1000))
    {
        return false;
    }
    // 3. Check if system is in active mode
    if (sys_state.inverter.inverter_active || sys_state.inverter.connected)
    {
        return false;
    }
    // 4. If we get here, system is inactive
    return true;
}

// ================== UPDATED INPUT HANDLER ==================
void update_activity()
{
    // Update last user activity timestamp
    if (system_is_inactive())
    {
        // If system is inactive, do not update last activity
        return;
    }
    sys_state.flags.last_user_activity = xTaskGetTickCount();
    // Turn display on if it was off
    if (!sys_state.display.display_on)
    {
        sys_state.display.display_on = true;
        LCD_power(true);
        update_led(LED_STATUS, 100); // Full brightness
    }
}

// ================== DISPLAY TIMEOUT TASK ==================
void display_timeout_task(void *arg)
{
    while (1)
    {
        // Turn off display after timeout

        if (sys_state.display.display_on &&
            xTaskGetTickCount() - sys_state.flags.last_user_activity > pdMS_TO_TICKS(DISPLAY_TIMEOUT * 1000))
        {
            sys_state.display.display_on = false;
            LCD_power(false);
            update_led(LED_STATUS, 0); // Turn off LED
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); // Check every second
    }
}

// Initialize LCD power control (call once at startup)
void lcd_power_init()
{
    // Configure power control GPIO
    gpio_config_t pwr_conf = {
        .pin_bit_mask = (1ULL << LCD_PWR_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE};
    gpio_config(&pwr_conf);

    // Configure PWM backlight
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_PWM_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = LCD_PWM_FREQ};
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t ch_conf = {
        .gpio_num = LCD_BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_PWM_CHANNEL,
        .timer_sel = LEDC_TIMER_0};
    ledc_channel_config(&ch_conf);
}

// Control LCD power (true = on, false = off)
void LCD_power(bool enable)
{
    if (enable)
    {
        // Power sequence: Enable LCD first, then backlight
        gpio_set_level(LCD_PWR_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(10));                            // Short delay for LCD to stabilize
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_PWM_CHANNEL, 128); // 50% brightness
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_PWM_CHANNEL);
    }
    else
    {
        // Power sequence: Disable backlight first, then LCD
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_PWM_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_PWM_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(LCD_PWR_GPIO, 0);
    }
}

// Optional: Set backlight brightness (0-255)
void lcd_set_brightness(uint8_t brightness)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_PWM_CHANNEL, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_PWM_CHANNEL);
}

static bool is_valid_error_code(uint8_t error)
{
    switch ((system_errors_t)error)
    {
    case ERR_NONE:
    case ERR_OVER_TEMP:
    case ERR_OVERLOAD:
    case ERR_BATTERY_VOLTAGE:
    case ERR_LOW_BAT:
    case ERR_UNDER_VOLTAGE:
    case ERR_OVER_VOLTAGE:
    case ERR_INVERTER_VOLTAGE:
    case ERR_AC_FAULT:
    case ERR_FAN_FAIL:
    case ERR_EEPROM:
    case ERR_HIGH_BAT:
    case ERR_SHORT_CIRCUIT:
    case ERR_SYSTEM_FAILURE:
    case ERR_OVER_UNDER_VOLTAGE:
        return true;

    default:
        return false;
    }
}

void init_system_state()
{

    // ✅ STEP 1: Clear system state
    memset(&sys_state, 0, sizeof(sys_state));

    // ✅ STEP 1.5: Bring up the graduated protection state machine before
    // anything else touches sys_state.error.error_flags. Thresholds get
    // re-synced to the actual battery profile once it's loaded below.
    if (!protection_init())
    {
        ESP_LOGE(TAG_SYS, "FATAL: protection_init failed");
    }

    // ✅ STEP 2: Initialize safe defaults
    sys_state.inverter.temperature = 25.0f; // Safe room temperature
    sys_state.inverter.inverter_active = false;
    sys_state.inverter.connected = false;

    /* System flags */
    sys_state.system_ready = false;
    sys_state.system_active = false;
    sys_state.output_enabled = false;
    sys_state.calibration_valid = false;
    sys_state.adc_ready = false;
    sys_state.hold_start_time = 0;

    /* Battery defaults */
    sys_state.battery_voltage_system = 12.0f;
    sys_state.battery_cutoff = 11.05f;
    sys_state.sound_enabled = true;
    sys_state.low_battery = false;

    // Initialize system parameters with default values
    sys_state.inverter.battery.voltage = 0.0f;
    sys_state.inverter.output_voltage = 230.0f;
    sys_state.inverter.inverter_output_voltage = 0.0f;
    sys_state.inverter.fan_voltage = 0;
    sys_state.inverter.output_frequency = 50.0f;
    sys_state.inverter.over_under_voltage = 0.0f;
    sys_state.temperature_limit = 60.0f;
    sys_state.current_limit = 30.0f;
    sys_state.temperature_limit = 70.0f;
    sys_state.cutoff_voltage = 48.0f;

    // Initialize value adjustment context
    sys_state.current_value_type = NULL;
    sys_state.value_edit_mode = false;
    sys_state.value_changed = false;
    sys_state.pending_confirmation = false;
    sys_state.repeat_count = 0;
    sys_state.fast_increment_active = false;

    /* UI / menu */
    sys_state.menu_state = MAIN_MENU;
    sys_state.menu_selection = 0;
    sys_state.in_detail_view = false;
    sys_state.safety_conditions_met = true; // Set based on actual conditions
    sys_state.last_activity_time = 0;
    sys_state.power_button_sequence_count = 0;

    /* Error handling - SAFE initialization */
    sys_state.error.error_flags = 0; // Clear all flags
    sys_state.fault_flags = 0;
    sys_state.error_count = 0;

    /* Timing */
    sys_state.last_activity_time = esp_timer_get_time();
    sys_state.flags.last_user_activity = xTaskGetTickCount();
    sys_state.flags.last_power_event = xTaskGetTickCount();

    /* Display */
    sys_state.display.display_on = true;
    sys_state.display.scroll_enabled = false;
    sys_state.display.scroll_speed = DEFAULT_SCROLL_SPEED;

    /* Output voltage and current */
    sys_state.inverter.output_voltage = 230.0f;
    sys_state.inverter.output_current = 0.0f;

    // ✅ STEP 0: CHECK IF COLD BOOT AND CLEAR RTC FIRST
    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();

    ESP_LOGI(TAG_SYS, "Wakeup cause: %d", wakeup_cause);
    if (wakeup_cause != ESP_SLEEP_WAKEUP_UNDEFINED &&
        is_valid_error_code(rtc_mem.last_error))
    {
        ESP_LOGW(TAG_SYS, "⚠️ Restoring error from RTC: 0x%02X", rtc_mem.last_error);
        sys_state.error.error_flags = (system_errors_t)rtc_mem.last_error;
    }
    else
    {
        // Wake from sleep
        ESP_LOGI(TAG_SYS, "🔵 Wake from sleep (cause: %d)", wakeup_cause);
        rtc_mem.wake_count++;
    }

    // ✅ FINAL: Only keep persistent error flags if any
    sys_state.error.error_flags &= (ERR_EEPROM | ERR_FAN_FAIL);
}

/* ── handle_critical_error() ─────────────────────────────────────────────── */
void handle_critical_error(void)
{
    ESP_LOGE(TAG_ERROR, "Critical Error: 0x%02X", sys_state.error.error_flags);
    log_error_to_nvs(sys_state.error.error_flags);
    post_buzzer_event(false);
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
    else if (sys_state.error.error_flags & ERR_UNDER_VOLTAGE)
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

void register_task_to_wdt(TaskHandle_t task)
{
    if (esp_task_wdt_add(task) != ESP_OK)
    {
        ESP_LOGE("WDT", "Failed to add task %p to watchdog", task);
    }
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

// =============== SYSTEM RESTART IMPLEMENTATION ===============
_Noreturn void system_restart(void)
{ // C11 standard syntax
    esp_restart();
    __builtin_unreachable(); // GCC/Clang intrinsic
}

/* ── perform_system_restart() ───────────────────────────────────────────── */
void perform_system_restart(bool factory_reset)
{
    lcd_watchdog_deinit();
    lcd_flash_info("System Restart  ", "Please wait...  ", 500);
    post_buzzer_event(false);
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
    snprintf(v, 17, "%s %dV  ",
             profile.name_prefix,
             (int)sys_state.battery_profile.nominal_voltage);
    lcd_show_monitor_detail(l, v);
}

// Re-enable brownout detector (call after wakeup)
void enable_brownout()
{
    // Standard configuration (2.43V threshold)
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA | RTC_CNTL_BROWN_OUT_PD_RF_ENA | (7 << RTC_CNTL_DBROWN_OUT_THRES_S));
}
// Disable brownout detector (call before deep sleep)
void disable_brownout()
{
    // For ESP32, ESP32-S2, ESP32-S3, ESP32-C3
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

// Alternative method (ESP-IDF 4.4+)
#ifdef CONFIG_ESP32_BROWNOUT_DET
    CLEAR_PERI_REG_MASK(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA);
#endif
}

void init_watchdog(bool enable_task_wdt, bool panic_on_hang)
{
    if (enable_task_wdt)
    {
        esp_task_wdt_config_t twdt_config = {
            .timeout_ms = 5000,                              // 5-second timeout
            .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, // Monitor all available cores
            .trigger_panic = panic_on_hang};

        esp_err_t err = esp_task_wdt_init(&twdt_config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE("WDT", "Failed to init task watchdog: %s", esp_err_to_name(err));
        }

        // Subscribe current task to the watchdog
        err = esp_task_wdt_add(NULL);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE("WDT", "Failed to add task to watchdog: %s", esp_err_to_name(err));
        }
    }

#ifdef CONFIG_ESP_INT_WDT
    // Interrupt WDT config can go here if needed in future.
    // The CONFIG_ESP_INT_WDT macro only exists if enabled in menuconfig.
#endif
}

void log_error_to_nvs(uint8_t error_code)
{
    nvs_handle_t nvs_handler;
    esp_err_t err = nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &nvs_handler); // Use default NVS

    if (err == ESP_OK)
    {
        uint32_t error_count = 0;
        nvs_get_u32(nvs_handler, "count", &error_count);

        char key[15];
        snprintf(key, sizeof(key), "err_%04lu", error_count % 1000);
        nvs_set_u8(nvs_handler, key, error_code);

        nvs_set_u32(nvs_handler, "count", error_count + 1);
        nvs_commit(nvs_handler);
        nvs_close(nvs_handler);
    }
    else
    {
        ESP_LOGE("NVS", "Failed to open error_log namespace: %s", esp_err_to_name(err));
    }
}

// used for adc sampling 10x
void filter_init(MovingAverageFilter *f)
{
    memset(f->buffer, 0, sizeof(f->buffer));
    f->sum = 0;
    f->index = 0;
}

float filter_update(MovingAverageFilter *f, float new_value)
{
    // Remove oldest value
    f->sum -= f->buffer[f->index];

    // Add new value
    f->buffer[f->index] = new_value;
    f->sum += new_value;

    // Update index
    f->index = (f->index + 1) % FILTER_DEPTH;

    return f->sum / FILTER_DEPTH;
}

void log_error_state()
{
    if (sys_state.error.error_flags & ERR_OVER_TEMP)
        ESP_LOGE("ERROR", "Over temperature!");
    if (sys_state.error.error_flags & ERR_OVERLOAD)
        ESP_LOGE("ERROR", "Overload detected!");
    if (sys_state.error.error_flags & ERR_LOW_BAT)
        ESP_LOGE("ERROR", "Low battery!");
    if (sys_state.error.error_flags & ERR_HIGH_BAT)
        ESP_LOGE("ERROR", "High battery!");
    if (sys_state.error.error_flags & ERR_FAN_FAIL)
        ESP_LOGE("ERROR", "Fan failure!");
};

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

    // Get the actual flags value to display
    uint8_t flags = sys_state.error.error_flags;
    char code_str[16];
    snprintf(code_str, 16, "Code: 0x%02X    ", flags);

    bool error_found = false;
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
    {
        if (sys_state.error.error_flags == errors[i].flag)
        {
            lcd_show_fault(errors[i].line1, errors[i].line2);
            post_buzzer_event(true);
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
                    shutdown_inverter();
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
                    shutdown_inverter();
                    sys_state.display.current_menu = MAIN_MENU;
                    fan_fail_started = false;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    if (!error_found)
    {
        // Instead of generic "Unknown", show the actual hex code
        lcd_show_fault("Error Detected  ", code_str);
        post_buzzer_event(true);
    }
    // Log the error state for debugging
    log_all_error_flags(sys_state.error.error_flags);
    lcd_show_confirm("Press BACK to   ", "Reset system... ");

    uint32_t elapsed = 0;
    while (elapsed < RESET_TIMEOUT_MS)
    {
        if (gpio_get_level(BTN_ENTER) == 0)
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

// Value editing functions
void edit_voltage_threshold(void)
{
    sys_state.current_value_type = &value_edit[VALUE_TYPE_VOLTAGE];
    sys_state.edit_backup_value = sys_state.current_value_type->current_value;
    sys_state.pending_confirmation = true;
    sys_state.value_changed = false;
    sys_state.repeat_count = 0;
    sys_state.fast_increment_active = false;
    sys_state.value_edit_mode = true;
    lcd_show_value_edit_screen();
}

void edit_current_limit(void)
{
    sys_state.edit_backup_value = sys_state.current_value_type->current_value;
    sys_state.pending_confirmation = true;
    sys_state.value_changed = false;
    sys_state.value_edit_mode = true;
    sys_state.current_value_type = &value_edit[VALUE_TYPE_CURRENT];
    lcd_show_value_edit_screen();
}

void edit_frequency_range(void)
{

    sys_state.edit_backup_value = sys_state.current_value_type->current_value;
    sys_state.pending_confirmation = true;
    sys_state.value_changed = false;
    sys_state.repeat_count = 0;
    sys_state.fast_increment_active = false;
    sys_state.value_edit_mode = true;
    sys_state.current_value_type = &value_edit[VALUE_TYPE_FREQUENCY];
    lcd_show_value_edit_screen();
}

void edit_temperature_alarm(void)
{
    sys_state.edit_backup_value = sys_state.current_value_type->current_value;
    sys_state.pending_confirmation = true;
    sys_state.value_changed = false;
    sys_state.repeat_count = 0;
    sys_state.fast_increment_active = false;
    sys_state.value_edit_mode = true;
    sys_state.current_value_type = &value_edit[VALUE_TYPE_TEMPERATURE];
    lcd_show_value_edit_screen();
}

void edit_system_timeout(void)
{
    sys_state.edit_backup_value = sys_state.current_value_type->current_value;
    sys_state.pending_confirmation = true;
    sys_state.value_changed = false;
    sys_state.repeat_count = 0;
    sys_state.fast_increment_active = false;
    sys_state.value_edit_mode = true;
    sys_state.current_value_type = &value_edit[VALUE_TYPE_TIMEOUT];
    lcd_show_value_edit_screen();
}

void edit_auto_shutdown(void)
{
    sys_state.edit_backup_value = sys_state.current_value_type->current_value;
    sys_state.pending_confirmation = true;
    sys_state.value_changed = false;
    sys_state.repeat_count = 0;
    sys_state.fast_increment_active = false;
    sys_state.value_edit_mode = true;
    sys_state.current_value_type = &value_edit[VALUE_TYPE_AUTO_SHUTDOWN];
    sys_state.current_value_type->current_value =
        sys_state.display.auto_shutdown_enabled ? 1.0f : 0.0f;
    lcd_show_value_edit_screen();
}

void edit_scroll_enable(void)
{
    sys_state.edit_backup_value = sys_state.current_value_type->current_value;
    sys_state.pending_confirmation = true;
    sys_state.value_changed = false;
    sys_state.repeat_count = 0;
    sys_state.fast_increment_active = false;
    sys_state.value_edit_mode = true;
    sys_state.current_value_type = &value_edit[VALUE_TYPE_SCROLL_ENABLE];
    sys_state.current_value_type->current_value =
        sys_state.display.scroll_enabled ? 1.0f : 0.0f;
    lcd_show_value_edit_screen();
}

void edit_sound_enable(void)
{
    sys_state.edit_backup_value = sys_state.current_value_type->current_value;
    sys_state.pending_confirmation = true;
    sys_state.value_changed = false;
    sys_state.repeat_count = 0;
    sys_state.fast_increment_active = false;
    sys_state.value_edit_mode = true;
    sys_state.current_value_type = &value_edit[VALUE_TYPE_SOUND_ENABLE];
    sys_state.current_value_type->current_value = sys_state.sound_enabled ? 1.0f : 0.0f;
    lcd_show_value_edit_screen();
}

void edit_scroll_speed(void)
{
    sys_state.edit_backup_value = sys_state.current_value_type->current_value;
    sys_state.pending_confirmation = true;
    sys_state.value_changed = false;
    sys_state.repeat_count = 0;
    sys_state.fast_increment_active = false;
    sys_state.value_edit_mode = true;
    sys_state.current_value_type = &value_edit[VALUE_TYPE_SCROLL_SPEED];
    sys_state.current_value_type->current_value = (float)sys_state.display.scroll_speed;
    lcd_show_value_edit_screen();
}

void edit_battery_type(void)
{
    sys_state.edit_backup_value = sys_state.current_value_type->current_value;
    sys_state.pending_confirmation = true;
    sys_state.value_changed = false;
    sys_state.repeat_count = 0;
    sys_state.fast_increment_active = false;
    sys_state.value_edit_mode = true;
    sys_state.current_value_type = &value_edit[VALUE_TYPE_BATTERY_TYPE];
    sys_state.current_value_type->selection_index = sys_state.battery_profile.profile_id;
    lcd_show_value_edit_screen();
}

void edit_battery_voltage_system(void)
{
    sys_state.edit_backup_value = sys_state.current_value_type->current_value;
    sys_state.pending_confirmation = true;
    sys_state.value_changed = false;
    sys_state.repeat_count = 0;
    sys_state.fast_increment_active = false;
    sys_state.value_edit_mode = true;
    sys_state.current_value_type = &value_edit[VALUE_TYPE_BATTERY_VOLTAGE_SYSTEM];
    sys_state.current_value_type->selection_index =
        voltage_system_to_index((voltage_system_t)sys_state.battery_profile.nominal_voltage);
    lcd_show_value_edit_screen();
}

void security_pin(void)
{
}

/*==============================================================================
  lcd_task — THE ONLY FUNCTION THAT CALLS lcd_* HARDWARE FUNCTIONS
  Implementation lives in lcd_task.c (already written above).
  Declaration here for linker:
==============================================================================*/
extern void lcd_task(void *arg); /* defined in lcd_task.c */

esp_err_t nvs_get_float(const char *key, float *out_value)
{
    nvs_handle_t handle;
    esp_err_t err;
    uint32_t temp_value;

    // Open NVS
    err = nvs_open(NVS_NS_SYSTEM, NVS_READONLY, &handle);
    if (err != ESP_OK)
        return err;

    // Read as uint32_t
    err = nvs_get_u32(handle, key, &temp_value);
    if (err == ESP_OK)
    {
        // Convert uint32_t to float
        memcpy(out_value, &temp_value, sizeof(float));
    }

    nvs_close(handle);
    return err;
}

esp_err_t nvs_set_float(const char *key, float value)
{
    nvs_handle_t handle;
    esp_err_t err;
    uint32_t temp_value;

    // Open NVS
    err = nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;

    // Convert float to uint32_t
    memcpy(&temp_value, &value, sizeof(float));

    // Write as uint32_t
    err = nvs_set_u32(handle, key, temp_value);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

/*==============================================================================
  app_main — unchanged except lcd_writer_init() added
==============================================================================*/
void app_main(void)
{

    system_events_init();

    event_dispatcher_init();

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

    /* ── NEW: initialise render state ─────────────────────────────────── */
    lcd_writer_init();

    init_hardware();
    nvs_init(false);
    security_init();
    init_system_state();
    init_menu_system();
    fault_log_init();
    nvs_print_stats();
    if (nvs_is_initialized())
        ESP_LOGI("MAIN", "NVS ready");

    restore_from_deep_sleep();
    log_all_error_flags(sys_state.error.error_flags);
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
    // lcd_show_boot_brand();
    xTaskCreate(adc_task, "adc_task", 4096, NULL, 5, NULL);
    xTaskCreate(lcd_task, "lcd_task", 4096, NULL, 4, &lcd_task_handle);
    xTaskCreatePinnedToCore(event_dispatcher_task, "dispatcher", 4096, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(buzzer_event_task, "buzzer_evt", 2048, NULL, 7, NULL, 1);
    xTaskCreatePinnedToCore(led_event_task, "led_evt", 2048, NULL, 7, NULL, 1);
    xTaskCreatePinnedToCore(fault_log_event_task, "logger_evt", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(monitor_event_task, "monitor_evt", 3072, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(protection_event_task, "prot_evt", 4096, NULL, 9, NULL, 0);

    lcd_watchdog_init(lcd_task_handle);
    while (sys_state.system_ready)
    {
        update_lcd_activity_state();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(APP_TAG, "Main loop ended");
    cleanup_button_system();
}