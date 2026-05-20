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
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/event_groups.h"

#include "sdkconfig.h"
#include "rom/ets_sys.h"
#include "utils.h" // Added MIN & MAX
#include <stdbool.h>
#include <button_controller.h>

// WIFI Credentials
#include "esp_wifi.h"
#define WIFI_SSID "johnson"
#define WIFI_PASS "internet"
#define WEATHER_API_KEY "YOUR_OPEN_WEATHER_API_KEY"
#define CITY_NAME "Lagos"
#define WEATHER_CHECK_INTERNAL_MS 60000

// =============== SYSTEM CONFIGURATION ===============
#define FIRMWARE_VERSION "v1.0.3" // Set your current firmware version here

// Hardware Configuration
#define LCD_ADDR 0x27
#define LCD_COLS 16
#define LCD_ROWS 2
#define SCROLL_DELAY_MS 300
#define ANIM_DELAY_MS 80

#define SDA_PIN 21
#define SCL_PIN 22
#define BUTTON_DEBOUNCE_TIME_MS 200 // Debounce time for button presses
// System Configuration
#define CONFIG_USE_ADC 1
#define CONFIG_USE_BUTTONS 1
#define CONFIG_USE_LCD 1
#define CONFIG_USE_LED_PWM 1
#define CONFIG_USE_DEEP_SLEEP 0
#define CONFIG_USE_DISPLAY_TIMEOUT_TASK 1

// watchdog
// #define CONFIG_ESP_INT_WDT

SemaphoreHandle_t sys_state_mutex;
TaskHandle_t lcd_task_handle = NULL;

// ADC Handles
#define USE_ADC2
#define FILTER_DEPTH 10 // for sampling adc channels 10x

// adc configuration parameters
#define TAG_ADC "ADC_INIT"
#define ADC_UNIT_1 ADC_UNIT_1
#define ADC_UNIT_USED ADC_UNIT_1
#define ADC_ATTEN_USED ADC_ATTEN_DB_12
#define ADC_BIT_WIDTH_USED ADC_BITWIDTH_DEFAULT

// Configuration constants
#define ADC_CHANNEL_MAX 5 // Adjust based on your ADC
#define VOLTAGE_DIVIDER_RATIO ((R1_DIVIDER + R2_DIVIDER) / (float)R2_DIVIDER)

// Nvs Error Codes
#define ADC_ERROR_CODE 0xFD
#define WATCHDOG_ERROR_CODE 0xFE
#define STACK_OVERFLOW_ERROR_CODE 0xFC
#define PERSISTENT_ERROR_CODE 0xFF

// Assumed GPIO and enum constants
#define GPIO_BTN_UP GPIO_NUM_17    // BLUE BUTTON IN WOKWI
#define GPIO_BTN_DOWN GPIO_NUM_5   // BLACK  BUTTON IN WOKWI
#define GPIO_BTN_ENTER GPIO_NUM_19 // GREY BUTTON IN WOKWI
#define GPIO_BTN_BACK GPIO_NUM_18  // YELLOW BUTTON IN WOKWI
#define GPIO_PWR_BTN GPIO_NUM_0    // RED BUTTON IN WOKWI REMEMBER TO CHANGE IT TO GPIO_NUM_1. IT'S SO IN INVERTER SYSTEM
#define GPIO_BUZZER GPIO_NUM_13
#define GPIO_STATUS_LED GPIO_NUM_14 // GREEN LED BOARD 2
// DEMO FOR STATUS LED
#define GPIO_ERROR_LED GPIO_NUM_26
#define GPIO_POWER_RELAY GPIO_NUM_12

#define GPIO_NEPA_INPUT GPIO_NUM_22
#define GPIO_FAN GPIO_NUM_33

// FAN TEST
#define GPIO_FAN_TEST GPIO_NUM_4      // MOSFET control pin
#define FAN_DISCONNECTED_THRESHOLD 50 // ADC value threshold
#define FAN_DISCONNECT_RETRIES 3      // Failures before error
#define FAN_CHECK_INTERVAL_MS 10000   // 10 seconds

// CONSTANTS FOR INVERTER PROTECTION LIMITS
// Voltage limits
#define DC_VOLTAGE_MIN 10.0f    // Volts
#define DC_VOLTAGE_MAX 60.0f    // Volts (adjust for your system)
#define AC_VOLTAGE_MIN 200.0f   // Volts
#define AC_VOLTAGE_MAX 250.0f   // Volts
#define GRID_VOLTAGE_MIN 200.0f // Volts
#define GRID_VOLTAGE_MAX 250.0f // Volts

// Frequency limits
#define GRID_FREQ_MIN 49.5f // Hz
#define GRID_FREQ_MAX 50.5f // Hz (or 59.5-60.5 for 60Hz systems)

// Current limits
#define DC_CURRENT_MAX 100.0f // Amps
#define AC_CURRENT_MAX 50.0f  // Amps

// Temperature limits
#define HEATSINK_TEMP_MAX 85.0f     // °C
#define TRANSFORMER_TEMP_MAX 100.0f // °C
#define AMBIENT_TEMP_MAX 50.0f      // °C
#define AMBIENT_TEMP_MIN -10.0f     // °C
#define FAN_START_TEMP 60.0f        // °C

// Protection limits
#define DC_INJECTION_MAX 0.5f          // % of rated voltage
#define INSULATION_RESISTANCE_MIN 1.0f // MΩ
#define DC_BUS_IMBALANCE_MAX 5.0f      // Volts
#define POWER_FACTOR_MIN 0.7f

// Battery limits
#define BATTERY_SOC_MIN 20.0f    // %
#define BATTERY_HEALTH_MIN 70.0f // %

// Timing
#define MIN_OFF_TIME_MS 5000 // milliseconds

// Fault flags (bit definitions)
#define FAULT_OVERCURRENT (1 << 0)
#define FAULT_SHORT_CIRCUIT (1 << 1)
#define FAULT_GROUND_FAULT (1 << 2)
#define FAULT_ARC_DETECTED (1 << 3)
#define FAULT_GATE_DRIVER (1 << 4)
#define FAULT_SENSOR_COMM (1 << 5)
#define FAULT_CAN_COMM (1 << 6)
#define FAULT_REVERSE_POLARITY (1 << 7)
#define FAULT_WATCHDOG (1 << 8)

// Mutex Constant
#define SYS_STATE_MUTEX_TIMEOUT_MS 100

// static int fan_disconnect_count = 0;
// static bool fan_connected_last_state = true;
// static const char *TAG = "FAN_MONITOR";

// Queue for button events
QueueHandle_t button_event_queue;

// Inverter system states
typedef enum
{
    INVERTER_OFF,
    INVERTER_STARTING,
    INVERTER_ON,
    INVERTER_STANDBY,
    INVERTER_FAULT,
    INVERTER_DIAGNOSTIC,
    INVERTER_FACTORY_RESET
} inverter_state_t;

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

// System Constants
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

// Protection Thresholds
#define MAX_TEMPERATURE 85.0f
#define MAX_CURRENT 25.0f
#define LOW_BATTERY_VOLTAGE_THRESHOLD 10.5f
#define HIGH_BATTERY_VOLTAGE 14.8f
#define UNDER_VOLTAGE_THRESHOLD 160.0f
#define OVER_VOLTAGE_THRESHOLD 260.0f
#define BATTERY_VOLTAGE_THRESHOLD 12.0f          // Minimum battery voltage for operation
#define INVERTER_OUTPUT_VOLTAGE_THRESHOLD 220.0f // Minimum inverter output voltage
#define FAN_SPEED_THRESHOLD 2.0f                 // Minimum fan speed voltage threshold
#define FAN_SPEED_MAX 5.0f                       // Maximum fan speed voltage threshold
#define FAN_SPEED_MIN 0.5f                       // Minimum fan speed voltage threshold]

// Error codes definition
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

// Nvs
#define NVS_NS_SYSTEM "inv_sys_v2"
static bool nvs_initialized = false;

// Thresholds (in seconds)
#define DISPLAY_TIMEOUT 300 // 5 minutes
#define SLEEP_TIMEOUT 1800  // 30 minutes

// Hardware Configuration (modify for your setup)
#define LCD_PWR_GPIO GPIO_NUM_27 // Safer alternative to GPIO12
#define LCD_BL_GPIO GPIO_NUM_13  // PWM control for backlight
#define LCD_PWM_CHANNEL LEDC_CHANNEL_0
#define LCD_PWM_FREQ 5000            // 5kHz
#define LCD_PWM_RES LEDC_TIMER_8_BIT // 8-bit resolution (0–255)

// Error Codes

// Alert tone for buzzer
#define ALERT_TONE_FREQ 500
#define ALERT_TONE_VOLUME 200

// wakeUp button
#define WAKEUP_BUTTON_1 GPIO_BTN_ENTER // Replace with your GPIO
#define WAKEUP_BUTTON_2 GPIO_BTN_BACK  // Replace with your GPIO

// Bit mask of the GPIOs to wake up on
const uint64_t wakeup_pin_mask = (1ULL << WAKEUP_BUTTON_1) | (1ULL << WAKEUP_BUTTON_2);

#define tag "LCD"

typedef enum
{
    ERR_NONE = 0x00,
    ERR_OVER_TEMP = 0x01,
    ERR_OVERLOAD = 0x02,
    ERR_BATTERY_VOLTAGE = 0x03,
    ERR_LOW_BAT = 0x04,
    ERR_UNDER_VOLTAGE = 0x05,
    ERR_OVER_VOLTAGE = 0x06,
    ERR_INVERTER_VOLTAGE = 0x07,
    // Additional error codes
    ERR_AC_FAULT = 0x08,
    ERR_FAN_FAIL = 0x10,
    ERR_EEPROM = 0x20,
    ERR_HIGH_BAT = 0x40,
    ERR_SHORT_CIRCUIT = 0x80,
    ERR_SYSTEM_FAILURE = 0x90,
    ERR_OVER_UNDER_VOLTAGE = 0x50,
} system_errors_t;

// for battery recharge menu
#define BUTTON_NONE -1

// for battery handle recharge menu
#define BATTERY_MENU_COUNT 3
#define BATTERY_PROFILE_VERSION 1
#define BATTERY_TYPE_KEY "battery_type"
#define BATTERY_CAPACITY_KEY "bat_capacity"
#define NVS_VOLTAGE_KEY_PREFIX "voltage_"
#define BATTERY_VOLTAGE_SYSTEM_KEY "inv_bat_volt"
#define BATTERY_CAPACITY_AH "bat_capacity_ah"

// Default battery system
#define DEFAULT_BATTERY_PROFILE BATTERY_CHEMISTRY_LITHIUM_ION
#define FREQUENCY_SETTING_KEY "frequency"
// static bool editing_battery_menu = false;
//  end

// battery submenu started here
typedef enum
{
    BATTERY_CUTOFF_MENU_MAIN,
    BATTERY_CUTOFF_MENU_SELECT_TYPE,
    BATTERY_CUTOFF_MENU_SELECT_VOLTAGE,
    BATTERY_CUTOFF_MENU_EDIT_CUTOFF
} battery_cutoff_menu_state_t;

typedef enum
{
    BATTERY_CHEMISTRY_LEAD_ACID = 0,
    BATTERY_CHEMISTRY_AGM,
    BATTERY_CHEMISTRY_GEL,
    BATTERY_CHEMISTRY_LITHIUM_ION,
    BATTERY_CHEMISTRY_LIFEPO4,
    BATTERY_CHEMISTRY_NIMH,
    BATTERY_CHEMISTRY_TYPE_COUNT
} battery_chemistry_t;

typedef enum
{
    VOLTAGE_SYSTEM_12V = 12,
    VOLTAGE_SYSTEM_24V = 24,
    VOLTAGE_SYSTEM_48V = 48,
    VOLTAGE_SYSTEM_96V = 96
} voltage_system_t;

// Battery type enumeration
typedef enum
{
    BATTERY_LEAD_ACID = 0,
    BATTERY_AGM,
    BATTERY_GEL,
    BATTERY_LIFEPO4,
    BATTERY_LITHIUM_ION,
    BATTERY_NIMH,
    BATTERY_TYPE_COUNT
} battery_type_t;

// Base profiles for 12V system (all other voltages are multiples of these)
typedef struct
{
    char name_prefix[8]; // e.g., "Lead-Acid"
    battery_chemistry_t chemistry;
    float depth_of_discharge_max;

    battery_type_t profile_id;
    voltage_system_t nominal_voltage;
    float capacity_ah;
    uint16_t usable_capacity_ah;

    // 12V reference voltages (will be scaled)
    float bulk_charge_voltage_12v;
    float float_charge_voltage_12v;
    float equalization_voltage_12v;
    float full_charge_voltage_12v;
    float nominal_voltage_actual_12v;
    float low_voltage_warning_12v;
    float low_voltage_alarm_12v;
    float cutoff_voltage_12v;
    float cutoff_voltage_min_12v;
    float high_battery_voltage_12v;
    float recharge_voltage_12v;
    float overvoltage_protection_12v;
    float undervoltage_protection_12v;

    // Current per 100Ah (will be scaled by capacity)
    float max_charge_current_per_100ah;
    float max_discharge_current_per_100ah;
    float recommended_charge_current_per_100ah;
    float trickle_charge_current_per_100ah;

    // Temperature parameters (same for all voltages)
    float temp_coefficient;
    float operating_temp_min;
    float operating_temp_max;
    float charge_temp_min;
    float charge_temp_max;
    float discharge_temp_min;
    float discharge_temp_max;

    // Timing parameters (same for all voltages)
    uint16_t bulk_charge_timeout_min;
    uint16_t absorption_time_min;
    uint16_t float_time_min;
    uint16_t equalization_time_min;
    uint8_t equalization_interval_days;

    // SOC parameters (same for all voltages)
    float soc_full_threshold;
    float soc_empty_threshold;
    float soc_low_warning;

    // Internal resistance per cell (will be scaled)
    float internal_resistance_mohm_12v;

    // Cycle life
    uint16_t cycle_life_rated;
    float cycle_life_dod;

    // Self-discharge (same for all voltages)
    float self_discharge_rate;

    // Charge termination
    float charge_termination_current_per_100ah;
    float charge_termination_voltage_12v;
    float charge_termination_timeout;

    // Balancing
    bool requires_balancing;
    float balance_start_voltage_12v;
    float balance_voltage_delta_max;

    // Safety features
    bool has_bms;
    bool requires_external_bms;
    bool supports_temperature_comp;

    // Efficiency
    float charge_efficiency;
    float discharge_efficiency;

    // Maintenance
    bool requires_equalization;
    bool is_sealed;
    uint16_t maintenance_interval_days;

} battery_profile_t;

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

    printf("Battery configuration saved successfully!\n");
    printf("  Type: %d, Voltage: %dV, Capacity: %dAh\n",
           battery_type, voltage_system, capacity_ah);

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

// Call this to display and handle selection
void select_battery_type(button_id_t btn)
{
    static battery_chemistry_t selected = BATTERY_AGM;
    static bool updated = true; // Force initial display update
    char display_char[20];
    // Handle button input
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
        esp_err_t err = battery_save_configuration((battery_type_t)selected,
                                                   VOLTAGE_SYSTEM_48V,
                                                   200); // Example: 48V, 200Ah
        lcd_clear();
        if (err == ESP_OK)
        {
            snprintf(display_char, sizeof(display_char), "Saved: %s", battery_type_names[selected]);
            lcd_print(display_char);
        }
        else
        {
            lcd_print("Save Failed!");
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
        updated = true; // Refresh menu after message
        break;
    }
    default:
        // No action
        break;
    }

    // Update LCD only if something changed or first run
    if (updated)
    {
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_print("Select Battery:");

        lcd_set_cursor(1, 0);
        snprintf(display_char, sizeof(display_char), "> %s", battery_type_names[selected]);
        lcd_print(display_char);
        updated = false;
    }
}

typedef struct
{
    float voltage;             // Current battery voltage
    float voltage_filtered;    // Filtered voltage (smooth out spikes)
    float battery_temperature; // Battery temp
    float battery_soc;         // State of Charge in percentage
    bool is_low;               // Below warning threshold
    bool is_critical;          // Below cutoff threshold
    uint32_t low_count;        // Consecutive low readings (debouncing)
    uint32_t critical_count;   // Consecutive critical readings
    uint32_t last_reading_time;
    TickType_t battery_last_update_tick;
} battery_state_t;

typedef struct
{
    inverter_state_t inverter_state;
    inverter_state_t previous_inverter_state;
    float temperature;
    float actual_current;
    float output_frequency;
    float output_voltage; // e.g., 220V
    float low_bat_egs002_signal;
    bool inverter_active;
    uint8_t operating_mode;
    uint8_t load_percentage;

    // Battery System Configuration
    battery_state_t battery;
    bool adc_data_valid;

    float fan_voltage;
    float over_under_voltage;
    float inverter_output_voltage;

    // Hardware status
    float fan_connection;
    bool connected;
    bool low_battery;
    bool overload;
    int battery_voltage_system; // 12, 24, 48, 96
    float output_current;
    bool wifi_enabled;
    float battery_voltage_calibration; // Calibration value for battery voltage
    bool boot_complete;
} inverter_status;

typedef struct
{
    bool connected;
    float control_level;
    float speed; // Fan speed in volts
} fan_status_t;

// Menu system states
typedef enum
{
    MENU_NONE,
    MAIN_MENU,
    MENU_SETTINGS,
    MENU_MONITORING,
    MENU_DIAGNOSTIC,
    MENU_FACTORY_RESET_CONFIRM,
    MENU_WIFI_CONFIG,
    MENU_OUTPUT_VOLTAGE_SETTING,
    MENU_FREQUENCY_SETTING,
    MENU_CURRENT_LIMIT,
    MENU_BATTERY_CUTOFF_SETTINGS,
    MENU_SCROLL_SETTINGS,
    MENU_TEMP_SETTING,
    MENU_SYSTEM_INFO,
    MENU_FACTORY_RESET,
    WIFI_ACTIVATION,
    MENU_COUNT
} menu_state_t;

typedef struct
{
    int32_t brightness;
    int32_t backlight_timeout;
    bool auto_shutdown_enabled;
    bool display_on;
    bool scroll_enabled;
    uint8_t scroll_speed;
    menu_state_t current_menu;
    uint8_t menu_position;
    uint8_t selected_index;
} display_status_t;

typedef struct
{
    system_errors_t error_flags; // Bitmask of active faults (ERR_OVER_TEMP etc.)
    char last_error_msg[32];     // Short description of last fault, always NUL-terminated
                                 // 32 chars fits "Over/Under Voltage\0" with room to spare
                                 // Replaces last_error_log[256] (wasteful) and msg (unsafe ptr)
} system_fault_state_t;

typedef struct
{
    uint8_t error_code;    // Raw error code (ERR_OVER_TEMP etc. cast to uint8_t)
    uint32_t timestamp_ms; // xTaskGetTickCount() * portTICK_PERIOD_MS at log time
    char description[16];  // Human-readable, NUL-terminated, matches last_error_msg width
} error_log_entry_t;

typedef struct
{
    uint32_t last_user_activity;
    uint32_t last_power_event;
} system_flags_t;

// menu editing variable
typedef struct
{
    float temp_value;   // for editing numeric parameters
    uint8_t edit_step;  // current editing step
    bool value_changed; // flag for unsaved changes
} menu_edit_state_t;

typedef enum
{
    VALUE_EDIT_NUMERIC,
    VALUE_EDIT_BOOL,
    VALUE_EDIT_SELECT,
    VALUE_EDIT_LIST,
    VALUE_TYPE_NONE
} value_edit_type_t;

// Value configuration structure
typedef struct
{
    value_edit_type_t edit_type;
    float temp_value;
    float min_value;
    float max_value;
    float step_size;
    bool bool_value;
    int selection_index;
    int max_selection;
    const char *label;
    const char *unit;
    uint8_t decimal_places;
    float increment_precision;
    float increment_small; // Normal increment
    float increment_large; // Fast increment (long press/repeat)
    bool is_critical;      // Requires confirmation for changes
    bool live_update;      // Update system in real-time
    uint16_t current_value;
    uint8_t options[10]; // For select/list types
} value_config_t;

// // Value configuration table
// static value_config_t g_value_configs[] = {
//     [VALUE_TYPE_VOLTAGE] = {
//         .edit_type = VALUE_EDIT_NUMERIC,
//         .min_value = 100.0f,
//         .max_value = 240.000f,
//         .temp_value = 220.0f,
//         .increment_small = 1.0f,
//         .increment_large = 5.0f,
//         .increment_precision = 0.1f,
//         .decimal_places = 1,
//         .unit = "V",
//         .label = "Voltage Thresh",
//         .is_critical = true,
//         .live_update = false},
//     [VALUE_TYPE_FREQUENCY] = {.edit_type = VALUE_EDIT_NUMERIC, .min_value = 45.0f, .max_value = 65.0f, .increment_small = 0.1f, .increment_large = 1.0f, .increment_precision = 0.01f, .decimal_places = 2, .unit = "Hz", .label = "Frequency", .is_critical = true, .live_update = false},
//     [VALUE_TYPE_CURRENT] = {.edit_type = VALUE_EDIT_NUMERIC, .min_value = 1.0f, .max_value = 50.0f, .increment_small = 0.5f, .increment_large = 2.0f, .increment_precision = 0.1f, .decimal_places = 1, .unit = "A", .label = "Current Limit", .is_critical = false, .live_update = true},
//     [VALUE_TYPE_TEMPERATURE] = {.edit_type = VALUE_EDIT_NUMERIC, .min_value = 40.0f, .max_value = 85.0f, .increment_small = 1.0f, .increment_large = 5.0f, .increment_precision = 0.5f, .decimal_places = 1, .unit = "°C", .label = "Temperature Limit", .is_critical = false, .live_update = true},
//     [VALUE_TYPE_BATTERY_VOLTAGE] = {.edit_type = VALUE_EDIT_NUMERIC, .min_value = 10.0f, .max_value = 15.0f, .increment_small = 0.1f, .increment_large = 0.5f, .increment_precision = 0.01f, .decimal_places = 2, .unit = "V", .label = "Battery Cutoff", .is_critical = true, .live_update = false},
//     [VALUE_TYPE_MENU_SELECTION] = {.edit_type = VALUE_EDIT_SELECT, .min_value = 0, .max_value = 0, .temp_value = 0, .unit = "", .label = "Menu Select", .is_critical = false, .live_update = false},
//     [VALUE_TYPE_BLUETOOTH] = {.edit_type = VALUE_EDIT_BOOL, .bool_value = false, .unit = "", .label = "Bluetooth", .is_critical = false, .live_update = true},
//     [VALUE_TYPE_WIFI] = {.edit_type = VALUE_EDIT_BOOL, .selection_index = 0, .bool_value = false, .unit = "", .label = "WiFi", .is_critical = false, .live_update = true}};

typedef struct
{
    bool enabled;
    char ssid[32];
    char password[64];
} wifi_state_t;

typedef struct
{
    float voltage_threshold; // V
    float current_limit;     // A
    float frequency_range;   // Hz
    float temperature_alarm; // °C
    int system_timeout;      // Seconds
} settings_t;

typedef struct
{
    value_edit_type_t edit_type;
    float temp_value;
    float min_value;
    float max_value;
    float step_size;
    bool bool_value;
    int list_index;
    int list_size;
    const char **list;
    bool active;
    int selection_index;
    int max_selection;
    const char *label;
    const char *unit;
    uint8_t decimal_places;
    float increment_precision;
    float increment_small; // Normal increment
    float increment_large; // Fast increment (long press/repeat)
    float current_value;
    const char *options[10]; // For select/list types
    bool is_critical;
    bool live_update;
} value_edit_context_t;

/*------------------------------------------------------------------------------
  DISPLAY STATE
------------------------------------------------------------------------------*/

typedef enum
{
    BOOT_SCREEN_BRAND,
    BOOT_SCREEN_INIT,
    BOOT_SCREEN_MAIN
} boot_screen_t;

typedef struct
{
    boot_screen_t current_screen;
    uint32_t boot_screen_timestamp_ms; // Time to display each boot screen
} boot_screen_config_t;

typedef enum
{
    LCD_SCREEN_NONE = 0,
    LCD_SCREEN_MAIN,        // Main status screen
    LCD_SCREEN_BATTERY,     // Battery details
    LCD_SCREEN_POWER,       // Power consumption
    LCD_SCREEN_DIAGNOSTICS, // System diagnostics
    LCD_SCREEN_ERRORS,      // Error details
    LCD_SCREEN_COUNT        // Total number of screens
} lcd_screen_t;

typedef struct
{
    // Battery
    float battery_voltage;
    bool battery_fresh;
    uint8_t load_percentage;
    float max_power_w; // Maximum power rating (e.g., 1000W)
    inverter_state_t inverter_state;
    bool blink_state;
    lcd_screen_t current_screen;
    lcd_screen_t previous_screen;
    // Fault info
    uint16_t fault_code;
    uint32_t last_update_time;
    uint32_t last_screen_change;
} lcd_display_state_t;

static value_edit_context_t value_edit;

// Final grouped system_state_t
typedef struct
{
    menu_state_t menu_state;
    uint8_t menu_selection;
    uint8_t max_menu_items;
    volatile bool system_ready;
    bool safety_conditions_met;
    int64_t last_activity_time;
    uint32_t power_button_sequence_count;
    int64_t power_sequence_start_time;
    bool in_detail_view;
    bool in_confirmation_screen;
    bool in_info_screen;
    menu_state_t detail_parent_menu;
    int detail_parent_selection;
    inverter_state_t pre_detail_inverter_state;

    // Value adjustment context
    value_edit_context_t current_value_type;
    bool value_edit_mode;
    settings_t settings;
    bool value_changed;
    bool pending_confirmation;
    int64_t last_increment_time;
    uint8_t repeat_count;
    bool fast_increment_active;

    // Status mode
    fan_status_t fan;
    display_status_t display;
    system_fault_state_t error;
    system_flags_t flags;
    lcd_display_state_t lcd_state;
    menu_edit_state_t menu_edit;
    uint8_t actual_temperature;
    uint8_t actual_current;
    bool output_enabled;

    bool system_active;    // Indicates if the system is currently active
    bool calibration_mode; // Indicates if the system is in calibration mode

    // Inverter System Parameters (Voltage, Current, temp, frequency e.t.c)
    inverter_status inverter;

    float temperature_limit; // e.g., 60°C
    float over_under_voltage;
    float current_limit;               // e.g., 30A
    float cutoff_voltage;              // e.g., 48V
    uint8_t battery_voltage_system;    // 12, 24, 48, 96
    battery_profile_t battery_profile; // Current battery profile
    float power_factor;                // pf
    float efficiency;
    uint64_t error_count;
    uint64_t memory_usage;
    uint64_t uptime_hours;
    wifi_state_t wifi;
    uint64_t system_timeout;
    bool calibration_valid;
    bool load_connected;
    volatile bool adc_ready;
    boot_screen_config_t lcd_boot_state;
    bool adc_data_valid;
    uint32_t tick_ms;

    // Battery status
    bool low_battery;
    float battery_temp;
    bool battery_charging;
    float battery_cutoff; // Battery cutoff voltage (V)
    bool precharge_complete;

    // Value editing backup
    float edit_backup_value;
    int selected_ssid_index;

    // Fault management
    uint32_t fault_flags;
    float insulation_resistance;
    float dc_injection;
    float ac_output_current;
    float dc_output_current;
    float dc_input_current;
    float heatsink_temperature;
    float transformer_temperature;
    float ambient_temperature;
    float dc_input_voltage;
    float ac_input_voltage;

    // Safety features
    bool fan_running;
    float dc_component_ac;
    float dc_bus_positive;
    float dc_bus_negative;
    uint32_t time_since_last_shutdown;
    bool emergency_stop_active;
    float last_power_off_time;

} system_state_t;

static system_state_t sys_state;

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

// Global Variables

static nvs_handle_t nvs_handler;

// ================== DEFAULT SETTINGS ==================
// Default settings for the inverter system
// You can define default values here
static const int default_settings[] = {
    48,  // Battery Voltage (e.g., 48V)
    220, // Output Voltage (e.g., 220V)
    30,  // Overload Limit (e.g., 30A)
    60   // Temperature Limit (e.g., 60°C)
};
// end DEFAULT SETTINGS

#define R1 10000.0 // Top resistor in voltage divider (ohms)
#define R2 2000.0  // Bottom resistor in voltage divider (ohms)

#define ADC_CHANNEL ADC1_CHANNEL_0 // ADC channel to read battery voltage
#define ADC_WIDTH ADC_WIDTH_BIT_12 // ADC width (12 bits)
#define ADC_ATTEN ADC_ATTEN_DB_11  // ADC attenuation (11 dB)

#define ADC_CHANNEL_USE 2 // Number of ADC in use
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

typedef struct
{
    uint32_t uptime_seconds;
    float cpu_load;
    float ram_usage;
    float temperature;
    bool system_ok;
    char last_error[32];
} diagnostic_data_t;

diagnostic_data_t diag_data;

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
    {"Factory Reset", MENU_FACTORY_RESET_CONFIRM}};

// SETTINGS MENU (5 items)
static const menu_item_t settings_items[] = {
    {"Voltage Thresh", MENU_SETTINGS},
    {"Current Limit", MENU_SETTINGS},
    {"Freq Range", MENU_SETTINGS},
    {"Temp Alarm", MENU_SETTINGS},
    {"Sys Timeout", MENU_SETTINGS}};

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

// FACTORY RESET MENU (3 items)
static const menu_item_t factory_reset_items[] = {
    {"Reset All Data", MENU_FACTORY_RESET_CONFIRM},
    {"Clear Settings", MENU_FACTORY_RESET_CONFIRM},
    {"Erase Logs", MENU_FACTORY_RESET_CONFIRM}};

// LED channel definitions for easy reference
typedef enum
{
    LED_STATUS = LEDC_CHANNEL_1,
    LED_ERROR = LEDC_CHANNEL_2,
    // LED_OTHER = LEDC_CHANNEL_3,
} led_channel_t;

// Get current time in milliseconds
int64_t lcd_get_current_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void lcd_create_custom_char(uint8_t location, uint8_t charmap[]);
void lcd_write_float(float value, uint8_t decimalPlaces);

adc_cali_handle_t handle = NULL;

// =============== FUNCTION PROTOTYPES ===============

void init_hardware();
void nvs_init(bool erase_on_fail);
bool save_settings();
bool load_settings();
static inline void lcd_lock(void);
static inline void lcd_unlock(void);
void lcd_show_bt_edit_screen(const char *label, const char *value);
void lcd_show_value_edit_screen(void);
void lcd_show_bt_connecting_screen(const char *device_name);
void lcd_show_factory_reset_screen(void);
void update_buzzer(uint16_t freq, uint8_t volume);
void update_led(led_channel_t led, uint8_t brightness /*uint8_t led*/);
void adc_task(void *arg);
static bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle);
static void adc_calibration_deinit(adc_cali_handle_t handle);
void display_task(void *arg);
uint8_t calculate_battery_percentage(float voltage);
void power_task(void *arg);
void error_handler();
void toggle_display();

// Function prototypes
void inverter_power_on(void);
void shutdown_inverter(void);
void enter_diagnostic_mode(void);
void exit_diagnostic_mode(void);
void lcd_draw_diagnostics_screen(uint8_t index);
void perform_factory_reset(void);
void menu_navigate_up(void);
void menu_navigate_down(void);
void menu_enter_selection(void);
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
void lcd_update_display(void);
void lcd_display_startup_screen(void);
static void sync_lcd_state(void);
void lcd_draw_main_screen(lcd_display_state_t *state);
void lcd_draw_progress_bar(uint8_t, uint8_t percent);
void lcd_update_menu_screen(void);
void lcd_update_value_edit_screen(void);
void lcd_draw_error_screen(void);

void process_battery_voltage(void);

bool inverter_set_output_voltage(float voltage_setpoint);
bool inverter_set_output_frequency(float frequency_setpoint);
bool inverter_set_current_limit(float current_limit);
bool thermal_protection_set_limit(float temperature_limit_celsius);
bool battery_monitor_set_cutoff(float cutoff_voltage);
void set_system_timeout(uint32_t timeout_ms);
void inverter_emergency_shutdown(void);

// Value adjustment functions
void increase_value(bool fast_mode, bool precision_mode);
void decrease_value(bool fast_mode, bool precision_mode);
void enter_value_edit_mode(value_edit_context_t *value_type);
void exit_value_edit_mode(bool save_changes);
void apply_value_change(void);
void reset_value_to_backup(void);
float *get_current_value_pointer(void);
value_edit_context_t *get_current_value_config(void);
float calculate_increment(bool fast_mode, bool precision_mode);
bool validate_value_range(float new_value);
void handle_value_confirmation(void);
void update_system_parameter(value_edit_context_t *context_type, float value);
static const menu_item_t *get_menu_items(menu_state_t state, int *item_count);
void edit_voltage_threshold(void);
void edit_current_limit(void);
void edit_frequency_range(void);
void edit_temperature_alarm(void);
void edit_system_timeout(void);

// Submenu functions
void lcd_show_monitoring_detail(const char *label, float value, const char *unit);
void lcd_draw_diagnostics_screen(uint8_t index);

static void wifi_init_sta(void);
void start_wifi_scan(void);
void lcd_show_wifi_scan_screen(void);
void start_wifi_connection(void);

// Function to save the frequency setting to NVS
void save_frequency_to_nvs(int frequency);
void clamp_values();
int get_setting_value(int index);
void set_setting_value(int index, int value);
void menu_exit();
menu_state_t display_menu_state();
void adjust_factory_reset(button_event_info_t btn);
void init_menu_system();
void restore_from_deep_sleep();
void enter_deep_sleep(uint32_t sleep_seconds);
void init_deep_sleep(uint64_t wakeup_pin_mask, int wakeup_time_sec);
void save_calibration();
void load_calibration();
float read_adc_calibrated(adc_channel_t channel, adc_oneshot_unit_handle_t adc_handle);
void check_protections();
void update_led_status();
void perform_factory_reset();
void show_system_info();
bool system_is_inactive();
void update_activity();
void display_timeout_task(void *arg);
void lcd_write_float(float value, uint8_t decimalPlaces);
void lcd_power_init();
void LCD_power(bool enable);
void lcd_set_brightness(uint8_t brightness);
void init_system_state();
void handle_critical_error(); // centralized error handling
bool battery_save_configuration(battery_type_t type, voltage_system_t voltage_sys, uint16_t capacity_ah);
void battery_print_profile(const battery_profile_t *profile);
bool battery_load_profile(battery_profile_t *profile);
void monitor_battery_task(void *arg);
void battery_menu_task(void *arg);
void log_error_to_nvs(uint8_t error_code);
void shutdown();
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

#define RTC_MAGIC_FLAG 0xA5A5A5A5 // Used to validate RTC memory after wake-up

// ================== RTC STRUCT ==================
typedef struct
{
    uint32_t magic_flag;        // To validate RTC memory content
    TickType_t last_sleep_time; // Time of last sleep entry
    bool was_inverter_active;
    bool ac_was_connected;
    uint32_t last_error;
    uint32_t wake_count;
} rtc_mem_t;

RTC_DATA_ATTR rtc_mem_t rtc_mem; // Must be placed in RTC fast memory

// ================== BUTTON SYSTEM DEFINITIONS ==================

// Button Configuration
#define BUTTON_MAX 5             // Max number of buttons supported
#define DEBOUNCE_THRESHOLD_MS 50 // Debounce time in ms
#define LONG_PRESS_MS 3000       // Long press threshold in ms
#define HOLD_PRESS_MS 500        // Hold press threshold in ms
#define VERY_LONG_PRESS_MS 3000  // Very long press threshold in ms
#define DOUBLE_CLICK_MS 400      // Double click max interval in ms
#define REPEAT_INITIAL_DELAY_MS 500
#define REPEAT_INTERVAL_MS 100

#define ISR_QUEUE_SIZE 10
#define TASK_POLL_INTERVAL_MS 10

static const char *APP_TAG = "BUTTON_APP";

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

// // Define actual GPIO pins for each button
// const gpio_num_t button_pins[BUTTON_MAX] = {
//     [BUTTON_POWER] = GPIO_NUM_16, // GPIO 0 for power button
//     [BUTTON_UP] = GPIO_NUM_17,    // GPIO 17 for up button
//     [BUTTON_DOWN] = GPIO_NUM_5,   // GPIO 5 for down button
//     [BUTTON_ENTER_MENU] = GPIO_NUM_19, // GPIO 19 for enter button
//     [BUTTON_BACK] = GPIO_NUM_18   // GPIO 18 for back button
// };
// ================== BUTTON INITIALIZATION ==================

// ================== BUTTON ISR HANDLER WITH DEBOUNCE ==================
extern void update_activity(); // Activity timestamp updater

// Polling task to detect release & long press
#define DEBOUNCE_TIME_MS 20  // Debounce threshold (adjust as needed)
#define REPEAT_DELAY_MS 1000 // 1000ms before repeat starts
#define REPEAT_RATE_MS 100   // 100ms between repeat events

// =============== HARDWARE INITIALIZATION ===============
void init_hardware()
{
#if CONFIG_USE_LCD
    lcd_init(LCD_ADDR, SDA_PIN, SCL_PIN);
#endif
    // ===== 1. Configure Output GPIOs (non-PWM) =====
    const gpio_num_t output_pins[] = {
        GPIO_POWER_RELAY,
        GPIO_FAN_TEST};

    gpio_config_t led_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};

    for (int i = 0; i < sizeof(output_pins) / sizeof(output_pins[0]); i++)
    {
        led_conf.pin_bit_mask = (1ULL << output_pins[i]);
        gpio_config(&led_conf);
        gpio_set_level(output_pins[i], 0);
    }

    // ===== 2. Initialize Buzzer PWM =====
    ledc_timer_config_t buzzer_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT, // Changed to 13-bit to match your function
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&buzzer_timer);

    ledc_channel_config_t buzzer_channel = {
        .gpio_num = GPIO_BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, // Changed to match your function
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0};
    ledc_channel_config(&buzzer_channel);

#if CONFIG_USE_LED_PWM
    // ===== 3. Initialize LED PWM Timer (shared by all LEDs) =====
    ledc_timer_config_t led_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT, // Changed to 13-bit to match your function
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&led_timer);

    // Define LED channels
    typedef struct
    {
        gpio_num_t gpio;
        ledc_channel_t channel;
    } led_pwm_config_t;

    const led_pwm_config_t led_configs[] = {
        {GPIO_STATUS_LED, LEDC_CHANNEL_1}, // Changed to start from channel 1
        {GPIO_ERROR_LED, LEDC_CHANNEL_2},
    };

    // Configure all LED channels
    for (int i = 0; i < sizeof(led_configs) / sizeof(led_configs[0]); i++)
    {
        ledc_channel_config_t led_channel = {
            .gpio_num = led_configs[i].gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = led_configs[i].channel,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0};
        ledc_channel_config(&led_channel);
    }

    // Install fade service for smooth LED transitions
    ledc_fade_func_install(0);
    update_led(LED_STATUS, 0); // Ensure LEDs start off
    update_led(LED_ERROR, 0);
#endif

    // ===== 5. Initialize System State Tracking =====
    sys_state.flags.last_user_activity = xTaskGetTickCount();
    sys_state.flags.last_power_event = xTaskGetTickCount();
    sys_state.display.display_on = true;

#if CONFIG_USE_DEEP_SLEEP
    // ===== 6. Setup Deep Sleep =====
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_14, 1);
    esp_sleep_enable_timer_wakeup(3600 * 1000000ULL);
#endif

#if CONFIG_USE_DISPLAY_TIMEOUT_TASK
    // ===== 7. Create Display Timeout Task =====
    xTaskCreate(display_timeout_task, "Display Timeout", 2048, NULL, 5, NULL);
#endif
}

// ================== PWM CONTROL FUNCTIONS ==================

// ===== BUZZER CONTROL =====
void update_buzzer(uint16_t freq_hz, uint8_t volume_percent)
{
    if (freq_hz == 0 || volume_percent == 0)
    {
        // Turn off buzzer completely
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        return;
    }

    // 1. Update frequency if changed
    static uint16_t last_freq = 0;
    if (freq_hz != last_freq)
    {
        ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1, freq_hz);
        last_freq = freq_hz;
    }

    // 2. Validate volume
    volume_percent = volume_percent > 100 ? 100 : volume_percent;

    // 3. Calculate and set duty cycle (volume) - 13-bit resolution
    uint32_t duty = (volume_percent / 100.0f) * ((1 << 13) - 1);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// Buzzer helper functions
void buzzer_off()
{
    update_buzzer(0, 0);
}

void buzzer_beep(uint16_t freq_hz, uint8_t volume_percent, uint32_t duration_ms)
{
    update_buzzer(freq_hz, volume_percent);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    buzzer_off();
}

void buzzer_alert()
{
    buzzer_beep(2000, 50, 100);
    vTaskDelay(pdMS_TO_TICKS(100));
    buzzer_beep(2000, 50, 100);
}

void buzzer_error()
{
    buzzer_beep(500, 70, 500);
}

void buzzer_success()
{
    buzzer_beep(2500, 40, 200);
}

// ===== LED CONTROL =====

void update_led(led_channel_t led, uint8_t brightness_percent)
{
    // Validate input
    brightness_percent = brightness_percent > 100 ? 100 : brightness_percent;

    // Calculate duty cycle (0-8191 for 13-bit)
    uint32_t duty = (brightness_percent / 100.0f) * ((1 << 13) - 1);

    // Apply to LED channel
    ledc_set_duty(LEDC_LOW_SPEED_MODE, led, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, led);
}

// LED helper functions using percentage-based control
void led_on(led_channel_t led)
{
    update_led(led, 100);
}

void led_off(led_channel_t led)
{
    update_led(led, 0);
}

void set_led_brightness(led_channel_t led, uint8_t brightness_percent)
{
    update_led(led, brightness_percent);
}

void fade_led(led_channel_t led, uint8_t target_brightness_percent, uint32_t fade_time_ms)
{
    // Validate input
    target_brightness_percent = target_brightness_percent > 100 ? 100 : target_brightness_percent;

    // Calculate duty cycle
    uint32_t duty = (target_brightness_percent / 100.0f) * ((1 << 13) - 1);

    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, led, duty, fade_time_ms);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, led, LEDC_FADE_NO_WAIT);
}

void blink_led(led_channel_t led, uint32_t on_time_ms, uint32_t off_time_ms, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++)
    {
        led_on(led);
        vTaskDelay(pdMS_TO_TICKS(on_time_ms));
        led_off(led);
        if (i < count - 1)
        {
            vTaskDelay(pdMS_TO_TICKS(off_time_ms));
        }
    }
}

void pulse_led(led_channel_t led, uint32_t period_ms, uint8_t cycles)
{
    for (uint8_t i = 0; i < cycles; i++)
    {
        fade_led(led, 100, period_ms / 2);
        vTaskDelay(pdMS_TO_TICKS(period_ms / 2));
        fade_led(led, 0, period_ms / 2);
        vTaskDelay(pdMS_TO_TICKS(period_ms / 2));
    }
}

void set_all_leds(uint8_t brightness_percent)
{
    update_led(LED_STATUS, brightness_percent);
    update_led(LED_ERROR, brightness_percent);
}

void all_leds_off()
{
    set_all_leds(0);
}

void all_leds_on()
{
    set_all_leds(100);
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

static void apply_default_value(void *dest, size_t size, uint32_t default_val)
{
    memcpy(dest, &default_val, size);
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
        load_error = false;
        return false;
    }

    struct
    {
        const char *key;
        bool (*get_fn)(nvs_handle_t, const char *, void *);
        void *dest;
        size_t size;
        uint32_t default_val;
    } settings[] = {
        {"brightness", (bool (*)(nvs_handle_t, const char *, void *))get_i32_safe, &sys_state.display.brightness, sizeof(int32_t), 100},
        {"backlight_time", (bool (*)(nvs_handle_t, const char *, void *))get_i32_safe, &sys_state.display.backlight_timeout, sizeof(int32_t), 30},
        {"auto_shutdown", (bool (*)(nvs_handle_t, const char *, void *))get_u8_safe, &sys_state.display.auto_shutdown_enabled, sizeof(uint8_t), 0},
        {"scroll_en", (bool (*)(nvs_handle_t, const char *, void *))get_u8_safe, &sys_state.display.scroll_enabled, sizeof(uint8_t), 0},
        {"scroll_spd", (bool (*)(nvs_handle_t, const char *, void *))get_u8_safe, &sys_state.display.scroll_speed, sizeof(uint8_t), DEFAULT_SCROLL_SPEED},
        {"out_volt", (bool (*)(nvs_handle_t, const char *, void *))get_i32_safe, &sys_state.inverter.output_voltage, sizeof(int32_t), 22000},
        {"out_freq", (bool (*)(nvs_handle_t, const char *, void *))get_i32_safe, &sys_state.inverter.output_frequency, sizeof(int32_t), 5000},
        {"bat_cutoff_volt", (bool (*)(nvs_handle_t, const char *, void *))get_i32_safe, &sys_state.battery_profile.cutoff_voltage_12v, sizeof(int32_t), 1050},
        {"bat_recharge_volt", (bool (*)(nvs_handle_t, const char *, void *))get_i32_safe, &sys_state.battery_profile.recharge_voltage_12v, sizeof(int32_t), 1480},
        {"bat_type", (bool (*)(nvs_handle_t, const char *, void *))get_u8_safe, &sys_state.battery_profile.profile_id, sizeof(uint8_t), BATTERY_AGM},
        {"voltage_threshold", (bool (*)(nvs_handle_t, const char *, void *))get_i32_safe, &sys_state.settings.voltage_threshold, sizeof(int32_t), 12.5},
        {"current_limit", (bool (*)(nvs_handle_t, const char *, void *))get_i32_safe, &sys_state.settings.current_limit, sizeof(int32_t), 5000},
        {"temperature_alarm", (bool (*)(nvs_handle_t, const char *, void *))get_i32_safe, &sys_state.settings.temperature_alarm, sizeof(int32_t), 70.0},
        {"frequency_range", (bool (*)(nvs_handle_t, const char *, void *))get_i32_safe, &sys_state.settings.frequency_range, sizeof(int32_t), 50},
        {"system_timeout", (bool (*)(nvs_handle_t, const char *, void *))get_i32_safe, &sys_state.settings.system_timeout, sizeof(int32_t), 300}};

    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++)
    {
        if (!settings[i].get_fn(nvs, settings[i].key, settings[i].dest))
        {
            ESP_LOGW(NVS_LOADING_TAG, "Using default for %s: %lu", settings[i].key, settings[i].default_val);
            apply_default_value(settings[i].dest, settings[i].size, settings[i].default_val);
            load_error = true;
        }

        // Rescale int values to float
        if (strcmp(settings[i].key, "out_volt") == 0 ||
            strcmp(settings[i].key, "out_freq") == 0 ||
            strcmp(settings[i].key, "bat_cutoff") == 0 ||
            strcmp(settings[i].key, "bat_recharge") == 0)
        {
            *(float *)settings[i].dest /= 100.0f;
        }
    }

    // Load battery profile (type and voltage)
    if (!battery_load_profile(&sys_state.battery_profile))
    {
        ESP_LOGW("BAT_PROFILE", "Failed to load battery profile, using defaults");
        load_error = true;
    }

    if (load_error)
    {
        ESP_LOGW(NVS_LOADING_TAG, "Settings loaded with defaults");
        save_settings(); // Save defaults
        return false;
    }

    ESP_LOGI(NVS_LOADING_TAG, "Settings loaded successfully");
    return true;
}

bool save_settings()
{
    esp_err_t err;
    // Open NVS for writing
    if (!nvs_initialized)
    {
        nvs_init(true); // Initialize NVS if not already done
    }
    nvs_handle_t nvs;
    const char *NVS_SAVE_TAG = "NVS_SAVE";

    err = nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
    {
        ESP_LOGE(NVS_SAVE_TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return false;
    }
    // Prepare settings to save
    // Use a struct array to hold key-value pairs for settings
    struct
    {
        const char *key;
        esp_err_t (*set_fn)(nvs_handle_t, const char *, void *);
        void *value;
    } settings[] = {
        {"bat_volt_system", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_u8_safe, &sys_state.inverter.battery_voltage_system},
        {"inverter_active", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_u8_safe, &sys_state.inverter.inverter_active},
        {"inverter_volt", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.inverter.output_voltage * 100)}},
        {"inverter_freq", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.inverter.output_frequency * 100)}},
        {"bat_cutoff_volt", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.battery_profile.cutoff_voltage_12v * 100)}},
        {"bat_rech_volt", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.battery_profile.recharge_voltage_12v * 100)}},
        {"bat_type", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_u8_safe, &sys_state.battery_profile.profile_id},
        {"bat_cap_ah", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.battery_profile.capacity_ah * 100)}},
        {"bat_charge_cur", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.battery_profile.max_charge_current_per_100ah * 100)}},
        {"bat_disc_cur", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.battery_profile.max_discharge_current_per_100ah * 100)}},
        {"bat_full_volt", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.battery_profile.high_battery_voltage_12v * 100)}},
        {"bat_cutoff_volt", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.battery_profile.cutoff_voltage_12v * 100)}},
        {"bat_volt_type", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_u8_safe, &sys_state.battery_profile.profile_id},
        {"brightness", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &sys_state.display.brightness},
        {"backlight_time", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &sys_state.display.backlight_timeout},
        {"auto_shutdown", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_u8_safe, &sys_state.display.auto_shutdown_enabled},
        {"scroll_en", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_u8_safe, &sys_state.display.scroll_enabled},
        {"scroll_spd", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_u8_safe, &sys_state.display.scroll_speed},
        {"out_volt", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.inverter.output_voltage * 100)}},
        {"out_freq", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.inverter.output_frequency * 100)}},
        {"bat_cutoff", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.battery_profile.cutoff_voltage_12v * 100)}},
        {"bat_recharge", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.battery_profile.recharge_voltage_12v * 100)}},
        {"volt_threshold", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.settings.voltage_threshold * 100)}},
        {"current_limit", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.settings.current_limit * 100)}},
        {"temp_alarm", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.settings.temperature_alarm * 100)}},
        {"frequency_range", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.settings.frequency_range * 100)}},
        {"system_timeout", (esp_err_t (*)(nvs_handle_t, const char *, void *))set_i32_safe, &(int32_t){(int)(sys_state.settings.system_timeout * 100)}}};
    // Save all settings
    ESP_LOGI(NVS_SAVE_TAG, "Saving settings to NVS...");
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++)
    {
        err = settings[i].set_fn(nvs, settings[i].key, settings[i].value);
        if (err != ESP_OK)
        {
            ESP_LOGE(NVS_SAVE_TAG, "Failed to save %s: %s", settings[i].key, esp_err_to_name(err));
        }
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

// Error code to string mapping

typedef struct
{
    uint32_t code;
    const char *message;
} ErrorInfo;

static const ErrorInfo error_table[] = {
    {ERR_OVER_TEMP, "Overtemperature"},
    {ERR_OVERLOAD, "Overload"},
    {ERR_BATTERY_VOLTAGE, "Battery Voltage Fault"},
    {ERR_LOW_BAT, "Low Battery"},
    {ERR_UNDER_VOLTAGE, "Under Voltage"},
    {ERR_OVER_VOLTAGE, "Over Voltage"},
    {ERR_INVERTER_VOLTAGE, "Inverter Voltage Fault"},
    {ERR_AC_FAULT, "AC Fault"},
    {ERR_FAN_FAIL, "Fan Failure"},
    {ERR_EEPROM, "EEPROM Error"},
    {ERR_HIGH_BAT, "High Battery"},
    {ERR_SHORT_CIRCUIT, "Short Circuit"},
    {ERR_SYSTEM_FAILURE, "System Failure"},
    {ERR_OVER_UNDER_VOLTAGE, "Over/Under Voltage"},
};

const char *get_error_string(uint32_t code)
{
    for (int i = 0; i < sizeof(error_table) / sizeof(error_table[0]); ++i)
    {
        if (error_table[i].code == code)
            return error_table[i].message;
    }
    return "Unknown Error";
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
        lcd_clear();                                            \
        lcd_set_cursor(0, 0);                                   \
        lcd_print(err_str);                                     \
        lcd_set_cursor(1, 0);                                   \
        lcd_print("System Halted");                             \
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
// voltage_divider_ratio = (R1 + R2) / R2
// Example: For 12V input with 10kΩ and 3.3kΩ resistors: ratio = (10k + 3.3k) / 3.3k = 4.03
// Example: For direct connection (no divider): ratio = 1.0

#define R1_BATTERY_VOLTAGE 56000.0f                                                                    // Top resistor in ohms
#define R2_BATTERY_VOLTAGE 15000.0f                                                                    // Bottom resistor in ohms
#define BATTERY_VOLTAGE_DIVIDER_RATIO ((R1_BATTERY_VOLTAGE + R2_BATTERY_VOLTAGE) / R2_BATTERY_VOLTAGE) // 4.7333

#define R1_LOW_BATTERY 56000.0f                                                        // Top resistor in ohms
#define R2_LOW_BATTERY 15000.0f                                                        // Bottom resistor in oh
#define LOW_BATTERY_DIVIDER_RATIO ((R1_LOW_BATTERY + R2_LOW_BATTERY) / R2_LOW_BATTERY) // 5.4545

#define R1_AC_VOLTAGE 56000.0f                                                     // Top resistor in ohms
#define R2_AC_VOLTAGE 15000.0f                                                     // Bottom resistor in oh
#define AC_VOLTAGE_DIVIDER_RATIO ((R1_AC_VOLTAGE + R2_AC_VOLTAGE) / R2_AC_VOLTAGE) // 31.303

#define R1_INVERTER_VOLTAGE 56000.0f                                                                       // Top resistor in ohms
#define R2_INVERTER_VOLTAGE 15000.0f                                                                       // Bottom resistor
#define INVERTER_VOLTAGE_DIVIDER_RATIO ((R1_INVERTER_VOLTAGE + R2_INVERTER_VOLTAGE) / R2_INVERTER_VOLTAGE) // 4.7333

#define R1_FAN_VOLTAGE 56000.0f                                                              // Top resistor in ohms
#define R2_FAN_VOLTAGE 15000.0f                                                              // Bottom resistor
#define FAN_SPEED_VOLTAGE_DIVIDER_RATIO ((R1_FAN_VOLTAGE + R2_FAN_VOLTAGE) / R2_FAN_VOLTAGE) // 2.0

/*==============================================================================
  BATTERY VOLTAGE MONITORING AND PROTECTION
==============================================================================*/

// Battery monitoring parameters
#define BATTERY_DEBOUNCE_COUNT 3  // Require 3 consecutive readings
#define BATTERY_FILTER_ALPHA 0.2f // Low-pass filter coefficient (0-1)

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

    ESP_LOGI(TAG_ADC, "Starting ADC sampling and LCD updates");
    // Main task loop

    while (1)
    {
        for (int i = 0; i < config_count; i++)
        {
            process_adc_reading(&adc_configs[i],
                                &adc1_context.channel_states[i],
                                adc1_context.handle);
        }
        // Process battery voltage monitoring
        process_battery_voltage();
        xEventGroupSetBits(sys_event_group, EVT_ADC_READY | EVT_ADC_VALID); // Signal: ADC data is valid and processed
        // Notify LCD task on first sample (if using separate LCD task)
        if (first_sample)
        {
            first_sample = false;
            ESP_LOGI(TAG_ADC, "First ADC sample complete: Battery=%.2fV",
                     sys_state.inverter.battery.voltage);

            if (lcd_task_handle != NULL)
            {
                xTaskNotifyGive(lcd_task_handle); // Signal: ADC READY
            }
        }

        // Delay before next ADC sample
        vTaskDelay(pdMS_TO_TICKS(100)); // Small delay to check LCD timing frequently
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

    // Periodic logging

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

// Power management task
// This task handles power management, including sleep mode, error handling, and system status updates.
void power_task(void *arg)
{
    static bool last_relay_state = false;
    static TickType_t last_state_change = 0;
    const TickType_t DEBOUNCE_TIME = pdMS_TO_TICKS(2000); // 2 second debounce

    while (1)
    {
        // Step 1: Update last power activity timestamp
        if (sys_state.inverter.inverter_active || sys_state.inverter.connected)
        {
            sys_state.flags.last_power_event = xTaskGetTickCount();
        }

        // Step 2: Check inactivity and enter sleep mode if needed
        if ((xTaskGetTickCount() - sys_state.flags.last_power_event) > pdMS_TO_TICKS(30 * 60 * 1000))
        {
            ESP_LOGI("POWER_TASK", "Entering deep sleep due to inactivity");
            enter_deep_sleep(3600);
        }

        // Step 3: Extreme temperature shutdown
        if (sys_state.inverter.temperature > 90.0f)
        {
            lcd_clear();
            lcd_print("Critical Temp!");
            vTaskDelay(pdMS_TO_TICKS(1000));
            lcd_set_cursor(0, 1);
            lcd_print("Shutting Down...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            perform_system_restart(false);
        }

        // Step 7: Automatic AC ↔ Inverter switching logic WITH DEBOUNCING
        bool new_relay_state;

        if (sys_state.inverter.connected && !(sys_state.error.error_flags & ERR_AC_FAULT))
        {
            new_relay_state = true; // Use AC
        }
        else
        {
            // Only turn relay off if no critical errors
            if (!(sys_state.error.error_flags & (ERR_OVER_TEMP | ERR_OVERLOAD | ERR_LOW_BAT | ERR_HIGH_BAT)))
            {
                new_relay_state = false; // Use inverter
            }
            else
            {
                new_relay_state = last_relay_state; // Keep current state if error
            }
        }

        // Only change relay state if enough time has passed since last change
        if (new_relay_state != last_relay_state)
        {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_state_change) > DEBOUNCE_TIME)
            {
                gpio_set_level(GPIO_POWER_RELAY, new_relay_state ? 1 : 0);
                sys_state.inverter.inverter_active = !new_relay_state;
                last_relay_state = new_relay_state;
                last_state_change = now;

                ESP_LOGI("POWER_TASK", "Relay switched to: %s",
                         new_relay_state ? "AC" : "INVERTER");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500)); // Check every 500ms
    }
}

// Battery monitoring parameters
#define BATTERY_DEBOUNCE_COUNT 3  // Require 3 consecutive readings
#define BATTERY_FILTER_ALPHA 0.2F // Low_pass filter co_efficient

/**
 * @brief Process battery voltage reading and take action
 */
void process_battery_voltage(void)
{
    const char *TAG = "BATTERY";

    // Get current voltage reading
    float voltage_raw = sys_state.inverter.battery.voltage;

    // Apply simple low-pass filter to smooth readings
    // filtered = alpha × new + (1 - alpha) × old
    if (sys_state.inverter.battery.voltage_filtered == 0.0f)
    {
        // First reading - initialize filter
        sys_state.inverter.battery.voltage_filtered = voltage_raw;
    }
    else
    {
        sys_state.inverter.battery.voltage_filtered =
            BATTERY_FILTER_ALPHA * voltage_raw +
            (1.0f - BATTERY_FILTER_ALPHA) * sys_state.inverter.battery.voltage_filtered;
    }

    float voltage = sys_state.inverter.battery.voltage_filtered;

    // Check for critical low voltage (with debouncing)
    if (voltage < sys_state.battery_profile.cutoff_voltage_min_12v)
    {
        sys_state.inverter.battery.critical_count++;

        if (sys_state.inverter.battery.critical_count >= BATTERY_DEBOUNCE_COUNT)
        {
            if (!sys_state.inverter.battery.is_critical)
            {
                // CRITICAL: Battery too low - TAKE ACTION!
                sys_state.inverter.battery.is_critical = true;

                ESP_LOGE(TAG, "╔════════════════════════════════════════╗");
                ESP_LOGE(TAG, "║   CRITICAL BATTERY VOLTAGE: %.2fV    ║", voltage);
                ESP_LOGE(TAG, "║   SHUTTING DOWN INVERTER!            ║");
                ESP_LOGE(TAG, "╚════════════════════════════════════════╝");

                // TAKE ACTION HERE:
                shutdown_inverter(); // Turn off inverter
                blink_led(LED_STATUS, 200, 100, 5);
                buzzer_beep(1000, 80, 3000);
                // Optional: Send notification
                // send_mqtt_alert("Battery critical!");
            }
        }
    }
    else
    {
        // Voltage is above critical - reset counter
        sys_state.inverter.battery.critical_count = 0;

        // Check if recovering from critical state
        if (sys_state.inverter.battery.is_critical && voltage > (sys_state.battery_profile.cutoff_voltage_min_12v + 0.3f))
        {
            sys_state.inverter.battery.is_critical = false;
            ESP_LOGI(TAG, "Battery voltage recovered to %.2fV", voltage);
        }
    }

    // Check for low voltage warning (with debouncing)
    if (voltage < sys_state.battery_profile.cutoff_voltage_12v - 0.4)
    {
        sys_state.inverter.battery.low_count++;

        if (sys_state.inverter.battery.low_count >= BATTERY_DEBOUNCE_COUNT)
        {
            if (!sys_state.inverter.battery.is_low)
            {
                sys_state.inverter.battery.is_low = true;
                ESP_LOGW(TAG, "⚠ Battery low: %.2fV (warning at %.2fV)",
                         voltage, sys_state.battery_profile.cutoff_voltage_12v - 0.4);
                blink_led(LED_STATUS, 200, 100, 2);
                buzzer_beep(500, 70, 500);
            }
        }
    }
    else
    {
        sys_state.inverter.battery.low_count = 0;
        if (sys_state.inverter.battery.is_low)
        {
            sys_state.inverter.battery.is_low = false;
            ESP_LOGI(TAG, "Battery voltage normal: %.2fV", voltage);
            update_led(LED_STATUS, 100);
        }
    }

    // Log battery status periodically
    static uint32_t last_log_time = 0;
    uint32_t now = xTaskGetTickCount();
    if ((now - last_log_time) > pdMS_TO_TICKS(30000)) // Every 30 seconds
    {
        ESP_LOGI("BATTERY_VOLTAGE", "Battery voltage: %.2f", voltage);
        last_log_time = now;
    }
}

void check_protections()
{

    // Clear non-critical flags at start (keep persistent ones)
    sys_state.error.error_flags &= (ERR_EEPROM | ERR_FAN_FAIL);

    // Temperature check
    if (sys_state.inverter.temperature > MAX_TEMPERATURE)
    {
        sys_state.error.error_flags |= ERR_OVER_TEMP;
        ESP_LOGE("PROTECTION", "Over temp: %.1fC", sys_state.inverter.temperature);
    }
    else
    {
        sys_state.error.error_flags &= ~ERR_OVER_TEMP;
    }

    // Current check
    if (sys_state.inverter.actual_current > MAX_CURRENT)
    {
        sys_state.error.error_flags |= ERR_OVERLOAD;
        ESP_LOGE("PROTECTION", "Overload: %.1fA", sys_state.inverter.actual_current);
    }
    else
    {
        sys_state.error.error_flags &= ~ERR_OVERLOAD;
    }

    // Battery voltage checks
    if (sys_state.inverter.battery.voltage < sys_state.battery_profile.cutoff_voltage_12v)
    {
        sys_state.error.error_flags |= ERR_LOW_BAT;
        ESP_LOGE("PROTECTION", "Low battery: %.1fV", sys_state.inverter.battery.voltage);
    }
    else
    {
        sys_state.error.error_flags &= ~ERR_LOW_BAT;
    }

    if (sys_state.inverter.battery.voltage > sys_state.battery_profile.high_battery_voltage_12v)
    {
        sys_state.error.error_flags |= ERR_HIGH_BAT;
        ESP_LOGE("PROTECTION", "High battery: %.1fV", sys_state.inverter.battery.voltage);
    }
    else
    {
        sys_state.error.error_flags &= ~ERR_HIGH_BAT;
    }

    // Fan check
    if (sys_state.inverter.temperature > 70.0f && !sys_state.fan.connected)
    {
        sys_state.error.error_flags |= ERR_FAN_FAIL;
        ESP_LOGE("PROTECTION", "Fan failure at %.1fC", sys_state.inverter.temperature);
    }
    // Step 7: Update error LEDs based on the flags
    // update_led_status(); // This function already controls the error LEDs
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

// Function prototype for battery percentage calculation (assumed to be defined elsewhere)
extern uint8_t calculate_battery_percentage(float voltage);

// Function prototype for getting the mode string
const char *get_mode_string(uint8_t mode);

// Advanced display task implementation
// ------------------------- Display Config -------------------------
#define DISPLAY_BUFFER_SIZE 17
#define SCREEN_DISPLAY_DURATION_MS 5000
#define SCREEN_SWITCH_INTERVAL_MS 1000
#define ERROR_DISPLAY_INTERVAL_MS 1000
#define STARTUP_DELAY_MS 2000
#define NUM_DISPLAY_SCREENS 3

// ------------------------- Display Screens Enum -------------------------
typedef enum
{
    SCREEN_OUTPUT_INFO,
    SCREEN_BATTERY_STATUS,
    SCREEN_SYSTEM_STATUS
} DisplayScreen_t;

// ------------------------- Helper Functions -------------------------
extern uint8_t calculate_battery_percentage(float voltage); // You may use your real implementation

// ------------------------- Screen Rendering Functions -------------------------
void render_output_info(char *buffer)
{
    lcd_set_cursor(0, 0);
    snprintf(buffer, DISPLAY_BUFFER_SIZE, "OUT:%03.0fV %02.0fHz",
             sys_state.inverter.output_voltage, sys_state.inverter.output_frequency);
    lcd_print(buffer);

    lcd_set_cursor(0, 1);
    snprintf(buffer, DISPLAY_BUFFER_SIZE, "CUR:%03.1fA PW:%03.0fW",
             sys_state.inverter.output_current,
             sys_state.inverter.output_voltage * sys_state.inverter.output_current);
    lcd_print(buffer);
}

void render_battery_status(char *buffer)
{
    lcd_set_cursor(0, 0);
    snprintf(buffer, DISPLAY_BUFFER_SIZE, "BAT:%02.1fV %03d%%",
             sys_state.inverter.battery.voltage,
             calculate_battery_percentage(sys_state.inverter.battery.voltage));
    lcd_print(buffer);

    lcd_set_cursor(0, 1);
    snprintf(buffer, DISPLAY_BUFFER_SIZE, "TEMP:%02.0fC CHG:%s",
             sys_state.inverter.battery.battery_temperature,
             sys_state.battery_charging ? "YES" : "NO ");
    lcd_print(buffer);
}

void render_system_status(char *buffer)
{
    lcd_set_cursor(0, 0);
    snprintf(buffer, DISPLAY_BUFFER_SIZE, "INV:%s MODE:%s",
             sys_state.inverter.inverter_active ? "ON " : "OFF",
             get_mode_string(sys_state.inverter.operating_mode));
    lcd_print(buffer);

    lcd_set_cursor(0, 1);
    snprintf(buffer, DISPLAY_BUFFER_SIZE, "AC:%s LOAD:%02d%%",
             sys_state.inverter.connected ? "YES" : "NO ",
             sys_state.inverter.load_percentage);
    lcd_print(buffer);
}

void render_static_off_screen(char *buffer)
{
    lcd_set_cursor(0, 0);
    snprintf(buffer, DISPLAY_BUFFER_SIZE, "%-16s", "Vonix Inverter");
    lcd_print(buffer);

    lcd_set_cursor(0, 1);
    if (sys_state.inverter.battery.voltage < 10.5f)
    {
        snprintf(buffer, DISPLAY_BUFFER_SIZE, "LOW BAT: %02.1fV", sys_state.inverter.battery.voltage);
    }
    else
    {
        snprintf(buffer, DISPLAY_BUFFER_SIZE, "STD_BY BAT: %03d%%",
                 calculate_battery_percentage(sys_state.inverter.battery.voltage));
    }
    lcd_print(buffer);
}

// ------------------------- Main Display Task -------------------------
void display_task(void *arg)
{
    char buffer[DISPLAY_BUFFER_SIZE];
    TickType_t screen_cycle_start = 0;
    TickType_t last_screen_switch = 0;
    DisplayScreen_t current_screen = SCREEN_OUTPUT_INFO;

    vTaskDelay(pdMS_TO_TICKS(STARTUP_DELAY_MS)); // Allow time for initialization

    while (1)
    {
        if (sys_state.error.error_flags)
        {
            lcd_clear();
            error_handler();
            vTaskDelay(pdMS_TO_TICKS(ERROR_DISPLAY_INTERVAL_MS));
            continue;
        }

        if (sys_state.inverter.inverter_active)
        {
            TickType_t now = xTaskGetTickCount();

            // Reset timing every SCREEN_DISPLAY_DURATION_MS
            if ((now - screen_cycle_start) >= pdMS_TO_TICKS(SCREEN_DISPLAY_DURATION_MS))
            {
                screen_cycle_start = now;
                current_screen = SCREEN_OUTPUT_INFO; // Reset to first screen
            }

            // Change screen every SCREEN_SWITCH_INTERVAL_MS
            if ((now - last_screen_switch) >= pdMS_TO_TICKS(SCREEN_SWITCH_INTERVAL_MS))
            {
                lcd_clear();

                switch (current_screen)
                {
                case SCREEN_OUTPUT_INFO:
                    render_output_info(buffer);
                    break;
                case SCREEN_BATTERY_STATUS:
                    render_battery_status(buffer);
                    break;
                case SCREEN_SYSTEM_STATUS:
                    render_system_status(buffer);
                    break;
                }

                current_screen = (current_screen + 1) % NUM_DISPLAY_SCREENS;
                last_screen_switch = now;
            }

            vTaskDelay(pdMS_TO_TICKS(100)); // Allow responsiveness
        }
        else
        {
            lcd_clear();
            render_static_off_screen(buffer);
            vTaskDelay(pdMS_TO_TICKS(SCREEN_DISPLAY_DURATION_MS * 2));
        }
        // Fixed sampling interval
        vTaskDelay(pdMS_TO_TICKS(100)); // 100ms = 10Hz sampling rate
    }

    vTaskDelete(NULL); // Safety, should never reach here
}

// Example implementation of the battery percentage function (you should have your own)
uint8_t calculate_battery_percentage(float voltage)
{
    // Example linear mapping - adjust based on your battery characteristics
    if (voltage <= 10.5f)
        return 0;
    if (voltage >= 12.6f)
        return 100;
    return (uint8_t)(((voltage - 10.5f) / (12.6f - 10.5f)) * 100);
}

#define RESET_TIMEOUT_MS 10000 // Optional timeout to auto-reset (10 seconds)

// Button Configuration Parameters
/*
 * =============================================================================
 * STEP 1: EVENT CALLBACK HANDLERS FOR EACH BUTTON
 * =============================================================================
 */

// Timing constants (in milliseconds)
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

// Value adjustment constants
#define MAX_REPEAT_MULTIPLIER 10
#define PRECISION_MODE_DIVISOR 10

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
void lcd_update_menu_screen(void)
{
    int num_items = 0;
    const menu_item_t *items = get_menu_items(sys_state.menu_state, &num_items);

    if (!items || num_items == 0)
        return;

    lcd_clear();

    char line1[17] = {0};
    char line2[17] = {0};

    // Current selection
    snprintf(line1, sizeof(line1), ">%-15.15s", items[sys_state.menu_selection].label);
    lcd_set_cursor(0, 0);
    lcd_print(line1);

    // Next item (wrap-around if at end)
    int next_index = (sys_state.menu_selection + 1) % num_items;
    snprintf(line2, sizeof(line2), " %-15.15s", items[next_index].label);
    lcd_set_cursor(0, 1);
    lcd_print(line2);
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

    case MENU_FACTORY_RESET_CONFIRM:
        *item_count = sizeof(factory_reset_items) / sizeof(factory_reset_items[0]);
        return factory_reset_items;

    default:
        *item_count = 0;
        return NULL;
    }
}

void lcd_show_monitoring_detail(const char *label, float value, const char *unit)
{
    char line1[17], line2[17];
    lcd_clear();

    // Format first line (Label)
    snprintf(line1, sizeof(line1), "%s", label);
    lcd_print_centered(0, line1);

    // Format second line (Value + Unit)
    snprintf(line2, sizeof(line2), "%.2f %s", value, unit);
    lcd_print_centered(1, line2);
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
 * @brief Convert RSSI value to a simple signal strength bar string
 */
static const char *wifi_rssi_to_bars(int rssi)
{
    if (rssi >= -50)
        return "||||"; // Excellent
    else if (rssi >= -65)
        return "|||"; // Good
    else if (rssi >= -75)
        return "||"; // Fair
    else if (rssi >= -85)
        return "|"; // Weak
    else
        return " "; // Very poor
}

/**
 * @brief Display WiFi scan results on 16x2 LCD with scrolling and signal bars
 */
void lcd_show_wifi_scan_screen(void)
{
    if (ap_count == 0)
    {
        lcd_clear();
        lcd_print_centered(0, "No Networks");
        lcd_print_centered(1, "Found");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    int index = 0;                                            // Current selection
    int top_index = 0;                                        // Start index for current LCD page
    int64_t last_activity_time = esp_timer_get_time() / 1000; // ms

    while (1)
    {
        // Auto-exit on timeout
        if ((esp_timer_get_time() / 1000) - last_activity_time > WIFI_SCAN_TIMEOUT_MS)
        {
            lcd_clear();
            lcd_print_centered(0, "Scan Timeout");
            vTaskDelay(pdMS_TO_TICKS(1200));
            break;
        }

        lcd_clear();

        // Display up to 2 networks per screen
        for (int line = 0; line < 2; line++)
        {
            int ap_index = top_index + line;
            if (ap_index >= ap_count)
                break;

            char ssid_trimmed[9] = {0};
            strncpy(ssid_trimmed, (char *)ap_records[ap_index].ssid, 8);
            ssid_trimmed[8] = '\0';

            const char *bars = wifi_rssi_to_bars(ap_records[ap_index].rssi);

            char line_buf[17];
            snprintf(line_buf, sizeof(line_buf), "%c%-8s %4s",
                     (ap_index == index) ? '>' : ' ',
                     ssid_trimmed, bars);

            lcd_set_cursor(0, line);
            lcd_print(line_buf);
        }

        // Handle button press via queue
        button_event_info_t button_event;
        if (xQueueReceive(button_event_queue, &button_event, pdMS_TO_TICKS(800)))
        {
            last_activity_time = esp_timer_get_time() / 1000;

            switch (button_event.button_id)
            {
            case BTN_UP:
                if (index > 0)
                    index--;
                if (index < top_index)
                    top_index -= 2; // scroll up
                if (top_index < 0)
                    top_index = 0;
                break;

            case BTN_DOWN:
                if (index < ap_count - 1)
                    index++;
                if (index >= top_index + 2)
                    top_index += 2; // scroll down
                if (top_index >= ap_count)
                    top_index = ap_count - 1;
                break;

            case BTN_ENTER_MENU:
                lcd_clear();
                lcd_print_centered(0, "Connecting to");
                lcd_set_cursor(0, 1);
                lcd_print_centered(1, (char *)ap_records[index].ssid);
                strncpy((char *)sys_state.wifi.ssid, (char *)ap_records[index].ssid, sizeof(sys_state.wifi.ssid) - 1);
                start_wifi_connection();
                vTaskDelay(pdMS_TO_TICKS(2000));
                return;

            case BTN_BACK:
                lcd_clear();
                lcd_print_centered(0, "Exiting Scan");
                vTaskDelay(pdMS_TO_TICKS(1000));
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
static int s_retry_num = 0;

#define MAX_RETRY 5

// --- Wi-Fi event handler ---
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_retry_num < MAX_RETRY)
            {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(WIFI_TAG, "Retrying WiFi connection (%d/%d)...", s_retry_num, MAX_RETRY);
            }
            else
            {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            break;

        default:
            break;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(WIFI_TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// --- Start WiFi connection ---
void start_wifi_connection(void)
{
    ESP_LOGI(WIFI_TAG, "Starting WiFi connection...");

    // === Step 1: Check if Wi-Fi is enabled ===
    if (!sys_state.wifi.enabled)
    {
        lcd_clear();
        lcd_print_centered(0, "Wi-Fi Disabled");
        lcd_print_centered(1, "Enable in Settings");
        ESP_LOGW(WIFI_TAG, "WiFi is disabled. Enable it in settings.");
        return;
    }

    // === Step 2: LCD Feedback ===
    lcd_clear();
    lcd_print_centered(0, "Connecting Wi-Fi");
    lcd_print_centered(1, sys_state.wifi.ssid);

    // === Step 3: Initialize NVS if not done ===
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // === Step 4: Initialize network stack and event loop ===
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // === Step 5: Register event handlers ===
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    // === Step 6: Configure Wi-Fi parameters ===
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, sys_state.wifi.ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, sys_state.wifi.password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    // === Step 7: Start Wi-Fi ===
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(WIFI_TAG, "WiFi started. Connecting to SSID: %s", sys_state.wifi.ssid);

    // === Step 8: Wait for connection result ===
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(15000));

    lcd_clear();
    if (bits & WIFI_CONNECTED_BIT)
    {
        lcd_print_centered(0, "Wi-Fi Connected");
        lcd_print_centered(1, sys_state.wifi.ssid);
        ESP_LOGI(WIFI_TAG, "WiFi connected successfully.");
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        lcd_print_centered(0, "Wi-Fi Failed");
        lcd_print_centered(1, "Check SSID/Password");
        ESP_LOGE(WIFI_TAG, "WiFi failed to connect.");
    }
    else
    {
        lcd_print_centered(0, "Wi-Fi Timeout");
        lcd_print_centered(1, "No Connection");
        ESP_LOGW(WIFI_TAG, "WiFi connection timeout.");
    }

    vEventGroupDelete(s_wifi_event_group);
}

void stop_wifi_connection(void)
{
    esp_wifi_stop();
    lcd_clear();
    lcd_print_centered(0, "Wi-Fi Disconnected");
    ESP_LOGI(WIFI_TAG, "WiFi stopped manually.");
}

void reload_default_settings();

void clear_all_settings(void)
{
    const char *TAG = "Settings_Clear";
    lcd_show_message("Clearing", "Settings...");

    ESP_LOGW(TAG, "Clearing all stored settings...");

    // Erase NVS (acts like EEPROM)
    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK)
        ESP_LOGI(TAG, "NVS flash erased successfully.");
    else
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(err));

    // Reinitialize NVS
    err = nvs_flash_init();
    if (err == ESP_OK)
        ESP_LOGI(TAG, "NVS flash reinitialized successfully.");

    // Reload default runtime settings into sys_state
    reload_default_settings();

    lcd_show_message("Done", "All cleared!");
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "All settings cleared and defaults loaded.");
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

void erase_all_logs(void)
{
    ESP_LOGI(LOG_TAG, "Erasing all logs...");

    // === 1. Clear runtime logs in memory ===
    sys_state.error_count = 0;
    sys_state.uptime_hours = 0;
    sys_state.memory_usage = 0;
    error_log_clear(); // Clear the error log ring buffer and singleton message
    ESP_LOGI(LOG_TAG, "Runtime logs cleared from memory.");
    // === 2. Clear persistent logs from NVS ===
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("log_storage", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK)
    {
        // Erase all key-value pairs in the log namespace
        nvs_erase_all(nvs_handle);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(LOG_TAG, "Logs cleared from NVS.");
    }
    else
    {
        ESP_LOGW(LOG_TAG, "Failed to open NVS for log clearing: %s", esp_err_to_name(err));
    }

    // === 3. If using SD card for logs ===
#ifdef USE_SD_LOGGING
    ESP_LOGI(LOG_TAG, "Attempting to clear SD card logs...");
    remove("/sdcard/logs/system_log.txt");
    remove("/sdcard/logs/error_log.txt");
    ESP_LOGI(LOG_TAG, "SD log files deleted (if existed).");
#endif

    // === 4. Notify user via LCD ===
    lcd_clear();
    lcd_print_centered(0, "Logs Cleared");
    lcd_print_centered(1, "System Clean");
    vTaskDelay(pdMS_TO_TICKS(1500));

    ESP_LOGI(LOG_TAG, "All logs have been erased successfully.");
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
void lcd_draw_menu_scroll(menu_state_t menu_state, int selection)
{
    // ── 1. Fetch item list ────────────────────────────────────────────────
    int item_count = 0;
    const menu_item_t *items = get_menu_items(menu_state, &item_count);
    if (!items || item_count == 0)
        return;

    // ── 2. Clamp selection to valid range ─────────────────────────────────
    if (selection < 0)
        selection = 0;
    if (selection >= item_count)
        selection = item_count - 1;

    // ── 3. Build Row 0: ">Label           " (exactly LCD_COLS chars) ──────
    char line1[LCD_COLS + 1];
    // Format: 1 arrow char + up to (LCD_COLS-1) label chars, space-padded
    snprintf(line1, sizeof(line1), "%c%-*.*s",
             MENU_ARROW,
             LCD_COLS - 1, LCD_COLS - 1,
             items[selection].label);

    // ── 4. Build Row 1: " Label     X/N " (exactly LCD_COLS chars) ────────
    int next_index = (selection + 1) % item_count;
    char line2[LCD_COLS + 1];

    if (item_count >= MENU_INDICATOR_MIN_ITEMS)
    {
        // Build the indicator string first so we know its exact width
        char indicator[MENU_INDICATOR_MAX_LEN + 1];
        int ind_len = snprintf(indicator, sizeof(indicator),
                               "%d/%d", selection + 1, item_count);

        // Layout: 1 (indent) + label_width + ind_len = LCD_COLS
        int label_width = LCD_COLS - 1 - ind_len;
        if (label_width < 1)
            label_width = 1; // always show at least one label character

        snprintf(line2, sizeof(line2), "%c%-*.*s%s",
                 MENU_INDENT,
                 label_width, label_width,
                 items[next_index].label,
                 indicator);
    }
    else
    {
        // Only 1 or 2 items — no indicator needed
        snprintf(line2, sizeof(line2), "%c%-*.*s",
                 MENU_INDENT,
                 LCD_COLS - 1, LCD_COLS - 1,
                 items[next_index].label);
    }

    // ── 5. Write to LCD ───────────────────────────────────────────────────
    // lcd_set_cursor(col, row) — confirmed from codebase usage pattern
    lcd_set_cursor(0, 0); // col=0, row=0
    lcd_print(line1);

    lcd_set_cursor(1, 0); // col=0, row=1  (was wrong: lcd_set_cursor(1,0))
    lcd_print(line2);
}

// == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == ==
// VALUE EDIT MODE DISPLAY
// ============================================================================

// Display value edit mode screen while editing
void lcd_show_value_edit_screen(void)
{
    ets_printf("LCD: Showing value edit screen\n");

    // Clear the LCD
    lcd_clear();
    vTaskDelay(2 / portTICK_PERIOD_MS);

    // Get current parameter being edited
    value_edit_context_t *config = get_current_value_config();
    float *current_value = get_current_value_pointer();

    if (!config || !current_value)
    {
        lcd_set_cursor(0, 0);
        lcd_print("Error: No param");
        return;
    }

    // ===== ROW 0: Parameter name =====
    lcd_set_cursor(0, 0);
    char param_str[17] = {0};
    snprintf(param_str, sizeof(param_str), "%s", config->label);
    lcd_print(param_str);

    // Pad with spaces
    int len = strlen(param_str);
    for (int i = len; i < 16; i++)
    {
        lcd_print_char(' ');
    }

    // ===== ROW 1: Current value with unit =====
    lcd_set_cursor(0, 1);
    char value_str[17] = {0};

    if (sys_state.pending_confirmation)
    {
        // Show confirmation prompt
        snprintf(value_str, sizeof(value_str), "Confirm? ENT/NO");
    }
    else
    {
        // Show current value
        snprintf(value_str, sizeof(value_str), "%.2f %s", *current_value, config->unit);
    }

    lcd_print(value_str);

    // Pad with spaces
    len = strlen(value_str);
    for (int i = len; i < 16; i++)
    {
        lcd_print(' ');
    }
}

void lcd_show_bt_connecting_screen(const char *device_name)
{
    lcd_clear();
    lcd_print_centered(0, "BT Connecting");
    lcd_set_cursor(0, 1);
    lcd_print_centered(1, device_name);
}

void lcd_show_factory_reset_screen(void)
{
    button_event_info_t event_info;
    bool waiting_for_input = true;
    int64_t entry_time = esp_timer_get_time() / 1000; // ms
    lcd_clear();
    lcd_print_centered(0, "Factory Reset?");
    lcd_set_cursor(0, 1);
    lcd_print("OK=Yes  BACK=No");

    while (waiting_for_input)
    {
        // Wait for button event (timeout = 50 ms for responsiveness)
        if (xQueueReceive(button_event_queue, &event_info, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            if (event_info.event == BUTTON_EVENT_CLICK)
            {
                switch (event_info.button_id)
                {
                case BTN_ENTER_MENU:
                    // Confirm reset
                    lcd_clear();
                    lcd_print_centered(0, "Resetting...");
                    perform_factory_reset();
                    lcd_clear();
                    lcd_print_centered(0, "Reset Done!");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    sys_state.menu_state = MAIN_MENU;
                    lcd_update_menu_screen();
                    waiting_for_input = false;
                    break;

                case BTN_BACK:
                    // Cancel reset
                    lcd_clear();
                    lcd_print_centered(0, "Cancelled");
                    vTaskDelay(pdMS_TO_TICKS(800));
                    sys_state.menu_state = MAIN_MENU;
                    lcd_update_menu_screen();
                    waiting_for_input = false;
                    break;

                default:
                    break;
                }
            }
        }

        // Optional: timeout after inactivity
        if ((esp_timer_get_time() / 1000 - entry_time) > 15000) // 15s
        {
            lcd_clear();
            lcd_print_centered(0, "Timeout");
            vTaskDelay(pdMS_TO_TICKS(800));
            sys_state.menu_state = MAIN_MENU;
            lcd_update_menu_screen();
            waiting_for_input = false;
        }
    }
}

/**
 * @brief Display Bluetooth edit screen for a configurable parameter.
 *
 * @param label The label of the parameter (e.g., "Device Name")
 * @param value The current value of the parameter (string)
 */

static inline void lcd_lock(void)
{
    if (lcd_task_handle != NULL)
        vTaskSuspend(lcd_task_handle);
}

static inline void lcd_unlock(void)
{
    if (lcd_task_handle != NULL)
        vTaskResume(lcd_task_handle);
}

void lcd_show_bt_edit_screen(const char *label, const char *value)
{
    lcd_clear();

    // First line: show which Bluetooth setting we’re editing
    char title[LCD_COLS + 1];
    snprintf(title, sizeof(title), "%s:", label);
    lcd_set_cursor(0, 0);
    lcd_print_centered(0, title);

    // Second line: show current or editable value
    lcd_set_cursor(1, 0);

    if (strlen(value) > LCD_COLS)
    {
        // If value is too long, show only the first 15 chars and ">"
        char truncated[LCD_COLS + 1];
        strncpy(truncated, value, LCD_COLS - 1);
        truncated[LCD_COLS - 1] = '>';
        truncated[LCD_COLS] = '\0';
        lcd_print(truncated);
    }
    else
    {
        lcd_print(value);
    }

    // Optional: blinking cursor or indicator for edit mode
    // This can be controlled externally (not in this static function)
}

/* ----------------------------- LCD UI ----------------------------- */
// Display value saved confirmation screen
void lcd_show_value_saved_screen(void)
{
    ets_printf("LCD: Showing value saved confirmation\n");

    lcd_clear();
    vTaskDelay(2 / portTICK_PERIOD_MS);

    // ===== ROW 0: Confirmation message =====
    lcd_set_cursor(0, 0);
    lcd_print("Value Saved!");

    // Pad with spaces
    for (int i = 11; i < 16; i++)
    {
        lcd_print(' ');
    }

    // ===== ROW 1: Parameter name or status =====
    lcd_set_cursor(0, 1);

    value_edit_context_t *config = get_current_value_config();
    float *current_value = get_current_value_pointer();

    if (config && current_value)
    {
        char saved_str[17] = {0};
        snprintf(saved_str, sizeof(saved_str), "%.2f %s", *current_value, config->unit);
        lcd_print(saved_str);

        int len = strlen(saved_str);
        for (int i = len; i < 16; i++)
        {
            lcd_print(' ');
        }
    }
}

// Display value edit canceled screen
void lcd_show_value_canceled_screen(void)
{
    ets_printf("LCD: Showing edit canceled screen\n");

    lcd_clear();
    // ===== ROW 0: Canceled message =====
    lcd_set_cursor(0, 0);
    lcd_print("Edit Canceled");
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

// Exit detail view and return to parent menu
void exit_detail_view(void)
{
    if (!sys_state.in_detail_view)
    {
        ets_printf("Warning: exit_detail_view called but not in detail view\n");
        return;
    }

    sys_state.in_detail_view = false;
    sys_state.inverter.inverter_state = sys_state.pre_detail_inverter_state;
    sys_state.menu_state = sys_state.detail_parent_menu;
    sys_state.menu_selection = sys_state.detail_parent_selection;

    ets_printf("Exited detail view, returning to menu %d, selection %d\n",
               sys_state.menu_state, sys_state.menu_selection);

    // Draw before resuming so there is no blank gap
    lcd_clear();
    lcd_draw_menu_scroll(sys_state.menu_state,
                         sys_state.menu_selection);

    if (lcd_task_handle != NULL)
        vTaskResume(lcd_task_handle);
}

// Enter submenu with history tracking
void enter_submenu(menu_state_t new_state)
{
    // Save current state before navigating
    push_menu_history(sys_state.menu_state, sys_state.menu_selection);

    // Navigate to new menu
    sys_state.menu_state = new_state;
    sys_state.menu_selection = 0;

    ets_printf("Entered submenu %d (history depth=%d)\n", new_state, menu_history.depth);
    lcd_clear();
    lcd_draw_menu_scroll(sys_state.menu_state,
                         sys_state.menu_selection);
}

// Power button event handler
void handle_power_button_event(const button_event_info_t *event_info, void *user_data)
{
    if (!sys_state.system_ready)
    {
        ets_printf("Power button: system not ready, ignoring\n");
        return;
    }

    int64_t current_time = event_info->timestamp_us / 1000;

    switch (event_info->event)
    {
    /* =========================================================================
     * SINGLE CLICK
     * Priority: value edit → confirmation → detail view → diagnostic →
     *           open menu → contextual main screen
     * ======================================================================= */
    case BUTTON_EVENT_CLICK:
    {
        ets_printf("Power button: click (inv=%d edit=%d detail=%d menu=%d)\n",
                   sys_state.inverter.inverter_state,
                   sys_state.value_edit_mode,
                   sys_state.in_detail_view,
                   sys_state.menu_state);

        // ── P1: Cancel value edit, restore backup, return to owning menu ──────
        if (sys_state.value_edit_mode)
        {
            ets_printf("Power button: cancelling value edit\n");
            if (sys_state.value_changed)
            {
                float *val = get_current_value_pointer();
                if (val)
                    *val = sys_state.edit_backup_value;
            }
            sys_state.value_edit_mode = false;
            sys_state.value_changed = false;
            sys_state.pending_confirmation = false;

            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_print("Edit Cancelled  ");
            lcd_set_cursor(0, 1);
            lcd_print("                ");
            vTaskDelay(pdMS_TO_TICKS(600));
            lcd_draw_menu_scroll(sys_state.menu_state, sys_state.menu_selection);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
            break;
        }

        // ── P2: Cancel confirmation screen ────────────────────────────────────
        if (sys_state.in_confirmation_screen)
        {
            ets_printf("Power button: cancelling confirmation\n");
            sys_state.in_confirmation_screen = false;

            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_clear();
            lcd_draw_menu_scroll(sys_state.menu_state, sys_state.menu_selection);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
            break;
        }

        // ── P3: Exit detail view ──────────────────────────────────────────────
        // CRITICAL: if we are returning to a diagnostic session, lcd_task must
        // NOT be resumed — enter_diagnostic_mode() owns the suspension and only
        // exit_diagnostic_mode() may resume it.
        if (sys_state.in_detail_view)
        {
            ets_printf("Power button: exiting detail view (parent=%d)\n",
                       sys_state.detail_parent_menu);

            bool returning_to_diagnostic =
                (sys_state.pre_detail_inverter_state == INVERTER_DIAGNOSTIC);

            sys_state.in_detail_view = false;
            sys_state.inverter.inverter_state = sys_state.pre_detail_inverter_state;
            sys_state.menu_state = sys_state.detail_parent_menu;
            sys_state.menu_selection = sys_state.detail_parent_selection;

            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_clear();
            lcd_draw_menu_scroll(sys_state.menu_state, sys_state.menu_selection);

            // Only resume if we are NOT back inside a diagnostic session.
            if (!returning_to_diagnostic)
            {
                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
            }
            // If returning_to_diagnostic, lcd_task stays suspended.
            // The next single-click will hit P4 and call exit_diagnostic_mode()
            // which is the only legal place to resume it.
            break;
        }

        // ── P4: Exit diagnostic mode ──────────────────────────────────────────
        if (sys_state.inverter.inverter_state == INVERTER_DIAGNOSTIC)
        {
            ets_printf("Power button: single-click — exiting diagnostic mode\n");
            exit_diagnostic_mode();
            sys_state.power_button_sequence_count = 0;
            break;
        }

        // ── P5: Close any open menu and return to main screen ─────────────────
        if (sys_state.menu_state != MENU_NONE)
        {
            ets_printf("Power button: closing menu (state=%d)\n",
                       sys_state.menu_state);

            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
            sys_state.in_detail_view = false;
            clear_menu_history();

            sync_lcd_state();
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_clear();
            lcd_draw_main_screen(&sys_state.lcd_state);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
            break;
        }

        // ── P6: No menu open — contextual main screen based on inverter state ─
        sys_state.power_button_sequence_count = 0;
        clear_menu_history();

        switch (sys_state.inverter.inverter_state)
        {
        case INVERTER_ON:
        case INVERTER_STARTING:
            ets_printf("Power button: inverter live — refresh main screen\n");
            sys_state.lcd_state.current_screen = LCD_SCREEN_MAIN;
            sync_lcd_state();
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_clear();
            lcd_draw_main_screen(&sys_state.lcd_state);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
            break;

        case INVERTER_STANDBY:
            ets_printf("Power button: standby summary\n");
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_clear();
            {
                char row0[17], row1[17];
                snprintf(row0, sizeof(row0), "STANDBY %4.1fV   ",
                         sys_state.inverter.battery.voltage);
                uint8_t pct = calculate_battery_percentage(
                    sys_state.inverter.battery.voltage);
                snprintf(row1, sizeof(row1), "BAT:%3d%% AC:%s ",
                         pct, sys_state.inverter.connected ? "YES" : "NO ");
                lcd_set_cursor(0, 0);
                lcd_print(row0);
                lcd_set_cursor(0, 1);
                lcd_print(row1);
            }
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
            break;

        case INVERTER_FAULT:
            ets_printf("Power button: fault active — show fault then main screen\n");
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_print("** FAULT ACTIVE ");
            {
                char row1[17];
                snprintf(row1, sizeof(row1), "%-16.16s",
                         get_error_string(sys_state.error.error_flags));
                lcd_set_cursor(0, 1);
                lcd_print(row1);
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            sync_lcd_state();
            lcd_clear();
            lcd_draw_main_screen(&sys_state.lcd_state);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
            break;

        case INVERTER_OFF:
        default:
            ets_printf("Power button: idle — main screen\n");
            sys_state.lcd_state.current_screen = LCD_SCREEN_MAIN;
            sync_lcd_state();
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_clear();
            lcd_draw_main_screen(&sys_state.lcd_state);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
            break;
        }
        break; // end BUTTON_EVENT_CLICK
    }

    /* =========================================================================
     * DOUBLE CLICK — toggle diagnostic mode
     * Blocked while inverter is live or while editing a value.
     * FIX: do NOT set menu_state = MENU_NONE before enter_diagnostic_mode().
     *      enter_diagnostic_mode() sets MENU_DIAGNOSTIC itself; touching
     *      menu_state first created a race window where lcd_task drew the
     *      main screen between the two writes.
     * ======================================================================= */
    case BUTTON_EVENT_DOUBLE_CLICK:
    {
        ets_printf("Power button: double-click (inv=%d)\n",
                   sys_state.inverter.inverter_state);

        if (sys_state.value_edit_mode)
        {
            ets_printf("Power button: double-click blocked — value edit active\n");
            break;
        }

        if (sys_state.inverter.inverter_state == INVERTER_ON ||
            sys_state.inverter.inverter_state == INVERTER_STARTING)
        {
            ets_printf("Power button: double-click blocked — inverter running\n");
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_print("Stop inverter   ");
            lcd_set_cursor(0, 1);
            lcd_print("before diag!    ");
            vTaskDelay(pdMS_TO_TICKS(1500));
            sync_lcd_state();
            lcd_clear();
            lcd_draw_main_screen(&sys_state.lcd_state);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
            break;
        }

        if (sys_state.inverter.inverter_state != INVERTER_DIAGNOSTIC)
        {
            ets_printf("Power button: entering diagnostic mode\n");

            // Cancel active sessions cleanly. Do NOT pre-set menu_state —
            // enter_diagnostic_mode() owns that write.
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
            ets_printf("Power button: exiting diagnostic mode\n");
            exit_diagnostic_mode();
        }

        sys_state.power_button_sequence_count = 0;
        break;
    }

    /* =========================================================================
     * TRIPLE CLICK — factory reset confirmation screen
     * Blocked while inverter is running.
     * ======================================================================= */
    case BUTTON_EVENT_TRIPLE_CLICK:
    {
        ets_printf("Power button: triple-click\n");

        if (sys_state.inverter.inverter_state == INVERTER_ON ||
            sys_state.inverter.inverter_state == INVERTER_STARTING)
        {
            ets_printf("Power button: factory reset blocked — inverter running\n");
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_print("Stop inverter   ");
            lcd_set_cursor(0, 1);
            lcd_print("before reset!   ");
            vTaskDelay(pdMS_TO_TICKS(1500));
            sync_lcd_state();
            lcd_clear();
            lcd_draw_main_screen(&sys_state.lcd_state);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
            break;
        }

        // Exit any active session cleanly before showing reset confirmation
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

        if (lcd_task_handle != NULL)
            vTaskSuspend(lcd_task_handle);
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_print("FACTORY RESET?  ");
        lcd_set_cursor(0, 1);
        lcd_print("Hold=Yes Back=No");
        // lcd_task stays suspended — user must hold or press Back
        break;
    }

    /* =========================================================================
     * LONG PRESS — start / stop inverter, or apply value / confirm reset
     * ======================================================================= */
    case BUTTON_EVENT_LONG_PRESS:
    {
        ets_printf("Power button: long press (inv=%d menu=%d edit=%d)\n",
                   sys_state.inverter.inverter_state,
                   sys_state.menu_state,
                   sys_state.value_edit_mode);

        // Long press in value edit mode → apply current value and stay in editor
        if (sys_state.value_edit_mode)
        {
            ets_printf("Power button: long press — applying value change\n");
            apply_value_change();
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_show_value_edit_screen();
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
            break;
        }

        // Long press on factory reset confirmation screen → execute reset
        if (sys_state.menu_state == MENU_FACTORY_RESET_CONFIRM &&
            sys_state.power_button_sequence_count > 0)
        {
            ets_printf("Power button: long press — confirming factory reset\n");
            sys_state.power_button_sequence_count = 0;
            clear_menu_history();

            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_print("Resetting...    ");
            lcd_set_cursor(0, 1);
            lcd_print("Please wait     ");
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);

            vTaskDelay(pdMS_TO_TICKS(500));
            perform_factory_reset();
            break;
        }

        // Long press in diagnostic mode → exit diagnostic, don't start inverter
        if (sys_state.inverter.inverter_state == INVERTER_DIAGNOSTIC)
        {
            ets_printf("Power button: long press — exiting diagnostic mode\n");
            exit_diagnostic_mode();
            break;
        }

        // Close any open menu before acting on inverter
        if (sys_state.menu_state != MENU_NONE)
        {
            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
            sys_state.in_detail_view = false;
            clear_menu_history();
        }

        // Start / stop inverter based on current state
        switch (sys_state.inverter.inverter_state)
        {
        case INVERTER_OFF:
        case INVERTER_STANDBY:
            ets_printf("Power button: long press — requesting inverter start\n");

            inverter_power_on();
            break;

        case INVERTER_ON:
        case INVERTER_STARTING:
            ets_printf("Power button: long press — requesting shutdown\n");
            shutdown_inverter();
            gpio_set_level(GPIO_POWER_RELAY, 0);
            break;

        case INVERTER_FAULT:
            ets_printf("Power button: long press — clearing fault and restarting\n");
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_print("Clearing fault  ");
            lcd_set_cursor(0, 1);
            lcd_print("Please wait...  ");
            vTaskDelay(pdMS_TO_TICKS(1000));

            sys_state.error.error_flags &= (ERR_EEPROM | ERR_FAN_FAIL);
            sys_state.inverter.inverter_state = INVERTER_OFF;

            if (check_safety_conditions())
            {
                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
                inverter_power_on();
                if (sys_state.inverter.inverter_state == INVERTER_ON)
                    gpio_set_level(GPIO_POWER_RELAY, 1);
            }
            else
            {
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_print("Fault persists  ");
                lcd_set_cursor(0, 1);
                lcd_print("Check system!   ");
                buzzer_error();
                vTaskDelay(pdMS_TO_TICKS(2000));
                sync_lcd_state();
                lcd_clear();
                lcd_draw_main_screen(&sys_state.lcd_state);
                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
            }
            break;

        default:
            ets_printf("Power button: long press — no action for state %d\n",
                       sys_state.inverter.inverter_state);
            break;
        }
        break; // end BUTTON_EVENT_LONG_PRESS
    }

    /* =========================================================================
     * VERY LONG PRESS — emergency hard shutdown
     * Only acts when inverter is live.
     * ======================================================================= */
    case BUTTON_EVENT_VERY_LONG_PRESS:
    {
        ets_printf("Power button: very long press\n");

        if (sys_state.inverter.inverter_state != INVERTER_ON &&
            sys_state.inverter.inverter_state != INVERTER_STARTING &&
            sys_state.inverter.inverter_state != INVERTER_FAULT)
        {
            ets_printf("Power button: emergency shutdown ignored — inverter not active\n");
            break;
        }

        ets_printf("Power button: EMERGENCY SHUTDOWN\n");

        if (lcd_task_handle != NULL)
            vTaskSuspend(lcd_task_handle);

        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_print("!! EMERGENCY !! ");
        lcd_set_cursor(0, 1);
        lcd_print("SYSTEM HALT     ");
        buzzer_error();
        vTaskDelay(pdMS_TO_TICKS(500));

        gpio_set_level(GPIO_POWER_RELAY, 0);
        inverter_emergency_shutdown();

        // Sanitise all state so lcd_task resumes to a clean main screen
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

        sync_lcd_state();
        lcd_clear();
        lcd_draw_main_screen(&sys_state.lcd_state);
        led_off(LED_STATUS);

        if (lcd_task_handle != NULL)
            vTaskResume(lcd_task_handle);

        ESP_LOGW("POWER", "Emergency shutdown complete");
        break;
    }

    default:
        break;
    } // end outer switch

    /* =========================================================================
     * FACTORY RESET CONFIRMATION TIMEOUT
     * Auto-cancel if user takes no action within SEQUENCE_TIMEOUT_MS.
     * ======================================================================= */
    if (sys_state.power_button_sequence_count > 0)
    {
        if ((current_time - sys_state.power_sequence_start_time) > SEQUENCE_TIMEOUT_MS)
        {
            ets_printf("Power button: factory reset timed out — cancelling\n");
            sys_state.power_button_sequence_count = 0;

            menu_state_t prev_state;
            int prev_sel;
            if (pop_menu_history(&prev_state, &prev_sel))
            {
                sys_state.menu_state = prev_state;
                sys_state.menu_selection = prev_sel;
            }
            else
            {
                sys_state.menu_state = MENU_NONE;
                sys_state.menu_selection = 0;
            }

            sync_lcd_state();
            lcd_clear();
            lcd_draw_main_screen(&sys_state.lcd_state);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
        }
    }

    sys_state.last_activity_time = current_time;
}

menu_state_t display_menu_state()
{
    lcd_clear();
    switch (sys_state.menu_state)
    {
    case MENU_NONE:
        lcd_print_centered(0, "Main Screen");
        lcd_print_centered(1, "Press Enter");
        break;
    case MAIN_MENU:
        lcd_print_centered(0, "Main Menu");
        lcd_print_centered(1, "Use Up/Down");
        break;
    case MENU_SETTINGS:
        lcd_print_centered(0, "Settings");
        lcd_print_centered(1, "Select Option");
        break;
    case MENU_MONITORING:
        lcd_print_centered(0, "Monitoring");
        lcd_print_centered(1, "View Stats");
        break;
    case MENU_DIAGNOSTIC:
        lcd_print_centered(0, "Diagnostic");
        lcd_print_centered(1, "Run Tests");
        break;
    case MENU_FACTORY_RESET_CONFIRM:
        lcd_print_centered(0, "Factory Reset?");
        lcd_print_centered(1, "Enter=Yes Back=No");
        break;
    case MENU_WIFI_CONFIG:
        lcd_print_centered(0, "WiFi Config");
        lcd_print_centered(1, "Enter=Setup Back=No");
        break;
    default:
        lcd_print_centered(0, "Unknown Menu");
        lcd_print_centered(1, "");
        break;
    }
    vTaskDelay(pdMS_TO_TICKS(1500)); // Allow time for user to see the menu
    return sys_state.menu_state;
}

// == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == == ==
// BUTTON EVENT HANDLERS - WHERE ALL LCD FUNCTIONS ARE CALLED
// ============================================================================

// ENTER/MENU BUTTON - Navigate menus and enter/confirm values
void handle_enter_menu_button_event(const button_event_info_t *event_info, void *user_data)
{
    // Guard: System not ready
    if (!sys_state.system_ready)
    {
        ets_printf("Enter/Menu button: System not ready, ignoring input\n");
        return;
    }

    // NOTE: Do NOT suspend lcd_task here globally. Each case that needs
    // to write to the LCD handles suspend/resume itself, so the task is
    // only paused for the minimum time required.

    switch (event_info->event)
    {
    /* ======================================================================
     * SINGLE CLICK
     * ==================================================================== */
    case BUTTON_EVENT_CLICK:
        ets_printf("Enter/Menu button: Single click (state=%d, sel=%d)\n",
                   sys_state.menu_state, sys_state.menu_selection);

        // === PRIORITY 1: Value edit mode — save the current value ===
        if (sys_state.value_edit_mode)
        {
            ets_printf("Enter/Menu: In value edit mode, saving...\n");

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

            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_show_value_saved_screen();
            vTaskDelay(pdMS_TO_TICKS(800));
            lcd_draw_menu_scroll(sys_state.menu_state, sys_state.menu_selection);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
        }

        // === PRIORITY 2: Menu navigation ===
        else
        {
            switch (sys_state.menu_state)
            {
            // ── Main menu → enter selected submenu ───────────────────────
            case MAIN_MENU:
            {
                menu_state_t next_state = MENU_NONE;
                switch (sys_state.menu_selection)
                {
                case 0:
                    next_state = MENU_SETTINGS;
                    break;
                case 1:
                    next_state = MENU_MONITORING;
                    break;
                case 2:
                    next_state = MENU_DIAGNOSTIC;
                    break;
                case 3:
                    next_state = MENU_WIFI_CONFIG;
                    break;
                case 4:
                    next_state = MENU_FACTORY_RESET_CONFIRM;
                    break;
                default:
                    ets_printf("Enter/Menu: Invalid main menu selection %d\n",
                               sys_state.menu_selection);
                    break;
                }

                if (next_state != MENU_NONE)
                {
                    ets_printf("Enter/Menu: Entering submenu %d\n", next_state);
                    push_menu_history(sys_state.menu_state,
                                      sys_state.menu_selection);
                    sys_state.menu_state = next_state;
                    sys_state.menu_selection = 0;

                    if (lcd_task_handle != NULL)
                        vTaskSuspend(lcd_task_handle);
                    lcd_draw_menu_scroll(next_state, 0);
                    if (lcd_task_handle != NULL)
                        vTaskResume(lcd_task_handle);
                }
                break;
            }

            // ── Settings → enter value edit mode for selected item ────────
            case MENU_SETTINGS:
                ets_printf("Enter/Menu: Settings item %d — entering edit mode\n",
                           sys_state.menu_selection);

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
                default:
                    ets_printf("Enter/Menu: Invalid settings selection %d\n",
                               sys_state.menu_selection);
                    break;
                }
                // edit_* functions write to LCD themselves and set
                // value_edit_mode — no suspend/resume needed here.
                sys_state.value_edit_mode = true;
                break;

            // ── Monitoring → show detail for selected item ────────────────
            case MENU_MONITORING:
                ets_printf("Enter/Menu: Monitoring item %d\n",
                           sys_state.menu_selection);

                enter_detail_view(MENU_MONITORING, sys_state.menu_selection);

                if (lcd_task_handle != NULL)
                    vTaskSuspend(lcd_task_handle);

                switch (sys_state.menu_selection)
                {
                case 0:
                    lcd_show_monitoring_detail("Voltage",
                                               sys_state.inverter.output_voltage, "V");
                    break;
                case 1:
                    lcd_show_monitoring_detail("Current",
                                               sys_state.actual_current, "A");
                    break;
                case 2:
                    lcd_show_monitoring_detail("Frequency",
                                               sys_state.inverter.output_frequency, "Hz");
                    break;
                case 3:
                    lcd_show_monitoring_detail("Temperature",
                                               sys_state.actual_temperature, "C");
                    break;
                case 4:
                    lcd_show_monitoring_detail("Power Factor",
                                               sys_state.power_factor, "");
                    break;
                case 5:
                    lcd_show_monitoring_detail("Efficiency",
                                               sys_state.efficiency, "%");
                    break;
                default:
                    ets_printf("Enter/Menu: Invalid monitoring selection %d\n",
                               sys_state.menu_selection);
                    sys_state.in_detail_view = false;
                    lcd_draw_menu_scroll(MENU_MONITORING,
                                         sys_state.menu_selection);
                    break;
                }

                // Resume immediately — monitoring detail is a static snapshot,
                // not a live session like diagnostics.
                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
                break;

            // ── Diagnostic → enter diagnostic session ─────────────────────
            case MENU_DIAGNOSTIC:
            {
                int item_count = 0;
                const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC,
                                                          &item_count);
                if (items == NULL ||
                    sys_state.menu_selection >= (uint8_t)item_count)
                {
                    ets_printf("Enter/Menu: Invalid diagnostic selection\n");
                    break;
                }

                enter_detail_view(MENU_DIAGNOSTIC, sys_state.menu_selection);

                // Suspend and keep suspended — the diagnostic session owns
                // the display until exit_diagnostic_mode() resumes it.
                if (lcd_task_handle != NULL)
                    vTaskSuspend(lcd_task_handle);

                lcd_draw_diagnostics_screen(sys_state.menu_selection);
                // Do NOT vTaskResume here.
                break;
            }

            // ── WiFi config → show item preview ───────────────────────────
            case MENU_WIFI_CONFIG:
                ets_printf("Enter/Menu: WiFi item %d (long press to configure)\n",
                           sys_state.menu_selection);

                if (lcd_task_handle != NULL)
                    vTaskSuspend(lcd_task_handle);
                lcd_draw_menu_scroll(MENU_WIFI_CONFIG, sys_state.menu_selection);
                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
                break;

            // ── Factory reset → require long press to execute ─────────────
            case MENU_FACTORY_RESET_CONFIRM:
                ets_printf("Enter/Menu: Factory reset — hold to confirm\n");

                if (lcd_task_handle != NULL)
                    vTaskSuspend(lcd_task_handle);
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_print("Hold to confirm ");
                lcd_set_cursor(1, 0);
                lcd_print("factory reset   ");
                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
                break;

            default:
                ets_printf("Enter/Menu: Unknown menu state %d\n",
                           sys_state.menu_state);
                break;
            }
        }
        break; /* end BUTTON_EVENT_CLICK */

    /* ======================================================================
     * LONG PRESS
     * ==================================================================== */
    case BUTTON_EVENT_LONG_PRESS:
        ets_printf("Enter/Menu: Long press (state=%d, sel=%d)\n",
                   sys_state.menu_state, sys_state.menu_selection);

        if (sys_state.value_edit_mode)
        {
            if (sys_state.pending_confirmation)
                handle_value_confirmation();
            else
                apply_value_change();

            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_show_value_edit_screen();
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
        }
        else
        {
            switch (sys_state.menu_state)
            {
            case MENU_NONE:
                sys_state.menu_state = (sys_state.inverter.inverter_state == INVERTER_ON)
                                           ? MENU_MONITORING
                                           : MAIN_MENU;
                sys_state.menu_selection = 0;

                if (lcd_task_handle != NULL)
                    vTaskSuspend(lcd_task_handle);
                lcd_draw_menu_scroll(sys_state.menu_state, 0);
                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
                break;

            case MAIN_MENU:
            {
                menu_state_t next_state = MENU_NONE;
                switch (sys_state.menu_selection)
                {
                case 0:
                    next_state = MENU_SETTINGS;
                    break;
                case 1:
                    next_state = MENU_MONITORING;
                    break;
                case 2:
                    next_state = MENU_DIAGNOSTIC;
                    break;
                case 3:
                    next_state = MENU_WIFI_CONFIG;
                    break;
                case 4:
                    next_state = MENU_FACTORY_RESET_CONFIRM;
                    break;
                default:
                    break;
                }
                if (next_state != MENU_NONE)
                {
                    push_menu_history(sys_state.menu_state,
                                      sys_state.menu_selection);
                    sys_state.menu_state = next_state;
                    sys_state.menu_selection = 0;

                    if (lcd_task_handle != NULL)
                        vTaskSuspend(lcd_task_handle);
                    lcd_draw_menu_scroll(next_state, 0);
                    if (lcd_task_handle != NULL)
                        vTaskResume(lcd_task_handle);
                }
                break;
            }

            case MENU_SETTINGS:
                ets_printf("Enter/Menu: Long press settings item %d\n",
                           sys_state.menu_selection);
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
                default:
                    break;
                }
                sys_state.value_edit_mode = true;
                break;

            case MENU_MONITORING:
                ets_printf("Enter/Menu: Long press monitoring item %d\n",
                           sys_state.menu_selection);

                enter_detail_view(MENU_MONITORING, sys_state.menu_selection);

                if (lcd_task_handle != NULL)
                    vTaskSuspend(lcd_task_handle);

                switch (sys_state.menu_selection)
                {
                case 0:
                    lcd_show_monitoring_detail("Voltage",
                                               sys_state.inverter.output_voltage, "V");
                    break;
                case 1:
                    lcd_show_monitoring_detail("Current",
                                               sys_state.actual_current, "A");
                    break;
                case 2:
                    lcd_show_monitoring_detail("Frequency",
                                               sys_state.inverter.output_frequency, "Hz");
                    break;
                case 3:
                    lcd_show_monitoring_detail("Temperature",
                                               sys_state.actual_temperature, "C");
                    break;
                case 4:
                    lcd_show_monitoring_detail("Power Factor",
                                               sys_state.power_factor, "");
                    break;
                case 5:
                    lcd_show_monitoring_detail("Efficiency",
                                               sys_state.efficiency, "%");
                    break;
                default:
                    sys_state.in_detail_view = false;
                    lcd_draw_menu_scroll(MENU_MONITORING,
                                         sys_state.menu_selection);
                    break;
                }

                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
                break;

            case MENU_DIAGNOSTIC:
            {
                int item_count = 0;
                const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC,
                                                          &item_count);
                if (items == NULL ||
                    sys_state.menu_selection >= (uint8_t)item_count)
                    break;

                enter_detail_view(MENU_DIAGNOSTIC, sys_state.menu_selection);

                if (lcd_task_handle != NULL)
                    vTaskSuspend(lcd_task_handle);

                lcd_draw_diagnostics_screen(sys_state.menu_selection);
                // Do NOT resume — diagnostic session owns display.
                break;
            }

            case MENU_WIFI_CONFIG:
                switch (sys_state.menu_selection)
                {
                case 5:
                    if (lcd_task_handle != NULL)
                        vTaskSuspend(lcd_task_handle);
                    lcd_clear();
                    lcd_set_cursor(0, 0);
                    lcd_print("Scanning...     ");
                    lcd_set_cursor(0, 1);
                    lcd_print("Please wait     ");
                    if (lcd_task_handle != NULL)
                        vTaskResume(lcd_task_handle);
                    start_wifi_scan();
                    lcd_show_wifi_scan_screen();
                    break;
                case 6:
                    start_wifi_connection();
                    break;
                default:
                    ets_printf("Enter/Menu: WiFi item %d — edit not implemented\n",
                               sys_state.menu_selection);
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

                if (lcd_task_handle != NULL)
                    vTaskSuspend(lcd_task_handle);
                lcd_draw_menu_scroll(MAIN_MENU, 0);
                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
                break;

            default:
                ets_printf("Enter/Menu: Long press — unknown state %d\n",
                           sys_state.menu_state);
                break;
            }
        }
        break; /* end BUTTON_EVENT_LONG_PRESS */

    /* ======================================================================
     * DOUBLE CLICK
     * ==================================================================== */
    case BUTTON_EVENT_DOUBLE_CLICK:
        ets_printf("Enter/Menu: Double click\n");

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

                if (lcd_task_handle != NULL)
                    vTaskSuspend(lcd_task_handle);
                lcd_show_value_saved_screen();
                vTaskDelay(pdMS_TO_TICKS(800));
                lcd_draw_menu_scroll(sys_state.menu_state,
                                     sys_state.menu_selection);
                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
            }
        }
        else if (sys_state.menu_state == MAIN_MENU)
        {
            push_menu_history(sys_state.menu_state, sys_state.menu_selection);
            sys_state.menu_state = MENU_MONITORING;
            sys_state.menu_selection = 0;

            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_draw_menu_scroll(MENU_MONITORING, 0);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
        }
        break; /* end BUTTON_EVENT_DOUBLE_CLICK */

    default:
        break;
    }

    sys_state.last_activity_time = event_info->timestamp_us / 1000;
}

// Up button event handler - Advanced value increase
void handle_up_button_event(const button_event_info_t *event_info, void *user_data)
{
    if (!sys_state.system_ready)
    {
        ets_printf("Up button: System not ready, ignoring input\n");
        return;
    }

    int64_t current_time = event_info->timestamp_us / 1000;

    switch (event_info->event)
    {
    case BUTTON_EVENT_CLICK:
        ets_printf("Up button: Click (edit=%d, detail=%d, confirm=%d, menu=%d)\n",
                   sys_state.value_edit_mode, sys_state.in_detail_view,
                   sys_state.in_confirmation_screen, sys_state.menu_state);

        // === PRIORITY 1: Value edit mode ===
        if (sys_state.value_edit_mode)
        {
            switch (value_edit.edit_type)
            {
            case VALUE_EDIT_NUMERIC:
                increase_value(false, false);
                printf("Up: Numeric increment\n");
                lcd_update_value_edit_screen();
                break;

            case VALUE_EDIT_SELECT:
                value_edit.selection_index++;
                if (value_edit.selection_index >= value_edit.max_selection)
                    value_edit.selection_index = 0;
                printf("Up: Selection next (idx=%d/%d)\n",
                       value_edit.selection_index, value_edit.max_selection);
                lcd_update_value_edit_screen();
                break;

            case VALUE_EDIT_BOOL:
                value_edit.bool_value = !value_edit.bool_value;
                printf("Up: Boolean toggle (val=%d)\n", value_edit.bool_value);
                lcd_update_value_edit_screen();
                break;

            case VALUE_EDIT_LIST:
                value_edit.list_index++;
                if (value_edit.list_index >= value_edit.list_size)
                    value_edit.list_index = 0;
                printf("Up: List next (idx=%d/%d)\n",
                       value_edit.list_index, value_edit.list_size);
                lcd_update_value_edit_screen();
                break;

            default:
                ets_printf("Up: Unknown edit type %d\n", value_edit.edit_type);
                break;
            }
        }

        // === PRIORITY 2: Confirmation screen ===
        else if (sys_state.in_confirmation_screen)
        {
            printf("Up: Toggle confirmation option\n");
        }

        // === PRIORITY 3: Diagnostic detail view ===
        // Diagnostic mode keeps lcd_task suspended for the entire session.
        // Never call vTaskSuspend/Resume here — enter/exit_diagnostic_mode
        // are the only owners of the task suspension in this state.
        else if (sys_state.menu_state == MENU_DIAGNOSTIC && !sys_state.in_detail_view)
        {
            int num_items = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &num_items);
            if (!items || num_items == 0)
                break;
            sys_state.menu_selection = (sys_state.menu_selection == 0)
                                           ? num_items - 1
                                           : sys_state.menu_selection - 1;

            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            sys_state.detail_parent_selection = sys_state.menu_selection;
            sys_state.detail_parent_menu = MENU_DIAGNOSTIC;

            lcd_draw_menu_scroll(sys_state.menu_state,
                                 sys_state.menu_selection);
        }

        // === PRIORITY 4: Other detail views (monitoring etc.) ===
        else if (sys_state.in_detail_view)
        {
            // Detail views for non-diagnostic menus have no up/down action.
            // User must press Back to exit.
            printf("Up: In detail view (%d) - no action\n", sys_state.menu_state);
        }

        // === PRIORITY 5: Info screen ===
        else if (sys_state.in_info_screen)
        {
            printf("Up: In info screen - no action\n");
        }

        // === PRIORITY 6: Normal menu navigation ===
        // lcd_task is running here, so suspend before drawing and
        // resume immediately after to prevent LCD corruption.
        else if (sys_state.menu_state != MENU_NONE)
        {
            int num_items = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state,
                                                      &num_items);
            if (items && num_items > 0)
            {
                sys_state.menu_selection = (sys_state.menu_selection == 0)
                                               ? num_items - 1
                                               : sys_state.menu_selection - 1;

                sys_state.last_activity_time = esp_timer_get_time() / 1000;

                printf("Up: Menu nav - sel=%d/%d (menu=%d)\n",
                       sys_state.menu_selection, num_items,
                       sys_state.menu_state);

                // Suspend → draw → resume atomically so lcd_task cannot
                // overwrite the menu between our writes.
                if (lcd_task_handle != NULL)
                    vTaskSuspend(lcd_task_handle);

                lcd_draw_menu_scroll(sys_state.menu_state,
                                     sys_state.menu_selection);

                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
            }
        }

        // === PRIORITY 7: Main screen — no action ===
        else
        {
            printf("Up: Main screen - no action\n");
        }
        break;

    case BUTTON_EVENT_DOUBLE_CLICK:
        ets_printf("Up button: Double-click\n");

        if (sys_state.value_edit_mode)
        {
            increase_value(false, true);
            printf("Up: Precision increment\n");
            lcd_update_value_edit_screen();
        }
        else if (sys_state.in_detail_view &&
                 sys_state.menu_state == MENU_DIAGNOSTIC)
        {
            // Jump to first diagnostic item
            sys_state.menu_selection = 0;
            sys_state.detail_parent_selection = 0;
            lcd_draw_diagnostics_screen(0);
        }
        else if (sys_state.in_detail_view)
        {
            exit_detail_view();
        }
        else if (sys_state.in_confirmation_screen)
        {
            sys_state.in_confirmation_screen = false;
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_draw_menu_scroll(sys_state.menu_state, sys_state.menu_selection);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
        }
        else if (sys_state.menu_state != MENU_NONE)
        {
            printf("Up: Double-click jump to first\n");
            sys_state.menu_selection = 0;
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_draw_menu_scroll(sys_state.menu_state, sys_state.menu_selection);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
        }
        break;

    case BUTTON_EVENT_LONG_PRESS:
        ets_printf("Up button: Long press\n");

        if (sys_state.value_edit_mode)
        {
            printf("Up: Long press - fast mode\n");
            sys_state.fast_increment_active = true;
            sys_state.repeat_count = 0;
            for (int i = 0; i < 10; i++)
                increase_value(true, false);
            lcd_update_value_edit_screen();
        }
        else if (sys_state.in_detail_view &&
                 sys_state.menu_state == MENU_DIAGNOSTIC)
        {
            // Jump to first diagnostic item on long press
            sys_state.menu_selection = 0;
            sys_state.detail_parent_selection = 0;
            lcd_draw_diagnostics_screen(0);
        }
        else if (sys_state.in_detail_view)
        {
            exit_detail_view();
        }
        else if (sys_state.in_confirmation_screen)
        {
            sys_state.in_confirmation_screen = false;
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_draw_menu_scroll(sys_state.menu_state, sys_state.menu_selection);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
        }
        else if (sys_state.menu_state != MENU_NONE)
        {
            printf("Up: Long press jump to top\n");
            sys_state.menu_selection = 0;
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_draw_menu_scroll(sys_state.menu_state, sys_state.menu_selection);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
        }
        break;

    case BUTTON_EVENT_REPEAT:
        if (sys_state.value_edit_mode)
        {
            sys_state.repeat_count++;
            uint64_t time_since_last = current_time - sys_state.last_increment_time;
            if (time_since_last < FAST_INCREMENT_THRESHOLD_MS)
                sys_state.fast_increment_active = true;

            bool fast_mode = sys_state.fast_increment_active ||
                             (sys_state.repeat_count > 5);
            increase_value(fast_mode, false);
            printf("Up: Repeat (cnt=%d, fast=%d)\n",
                   sys_state.repeat_count, fast_mode);
            lcd_update_value_edit_screen();
        }
        else if (sys_state.in_detail_view &&
                 sys_state.menu_state == MENU_DIAGNOSTIC)
        {
            int num_items = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &num_items);
            if (items && num_items > 0)
            {
                sys_state.menu_selection =
                    (sys_state.menu_selection + 1) % num_items;
                sys_state.detail_parent_selection = sys_state.menu_selection;
                lcd_draw_diagnostics_screen(sys_state.menu_selection);
            }
        }
        else if (sys_state.menu_state != MENU_NONE && !sys_state.in_detail_view)
        {
            int num_items = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state,
                                                      &num_items);
            if (items && num_items > 0)
            {
                sys_state.menu_selection = (sys_state.menu_selection == 0)
                                               ? num_items - 1
                                               : sys_state.menu_selection - 1;

                lcd_draw_menu_scroll(sys_state.menu_state,
                                     sys_state.menu_selection);
            }
        }
        break;

    case BUTTON_EVENT_RELEASE:
        sys_state.repeat_count = 0;
        sys_state.fast_increment_active = false;
        printf("Up: Released - reset acceleration\n");
        break;

    default:
        break;
    }

    sys_state.last_activity_time = current_time;
    sys_state.last_increment_time = current_time;
}

// Down button event handler - Advanced value decrease
void handle_down_button_event(const button_event_info_t *event_info, void *user_data)
{
    if (!sys_state.system_ready)
    {
        ets_printf("Down button: System not ready, ignoring input\n");
        return;
    }

    int64_t current_time = event_info->timestamp_us / 1000;

    switch (event_info->event)
    {
    case BUTTON_EVENT_CLICK:
        ets_printf("Down button: Click (edit=%d, detail=%d, confirm=%d, menu=%d)\n",
                   sys_state.value_edit_mode, sys_state.in_detail_view,
                   sys_state.in_confirmation_screen, sys_state.menu_state);

        // === PRIORITY 1: Value edit mode ===
        if (sys_state.value_edit_mode)
        {
            switch (value_edit.edit_type)
            {
            case VALUE_EDIT_NUMERIC:
                decrease_value(false, false);
                printf("Down: Numeric decrement\n");
                lcd_update_value_edit_screen();
                break;

            case VALUE_EDIT_SELECT:
                if (value_edit.selection_index > 0)
                    value_edit.selection_index--;
                else
                    value_edit.selection_index = value_edit.max_selection - 1;
                printf("Down: Selection prev (idx=%d/%d)\n",
                       value_edit.selection_index, value_edit.max_selection);
                lcd_update_value_edit_screen();
                break;

            case VALUE_EDIT_BOOL:
                value_edit.bool_value = !value_edit.bool_value;
                printf("Down: Boolean toggle (val=%d)\n", value_edit.bool_value);
                lcd_update_value_edit_screen();
                break;

            case VALUE_EDIT_LIST:
                if (value_edit.list_index > 0)
                    value_edit.list_index--;
                else
                    value_edit.list_index = value_edit.list_size - 1;
                printf("Down: List prev (idx=%d/%d)\n",
                       value_edit.list_index, value_edit.list_size);
                lcd_update_value_edit_screen();
                break;

            default:
                ets_printf("Down: Unknown edit type %d\n", value_edit.edit_type);
                break;
            }
        }

        // === PRIORITY 2: Confirmation screen ===
        else if (sys_state.in_confirmation_screen)
        {
            printf("Down: Toggle confirmation option\n");
        }

        // === PRIORITY 3: Diagnostic detail view ===
        // lcd_task is already suspended for the entire diagnostic session.
        // Never call vTaskSuspend/Resume here — enter/exit_diagnostic_mode
        // are the only owners of the task suspension in this state.
        else if (sys_state.menu_state == MENU_DIAGNOSTIC && !sys_state.in_detail_view)
        {
            int num_items = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &num_items);
            if (!items || num_items == 0)
                break;

            // DOWN: scroll backward through diagnostic items with wrap-around
            sys_state.menu_selection =
                (sys_state.menu_selection + 1) % num_items;

            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            sys_state.detail_parent_selection = sys_state.menu_selection;
            sys_state.detail_parent_menu = MENU_DIAGNOSTIC;

            lcd_draw_menu_scroll(sys_state.menu_state,
                                 sys_state.menu_selection);
        }

        // === PRIORITY 4: Other detail views (monitoring etc.) ===
        else if (sys_state.in_detail_view)
        {
            printf("Down: In detail view (%d) - no action\n", sys_state.menu_state);
        }

        // === PRIORITY 5: Info screen ===
        else if (sys_state.in_info_screen)
        {
            printf("Down: In info screen - no action\n");
        }

        // === PRIORITY 6: Normal menu navigation ===
        // lcd_task is running here, so suspend before drawing and
        // resume immediately after to prevent LCD corruption.
        else if (sys_state.menu_state != MENU_NONE)
        {
            int num_items = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state,
                                                      &num_items);
            if (items && num_items > 0)
            {
                // DOWN: scroll forward through menu items with wrap-around
                sys_state.menu_selection =
                    (sys_state.menu_selection + 1) % num_items;

                sys_state.last_activity_time = esp_timer_get_time() / 1000;

                printf("Down: Menu nav - sel=%d/%d (menu=%d)\n",
                       sys_state.menu_selection, num_items,
                       sys_state.menu_state);

                // Suspend → draw → resume atomically so lcd_task cannot
                // overwrite the menu between our writes.
                if (lcd_task_handle != NULL)
                    vTaskSuspend(lcd_task_handle);

                lcd_draw_menu_scroll(sys_state.menu_state,
                                     sys_state.menu_selection);

                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
            }
        }

        // === PRIORITY 7: Main screen — no action ===
        else
        {
            printf("Down: Main screen - no action\n");
        }
        break;

    case BUTTON_EVENT_DOUBLE_CLICK:
        ets_printf("Down button: Double-click\n");

        if (sys_state.value_edit_mode)
        {
            decrease_value(false, true);
            printf("Down: Precision decrement\n");
            lcd_update_value_edit_screen();
        }
        else if (sys_state.in_detail_view &&
                 sys_state.menu_state == MENU_DIAGNOSTIC)
        {
            // Jump to last diagnostic item on double-click
            int num_items = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &num_items);
            if (items && num_items > 0)
            {
                sys_state.menu_selection = num_items - 1;
                sys_state.detail_parent_selection = num_items - 1;
                lcd_draw_diagnostics_screen(sys_state.menu_selection);
            }
        }
        else if (sys_state.in_detail_view)
        {
            exit_detail_view();
        }
        else if (sys_state.in_confirmation_screen)
        {
            sys_state.in_confirmation_screen = false;
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_draw_menu_scroll(sys_state.menu_state, sys_state.menu_selection);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
        }
        else if (sys_state.menu_state != MENU_NONE)
        {
            // Jump to last item in current menu
            int num_items = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state,
                                                      &num_items);
            if (items && num_items > 0)
            {
                printf("Down: Double-click jump to last\n");
                sys_state.menu_selection = num_items - 1;
                if (lcd_task_handle != NULL)
                    vTaskSuspend(lcd_task_handle);
                lcd_draw_menu_scroll(sys_state.menu_state,
                                     sys_state.menu_selection);
                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
            }
        }
        break;

    case BUTTON_EVENT_LONG_PRESS:
        ets_printf("Down button: Long press\n");

        if (sys_state.value_edit_mode)
        {
            printf("Down: Long press - fast mode\n");
            sys_state.fast_increment_active = true;
            sys_state.repeat_count = 0;
            for (int i = 0; i < 10; i++)
                decrease_value(true, false);
            lcd_update_value_edit_screen();
        }
        else if (sys_state.in_detail_view &&
                 sys_state.menu_state == MENU_DIAGNOSTIC)
        {
            // Jump to last diagnostic item on long press
            int num_items = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &num_items);
            if (items && num_items > 0)
            {
                sys_state.menu_selection = num_items - 1;
                sys_state.detail_parent_selection = num_items - 1;
                lcd_draw_diagnostics_screen(sys_state.menu_selection);
            }
        }
        else if (sys_state.in_detail_view)
        {
            exit_detail_view();
        }
        else if (sys_state.in_confirmation_screen)
        {
            sys_state.in_confirmation_screen = false;
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            lcd_draw_menu_scroll(sys_state.menu_state, sys_state.menu_selection);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
        }
        else if (sys_state.menu_state != MENU_NONE)
        {
            // Jump to bottom of current menu
            int num_items = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state,
                                                      &num_items);
            if (items && num_items > 0)
            {
                printf("Down: Long press jump to bottom\n");
                sys_state.menu_selection = num_items - 1;
                if (lcd_task_handle != NULL)
                    vTaskSuspend(lcd_task_handle);
                lcd_draw_menu_scroll(sys_state.menu_state,
                                     sys_state.menu_selection);
                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
            }
        }
        break;

    case BUTTON_EVENT_REPEAT:
        if (sys_state.value_edit_mode)
        {
            sys_state.repeat_count++;
            uint32_t time_since_last = current_time - sys_state.last_increment_time;
            if (time_since_last < FAST_INCREMENT_THRESHOLD_MS)
                sys_state.fast_increment_active = true;

            bool fast_mode = sys_state.fast_increment_active ||
                             (sys_state.repeat_count > 5);
            decrease_value(fast_mode, false);
            printf("Down: Repeat (cnt=%d, fast=%d)\n",
                   sys_state.repeat_count, fast_mode);
            lcd_update_value_edit_screen();
        }
        else if (sys_state.in_detail_view &&
                 sys_state.menu_state == MENU_DIAGNOSTIC)
        {
            // Fast scroll backward through diagnostic items
            int num_items = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &num_items);
            if (items && num_items > 0)
            {
                sys_state.menu_selection = (sys_state.menu_selection == 0)
                                               ? num_items - 1
                                               : sys_state.menu_selection - 1;
                sys_state.detail_parent_selection = sys_state.menu_selection;
                lcd_draw_diagnostics_screen(sys_state.menu_selection);
            }
        }
        else if (sys_state.menu_state != MENU_NONE && !sys_state.in_detail_view)
        {
            // Fast scroll forward through menu items
            int num_items = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state,
                                                      &num_items);
            if (items && num_items > 0)
            {
                sys_state.menu_selection =
                    (sys_state.menu_selection + 1) % num_items;

                if (lcd_task_handle != NULL)
                    vTaskSuspend(lcd_task_handle);
                lcd_draw_menu_scroll(sys_state.menu_state,
                                     sys_state.menu_selection);
                if (lcd_task_handle != NULL)
                    vTaskResume(lcd_task_handle);
            }
        }
        break;

    case BUTTON_EVENT_RELEASE:
        sys_state.repeat_count = 0;
        sys_state.fast_increment_active = false;
        printf("Down: Released - reset acceleration\n");
        break;

    default:
        break;
    }

    sys_state.last_activity_time = current_time;
    sys_state.last_increment_time = current_time;
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

// Advanced back button handler
void handle_back_button_event(const button_event_info_t *event_info, void *user_data)
{
    if (!sys_state.system_ready)
    {
        ets_printf("Back button: system not ready, ignoring\n");
        return;
    }

    switch (event_info->event)
    {
    /* ======================================================================
     * SINGLE CLICK
     * ==================================================================== */
    case BUTTON_EVENT_CLICK:

        ets_printf("Back: click (edit=%d detail=%d confirm=%d menu=%d depth=%d)\n",
                   sys_state.value_edit_mode,
                   sys_state.in_detail_view,
                   sys_state.in_confirmation_screen,
                   sys_state.menu_state,
                   menu_history.depth);

        /* -- PRIORITY 1: exit value-edit mode without saving --------------- */
        if (sys_state.value_edit_mode)
        {
            ets_printf("Back: cancelling value edit\n");

            exit_value_edit_mode(false);
            sys_state.value_edit_mode = false;

            lcd_show_value_canceled_screen();
            vTaskDelay(pdMS_TO_TICKS(800));

            lcd_clear();
            lcd_draw_menu_scroll(sys_state.menu_state,
                                 sys_state.menu_selection);
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            return;
        }
        /* -- PRIORITY 2: exit detail view ---------------------------------- */
        if (sys_state.in_detail_view)
        {
            // Step 1: clear the detail flag
            sys_state.in_detail_view = false;
            sys_state.inverter.inverter_state = sys_state.pre_detail_inverter_state;

            sys_state.last_activity_time = esp_timer_get_time() / 1000;

            // Step 4: resume lcd_task if the detail screen suspended it
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
            sync_lcd_state();

            // Step 6: draw — row1 now shows "System:OFF " or "System:ON "
            lcd_clear();
            lcd_draw_menu_scroll(sys_state.menu_state,
                                 sys_state.menu_selection);
            return;
        }

        /* -- PRIORITY 3: exit confirmation screen -------------------------- */
        if (sys_state.in_confirmation_screen)
        {
            ets_printf("Back: exiting confirmation screen\n");

            sys_state.in_confirmation_screen = false;
            sys_state.last_activity_time = esp_timer_get_time() / 1000;

            lcd_clear();
            lcd_draw_menu_scroll(sys_state.menu_state,
                                 sys_state.menu_selection);
            return;
        }

        /* -- PRIORITY 4: exit info screen ---------------------------------- */
        if (sys_state.in_info_screen)
        {
            ets_printf("Back: exiting info screen\n");

            sys_state.in_info_screen = false;
            sys_state.last_activity_time = esp_timer_get_time() / 1000;

            sync_lcd_state();
            lcd_clear();
            lcd_draw_main_screen(&sys_state.lcd_state);
            return;
        }

        /* -- PRIORITY 5: pop menu history ---------------------------------- */
        if (menu_history.depth > 0)
        {
            menu_state_t previous_state;
            int previous_selection;

            if (pop_menu_history(&previous_state, &previous_selection))
            {
                ets_printf("Back: history pop -> state=%d sel=%d\n",
                           previous_state, previous_selection);

                sys_state.menu_state = previous_state;
                sys_state.menu_selection = previous_selection;
                sys_state.last_activity_time = esp_timer_get_time() / 1000;

                lcd_clear();

                if (previous_state == MENU_NONE)
                {
                    /* Fully exited the menu system */
                    sync_lcd_state();
                    if (lcd_task_handle != NULL)
                        vTaskResume(lcd_task_handle);
                    lcd_draw_main_screen(&sys_state.lcd_state);
                }
                else
                {
                    lcd_draw_menu_scroll(sys_state.menu_state,
                                         sys_state.menu_selection);
                }
                return;
            }
        }

        /* -- PRIORITY 6: fallback based on current menu state -------------- */
        switch (sys_state.menu_state)
        {
        case MENU_SETTINGS:
        case MENU_MONITORING:
        case MENU_DIAGNOSTIC:
        case MENU_WIFI_CONFIG:
        case MENU_FACTORY_RESET_CONFIRM:
            if (lcd_task_handle != NULL)
                vTaskSuspend(lcd_task_handle);
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            if (xSemaphoreTake(sys_state_mutex, pdMS_TO_TICKS(SYS_STATE_MUTEX_TIMEOUT_MS)) == pdTRUE)
            {
                sys_state.inverter.inverter_state = sys_state.inverter.previous_inverter_state;
                sys_state.lcd_state.fault_code = 0;
                sys_state.menu_state = MENU_NONE;
                sys_state.menu_selection = 0;
                sys_state.value_edit_mode = false;
                xSemaphoreGive(sys_state_mutex);
            }
            lcd_clear();
            lcd_draw_main_screen(&sys_state.lcd_state);
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
            break;

        case MAIN_MENU:
            ets_printf("Back: MAIN_MENU -> main screen\n");
            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
            clear_menu_history();
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            sync_lcd_state();
            if (lcd_task_handle != NULL)
                vTaskResume(lcd_task_handle);
            lcd_clear();
            lcd_draw_main_screen(&sys_state.lcd_state);
            break;

        case MENU_NONE:
            ets_printf("Back: already at main screen\n");
            break;

        default:
            ets_printf("Back: unknown state %d -> MAIN_MENU\n", sys_state.menu_state);
            sys_state.menu_state = MAIN_MENU;
            sys_state.menu_selection = 0;
            clear_menu_history();
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            lcd_clear();
            lcd_draw_menu_scroll(sys_state.menu_state,
                                 sys_state.menu_selection);
            break;
        }
        break; /* end BUTTON_EVENT_CLICK */

    /* ======================================================================
     * LONG PRESS — quick escape to main screen from anywhere
     * ==================================================================== */
    case BUTTON_EVENT_LONG_PRESS:
        if (sys_state.value_edit_mode)
        {
            exit_value_edit_mode(false);
            sys_state.value_edit_mode = false;
        }

        // Restore inverter state if escaping from a detail view
        if (sys_state.in_detail_view)
            sys_state.inverter.inverter_state = sys_state.pre_detail_inverter_state;

        sys_state.in_detail_view = false;
        sys_state.in_confirmation_screen = false;
        sys_state.in_info_screen = false;
        sys_state.menu_state = MENU_NONE;
        sys_state.menu_selection = 0;
        clear_menu_history();
        sys_state.last_activity_time = esp_timer_get_time() / 1000;

        sync_lcd_state();
        if (lcd_task_handle != NULL)
            vTaskResume(lcd_task_handle);
        lcd_clear();
        lcd_draw_main_screen(&sys_state.lcd_state);
        break;
    default:
    }
}

static const char *TAG = "INVERTER";

void inverter_power_on(void)
{
    // ── Safety check ──────────────────────────────────────────────────────
    if (!check_safety_conditions())
    {
        ets_printf("Power button: safety check FAILED\n");
        if (lcd_task_handle != NULL)
            vTaskSuspend(lcd_task_handle);
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_print("Safety check    ");
        lcd_set_cursor(0, 1);
        lcd_print("FAILED! See log ");
        buzzer_error();
        vTaskDelay(pdMS_TO_TICKS(2000));
        sys_state.lcd_state.current_screen = LCD_SCREEN_ERRORS;
        sync_lcd_state();
        lcd_clear();
        lcd_draw_main_screen(&sys_state.lcd_state);
        if (lcd_task_handle != NULL)
            vTaskResume(lcd_task_handle);
        return;
    }

    // ── Suspend lcd_task — we own the display from here ───────────────────
    if (lcd_task_handle != NULL)
        vTaskSuspend(lcd_task_handle);

    // ── Transition to STARTING state ──────────────────────────────────────
    sys_state.inverter.inverter_state = INVERTER_STARTING;
    sys_state.inverter.inverter_active = false;

    // ── Configure output parameters ───────────────────────────────────────
    inverter_set_output_voltage(220);
    vTaskDelay(pdMS_TO_TICKS(300));

    inverter_set_output_frequency(sys_state.inverter.output_frequency);
    vTaskDelay(pdMS_TO_TICKS(300));

    // ── Show startup screen ───────────────────────────────────────────────
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print("STARTING...     "); // exactly 16 chars

    // ── Soft-start progress display ───────────────────────────────────────
    for (int i = 0; i <= 100; i += 10)
    {
        char prog[17];
        snprintf(prog, sizeof(prog), "Progress:%3d%%   ", i); // exactly 16 chars
        lcd_set_cursor(0, 1);
        lcd_printf(1, 0, "%s", prog);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // ── Set current limit immediately before closing relay ────────────────
    inverter_set_current_limit(sys_state.current_limit);

    // ── Energise power relay ──────────────────────────────────────────────
    esp_err_t err = gpio_set_level(GPIO_POWER_RELAY, 1);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "gpio_set_level failed for power relay: %s",
                 esp_err_to_name(err));

        // Restore safe state
        sys_state.inverter.inverter_state = INVERTER_FAULT;
        sys_state.inverter.inverter_active = false;
        sys_state.error.error_flags |= SYSTEM_FAILURE_ERROR;

        // Show fault before handing display back
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_print("** START FAILED "); // exactly 16 chars
        lcd_set_cursor(0, 1);
        lcd_print("Check relay/HW  "); // exactly 16 chars
        buzzer_error();

        vTaskDelay(pdMS_TO_TICKS(2000));

        // Sync fault state into lcd_state before resuming
        sys_state.lcd_state.inverter_state = INVERTER_FAULT;
        sys_state.lcd_state.fault_code = (uint16_t)sys_state.error.error_flags;
        sys_state.lcd_state.current_screen = LCD_SCREEN_MAIN;
        sys_state.menu_state = MENU_NONE;

        lcd_clear(); // clean slate for lcd_task

        if (lcd_task_handle != NULL)
            vTaskResume(lcd_task_handle);

        return;
    }

    // ── Inverter fully started ────────────────────────────────────────────
    sys_state.inverter.inverter_state = INVERTER_ON;
    sys_state.inverter.inverter_active = true;

    // ── Sync lcd_state before resuming lcd_task ───────────────────────────
    // lcd_task reads lcd_state — populate it now so the first draw is correct
    sync_lcd_state();
    sys_state.lcd_state.current_screen = LCD_SCREEN_MAIN;

    // MENU_NONE lets lcd_task drive lcd_update_display() normally
    sys_state.menu_state = MENU_NONE;

    // Clean slate so lcd_task doesn't inherit startup text
    lcd_clear();

    // ── Resume lcd_task — it owns the display again ───────────────────────
    if (lcd_task_handle != NULL)
        vTaskResume(lcd_task_handle);

    // ── User feedback ─────────────────────────────────────────────────────
    buzzer_success();
    led_on(LED_STATUS);

    ESP_LOGI(TAG, "Inverter powered on successfully");
}

void shutdown_inverter(void)
{
    // ── Suspend lcd_task only for the critical display sequences ──────────
    if (lcd_task_handle != NULL)
        vTaskSuspend(lcd_task_handle);

    // ── Initial shutdown notice ───────────────────────────────────────────
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print("  Shutting Down ");
    lcd_set_cursor(0, 1);
    lcd_print(" Please wait... ");
    vTaskDelay(pdMS_TO_TICKS(500));

    // ── Warn if load still connected ──────────────────────────────────────
    // ── Warn if load still connected (lcd_task suspended only for the write) ──
    if (sys_state.inverter.actual_current > 0.5f)
    {
        char load_line[17];
        snprintf(load_line, sizeof(load_line), "Load:%-6.1fA    ",
                 sys_state.inverter.actual_current);
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_print("** WARNING! **  ");
        lcd_set_cursor(0, 1);
        lcd_print(load_line);

        // Resume during the wait — lcd_task won't disturb us because
        // current_screen is not LCD_SCREEN_MAIN yet
        if (lcd_task_handle != NULL)
            vTaskResume(lcd_task_handle);

        vTaskDelay(pdMS_TO_TICKS(1500)); // lcd_task runs freely during this wait

        // Re-suspend for the ramp sequence
        if (lcd_task_handle != NULL)
            vTaskSuspend(lcd_task_handle);
    }

    // ── Ramp down display ─────────────────────────────────────────────────
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print("RAMP DOWN       ");

    for (int i = 100; i >= 0; i -= 10)
    {
        char prog[17];
        snprintf(prog, sizeof(prog), "Power:%4d%%     ", i);
        lcd_set_cursor(1, 0);
        lcd_print(prog);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // ── Ramp output voltage to zero BEFORE opening relay ─────────────────
    inverter_set_output_voltage(0);
    vTaskDelay(pdMS_TO_TICKS(200));

    // ── Open power relay ──────────────────────────────────────────────────
    esp_err_t err = gpio_set_level(GPIO_POWER_RELAY, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "gpio_set_level failed on power relay: %s",
                 esp_err_to_name(err));
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_print("** RELAY FAULT  ");
        lcd_set_cursor(0, 1);
        lcd_print("Restarting...   ");
        vTaskDelay(pdMS_TO_TICKS(1500));

        if (lcd_task_handle != NULL)
            vTaskResume(lcd_task_handle);

        perform_system_restart(false);
        return;
    }

    led_off(LED_STATUS);

    // ── Update state ──────────────────────────────────────────────────────
    sys_state.inverter.inverter_state = INVERTER_OFF;
    sys_state.inverter.inverter_active = false;
    sys_state.menu_state = MENU_NONE;
    sys_state.error.error_flags &= ~SYSTEM_FAILURE_ERROR;

    // ── Sync lcd_state immediately so lcd_task draws correctly on resume ──
    sync_lcd_state();
    sys_state.lcd_state.current_screen = LCD_SCREEN_MAIN;

    lcd_clear();

    // ── Resume lcd_task HERE — as soon as state and display are ready ─────
    // lcd_task immediately picks up MENU_NONE + LCD_SCREEN_MAIN and draws
    // the status screen. No more delays happen under suspension.
    if (lcd_task_handle != NULL)
        vTaskResume(lcd_task_handle);

    // ── Everything below runs concurrently with lcd_task ──────────────────
    ESP_LOGI(TAG, "Inverter powered off");
}

// ============================================================================
// ENTER DIAGNOSTIC MODE
// ============================================================================

void enter_diagnostic_mode(void)
{
    // ── Suspend lcd_task FIRST — before touching the display ─────────────
    if (lcd_task_handle != NULL)
        vTaskSuspend(lcd_task_handle);

    // ── Update shared state under mutex ───────────────────────────────────
    if (xSemaphoreTake(sys_state_mutex, pdMS_TO_TICKS(SYS_STATE_MUTEX_TIMEOUT_MS)) == pdTRUE)
    {
        sys_state.inverter.previous_inverter_state = sys_state.inverter.inverter_state;
        sys_state.inverter.inverter_state = INVERTER_DIAGNOSTIC;
        sys_state.lcd_state.current_screen = LCD_SCREEN_DIAGNOSTICS;
        sys_state.menu_state = MENU_DIAGNOSTIC;
        sys_state.menu_selection = 0;
        xSemaphoreGive(sys_state_mutex);
    }

    // ── Entry animation — safe now that lcd_task is suspended ─────────────
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print("Entering Diag.  "); // exactly 16 chars

    // Dot animation: "Please wait.    " → "Please wait..   " → "Please wait...  "
    // Each iteration writes a full fixed-width 16-char string so no
    // leftover characters from the previous write remain on the display.
    const char *dot_frames[3] = {
        "Please wait.    ", // 16 chars
        "Please wait..   ", // 16 chars
        "Please wait...  "  // 16 chars
    };

    for (int i = 0; i < 3; i++)
    {
        lcd_set_cursor(1, 0);
        lcd_print(dot_frames[i]);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // ── Draw the diagnostic menu — lcd_task stays suspended ──────────────
    lcd_draw_menu_scroll(MENU_DIAGNOSTIC, sys_state.menu_selection);
}

// ============================================================================
// EXIT DIAGNOSTIC MODE
// ============================================================================
void exit_diagnostic_mode(void)
{
    // lcd_task is already suspended from enter_diagnostic_mode().
    // Guard against being called when it isn't (e.g. double-exit).
    if (lcd_task_handle != NULL)
        vTaskSuspend(lcd_task_handle);

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print(" Exiting Diag.. "); // 16 chars
    lcd_set_cursor(1, 0);
    lcd_print("                "); // blank row 1
    vTaskDelay(pdMS_TO_TICKS(1500));

    if (xSemaphoreTake(sys_state_mutex, pdMS_TO_TICKS(SYS_STATE_MUTEX_TIMEOUT_MS)) == pdTRUE)
    {
        sys_state.inverter.inverter_state = sys_state.inverter.previous_inverter_state;
        sys_state.lcd_state.fault_code = 0;
        sys_state.menu_state = MENU_NONE;
        sys_state.menu_selection = 0;
        sys_state.value_edit_mode = false;
        xSemaphoreGive(sys_state_mutex);
    }

    // Draw the main screen while lcd_task is still suspended so the
    // display is already populated when the task wakes — no blank gap.
    // sync_lcd_state();
    lcd_draw_main_screen(&sys_state.lcd_state);

    if (lcd_task_handle != NULL)
        vTaskResume(lcd_task_handle);

    ESP_LOGI(TAG, "Diagnostic mode exited");
}

void perform_factory_reset(void)
{
    lcd_clear();
    lcd_print_centered(0, "FACTORY RESET");
    lcd_print_centered(1, "Starting...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    printf("Performing factory reset...\n");
    sys_state.inverter.inverter_state = INVERTER_FACTORY_RESET;

    // Show progress
    lcd_clear();
    lcd_print_centered(0, "RESETTING...");
    for (int i = 0; i <= 100; i += 20)
    {
        lcd_printf(1, 0, "Progress: %d%%   ", i);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // Reset all settings to defaults
    sys_state.inverter.output_voltage = 220.0f;
    sys_state.inverter.output_frequency = 50.0f;
    sys_state.current_limit = 20.0f;
    sys_state.temperature_limit = 70.0f;
    sys_state.cutoff_voltage = 11.5f;
    sys_state.display.scroll_speed = DEFAULT_SCROLL_SPEED;
    // Clear fault logs
    lcd_clear();
    lcd_print_centered(0, "CLEARING LOGS");
    lcd_print_centered(1, "Please wait...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Reset calibration values
    lcd_clear();
    lcd_print_centered(0, "CALIBRATION");
    lcd_print_centered(1, "Resetting...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Visual indication
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

    lcd_clear();
    lcd_print_centered(0, "RESET COMPLETE");
    lcd_print_centered(1, "Restarting...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    printf("Factory reset complete\n");

    // 1. Erase NVS partition
    ESP_ERROR_CHECK(nvs_flash_erase());

    // 2. Reinitialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // 3. Reset all system parameters to defaults
    memset(&sys_state, 0, sizeof(system_state_t));
    sys_state.inverter.output_voltage = 220.0f;
    sys_state.inverter.temperature = 50.0f;
    sys_state.display.scroll_speed = DEFAULT_SCROLL_SPEED;

    // 4. Reset calibration data
    for (int i = 0; i < ADC_CHANNEL_USE; i++)
    {
        adc_calibration[i].calibration_values[0] = 0.0f; // Offset
        adc_calibration[i].calibration_values[1] = 1.0f; // Gain
        adc_calibration[i].calibrated = false;           // Mark as not calibrated
        adc_calibration[i].calibration_mode = false;     // Not in calibration mode
    }

    // 5. Save default settings
    err = nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &nvs_handler);
    if (err == ESP_OK)
    {
        save_settings();    // Assumes nvs_handler is open globally or passed in
        save_calibration(); // Same here
        nvs_close(nvs_handler);
    }
    else
    {
        ESP_LOGE(TAG_NVS, "Failed to open NVS: %s", esp_err_to_name(err));
    }

    sys_state.inverter.inverter_state = INVERTER_FACTORY_RESET;
    sys_state.menu_state = MENU_NONE;
    sys_state.power_button_sequence_count = 0;
    // 6. Visual confirmation
    lcd_clear();
    lcd_print("Factory Reset Complete");
    update_buzzer(3000, 70);
    vTaskDelay(2000 / portTICK_PERIOD_MS);

    // 7. Restart the system
    perform_system_restart(true);
    // Restart display
    lcd_display_startup_screen();
    vTaskDelay(pdMS_TO_TICKS(1000));
    sync_lcd_state();
    lcd_draw_main_screen(&sys_state.lcd_state);
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

void handle_menu_timeout(void)
{
    int64_t current_time = esp_timer_get_time() / 1000; // Convert to ms
    if (sys_state.menu_state != MENU_NONE &&
        current_time - sys_state.last_activity_time > MENU_TIMEOUT_MS)
    {
        printf("Menu timeout - returning to main screen\n");
        sys_state.menu_state = MENU_NONE;
        sys_state.menu_selection = 0;
        clear_menu_history();
        sync_lcd_state();
        lcd_draw_main_screen(&sys_state.lcd_state);
    }
}

// ============================================================================
// LCD SCREEN DISPLAY FUNCTIONS
// ============================================================================

/**
 * @brief Display initial startup message
 */
void lcd_display_startup_screen(void)
{
    lcd_clear();
    lcd_print_centered(0, "C-TECH SYSTEMS");
    lcd_printf(1, 0, " Starting...   ");

#if LCD_ROWS >= 4
    lcd_printf(2, 0, "                ");
    lcd_printf(3, 0, "  Version 1.0   ");
#endif
}

/**
 * @brief Draw brand/logo screen
 */
void lcd_draw_brand_screen(void)
{
    static bool first_draw = true;

    if (first_draw)
    {
        lcd_clear();

        // Display startup screen
        lcd_display_startup_screen();

#if LCD_ROWS >= 4
        lcd_printf(2, 0, "                ");
        lcd_printf(3, 0, "  Please Wait   ");
#endif

        first_draw = false;
    }
}

/*------------------------------------------------------------------------------
  LCD DISPLAY CONFIGURATION
------------------------------------------------------------------------------*/
// Display update intervals
#define LCD_REFRESH_RATE_MS 500       // Update every 500ms
#define LCD_BLINK_INTERVAL_MS 1000    // Blink rate for warnings
#define LCD_ROTATION_INTERVAL_MS 5000 // Auto-rotate every 5 seconds

/*------------------------------------------------------------------------------
  DISPLAY SCREEN TYPES
------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------
  HELPER FUNCTIONS
------------------------------------------------------------------------------*/

static const char *get_battery_icon(float voltage)
{
    if (voltage >= 12.6f)
        return "\xDB"; // Full
    else if (voltage >= 12.0f)
        return "\xB2"; // 75%
    else if (voltage >= 11.5f)
        return "\xB1"; // 50%
    else if (voltage >= 11.0f)
        return "\xB0"; // 25%
    else
        return " "; // Empty
}

/**
 * @brief Display main operating screen
 */
/*------------------------------------------------------------------------------
  MAIN SCREEN - 16x2 Optimized
------------------------------------------------------------------------------*/

static inline float clamp_float(float value, float min, float max)
{
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

static float display_voltage = 0.0f;

static void lcd_draw_battery_smooth(float target_voltage)
{
    if (display_voltage == 0.0f)
    {
        display_voltage = target_voltage; // first valid lock-in
    }
    else
    {
        display_voltage +=
            (target_voltage - display_voltage) * 0.2f; // smoothing
    }

    float percent =
        ((display_voltage - 10.5f) / (12.6f - 10.5f)) * 100.0f;
    percent = clamp_float(percent, 0.0f, 100.0f);

    lcd_printf(0, 0, "Bat:%4.1fV %3.0f%%",
               display_voltage, percent);
}

void lcd_draw_battery_normal(float battery_v)
{
    static float last_displayed_v = -1.0f;

    // Avoid unnecessary redraw (reduce flicker)
    if (fabsf(battery_v - last_displayed_v) < 0.05f)
        return;

    last_displayed_v = battery_v;

    // Convert voltage to percentage (example for 12V battery)
    int percent = (int)(((battery_v - 10.5f) / (12.6f - 10.5f)) * 100.0f);

    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    lcd_printf(0, 0, "Bat:%4.2fV %3d%%", battery_v, percent);
}

/**
 * @brief Draw initialization screen with progress
 */
void lcd_draw_init_screen(void)
{
    static uint8_t boot_progress = 0;
    static uint32_t last_update = 0;
    uint32_t now = xTaskGetTickCount();

    // Update progress every 200ms
    if ((now - last_update) >= pdMS_TO_TICKS(200))
    {
        if (boot_progress < 100)
        {
            boot_progress += 10;
        }

        lcd_printf(0, 0, "Initializing... ");

        // Draw progress bar on line 2
        lcd_draw_progress_bar(1, boot_progress);

#if LCD_ROWS >= 4
        EventBits_t bits = xEventGroupGetBits(sys_event_group);

        if (bits & EVT_ADC_VALID)
        {
            lcd_printf(2, 0, "ADC: Ready      ");
        }
        else
        {
            lcd_printf(2, 0, "ADC: Waiting... ");
        }

        lcd_printf(3, 0, "Progress: %3d%% ", boot_progress);
#endif

        last_update = now;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    sys_state.inverter.boot_complete = (boot_progress >= 100);
    return;
}

/* ── Tuning constants ─────────────────────────────────────────── */

#define VOLTAGE_DELTA_THRESH 0.09f /* min change (V) to redraw battery line  */
#define LOAD_DELTA_THRESH 1        /* min change (%) to redraw line 1        */
#define LINE1_CYCLE_MS 3000        /* ms between line-1 sub-page flips       */
#define INVERTER_RATED_WATTS 2400  /* used for watt estimate on sub-page 1   */
#define BATT_CELLS 5               /* number of bar segments in battery icon */

/* Battery voltage range for the fill bar (adjust to your chemistry) */
#define BATT_V_MIN 44.0f
#define BATT_V_MAX 54.4f

/* ── Forward declarations ─────────────────────────────────────── */
static const char *inverter_mode_string(inverter_state_t state);

/*------------------------------------------------------------------------------
  16x2 LCD DISPLAY SYSTEM FOR INVERTER
  Line 0: Battery Voltage + State/Status
  Line 1: Load/Power + Mode
------------------------------------------------------------------------------*/

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

#define STARTUP_ANIMATION_DURATION_MS 2000
#define VOLTAGE_SMOOTHING_FACTOR 0.2f
#define LOAD_SMOOTHING_FACTOR 0.3f

/*------------------------------------------------------------------------------
  SMOOTH VALUE INTERPOLATION WITH STARTUP ANIMATION
------------------------------------------------------------------------------*/
static float smooth_value_update(float current, float target, float smoothing,
                                 bool is_startup, float startup_progress)
{
    if (is_startup)
    {
        // Ease-out quadratic for natural startup feel
        float eased = 1.0f - (1.0f - startup_progress) * (1.0f - startup_progress);
        return target * eased;
    }
    else
    {
        // Normal smooth tracking
        return current + (target - current) * smoothing;
    }
}

static void sync_lcd_state(void)
{
    sys_state.lcd_state.battery_voltage = sys_state.inverter.battery.voltage;
    sys_state.lcd_state.load_percentage = sys_state.inverter.load_percentage;
    sys_state.lcd_state.inverter_state = sys_state.inverter.inverter_state;
    sys_state.lcd_state.fault_code = (uint16_t)sys_state.error.error_flags;
    sys_state.lcd_state.max_power_w = 1000.0f; // your rated wattage
    sys_state.lcd_state.blink_state =
        ((xTaskGetTickCount() / pdMS_TO_TICKS(500)) % 2 == 0);
}

/*------------------------------------------------------------------------------
  MAIN SCREEN DRAWING WITH SMOOTH ANIMATIONS
------------------------------------------------------------------------------*/
void lcd_draw_main_screen(lcd_display_state_t *data)
{
    if (data == NULL)
        return;

    uint32_t now = xTaskGetTickCount();
    bool is_startup = !lcd_anim.startup_complete;
    float startup_progress = 1.0f;

    if (is_startup)
    {
        if (lcd_anim.startup_start_time == 0)
            lcd_anim.startup_start_time = now;

        uint32_t elapsed_ms = pdTICKS_TO_MS(now - lcd_anim.startup_start_time);
        startup_progress = (float)elapsed_ms / STARTUP_ANIMATION_DURATION_MS;
        if (startup_progress >= 1.0f)
        {
            lcd_anim.startup_complete = true;
            startup_progress = 1.0f;
        }
    }

    // ── Smooth voltage ────────────────────────────────────────────────────
    float target_v = data->battery_voltage; // populated by sync_lcd_state()
    lcd_anim.display_voltage = is_startup
                                   ? target_v * (1.0f - (1.0f - startup_progress) * (1.0f - startup_progress))
                                   : lcd_anim.display_voltage + (target_v - lcd_anim.display_voltage) * VOLTAGE_SMOOTHING_FACTOR;

    // ── Battery percent (clamp 0–100) ─────────────────────────────────────
    float pct = ((lcd_anim.display_voltage - 10.5f) / (12.6f - 10.5f)) * 100.0f;
    if (pct < 0.0f)
        pct = 0.0f;
    if (pct > 100.0f)
        pct = 100.0f;

    // ── First frame placeholder ───────────────────────────────────────────
    if (lcd_anim.first_update)
    {
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_print("Bat:--.-V   --%  "); // exactly 16 chars
        lcd_set_cursor(1, 0);
        lcd_print("Initializing... "); // exactly 16 chars
        lcd_anim.first_update = false;
        return;
    }

    // ── ROW 0: Battery voltage + percent  (always 16 chars) ──────────────
    // Format: "Bat:XX.XV  XXX%  "
    //          1234567890123456
    char row0[17];
    snprintf(row0, sizeof(row0), "Bat:%4.1fV  %3.0f%%",
             lcd_anim.display_voltage, pct);
    // Guarantee exactly 16 printable chars — pad/truncate
    row0[16] = '\0';
    lcd_set_cursor(0, 0);
    lcd_print(row0);

    // ── ROW 1: Status line  (always 16 chars) ────────────────────────────
    char row1[17];

    if (is_startup)
    {
        // Show animated dots during startup
        uint8_t dots = (pdTICKS_TO_MS(now) / 400) % 4;
        const char *dot_str[] = {"               ",
                                 ".              ",
                                 "..             ",
                                 "...            "};
        snprintf(row1, sizeof(row1), "Starting%s", dot_str[dots]);
    }
    else
    {
        switch (data->inverter_state)
        {
        case INVERTER_OFF:
            snprintf(row1, sizeof(row1), "System:%s      ",
                     inverter_mode_string(INVERTER_OFF));
            break;

        case INVERTER_STARTING:
            snprintf(row1, sizeof(row1), "System:%s     ",
                     inverter_mode_string(INVERTER_STARTING));
            break;

        case INVERTER_STANDBY:
            snprintf(row1, sizeof(row1), "System:%s     ",
                     inverter_mode_string(INVERTER_STANDBY));
            break;

        case INVERTER_ON:
        {
            // "AC:220V Ld: 45% "
            //  1234567890123456
            float power_w = (lcd_anim.display_load / 100.0f) * data->max_power_w;
            if (power_w < 1000.0f)
                snprintf(row1, sizeof(row1), "AC:%3.0fV%4.0fW%3.0f%%",
                         sys_state.inverter.output_voltage,
                         power_w,
                         lcd_anim.display_load);
            else
                snprintf(row1, sizeof(row1), "AC:%3.0fV %.1fkW%2.0f%%",
                         sys_state.inverter.output_voltage,
                         power_w / 1000.0f,
                         lcd_anim.display_load);
            break;
        }

        case INVERTER_FAULT:
            if (data->blink_state)
                snprintf(row1, sizeof(row1), "** FAULT %03d ** ",
                         data->fault_code & 0xFF);
            else
                snprintf(row1, sizeof(row1), "Check error log ");
            break;

        case INVERTER_DIAGNOSTIC:
            snprintf(row1, sizeof(row1), "Mode:DIAGNOSTIC ");
            break;

        default:
            snprintf(row1, sizeof(row1), "Status:UNKNOWN  ");
            break;
        }
    }

    row1[16] = '\0';
    lcd_set_cursor(1, 0);
    lcd_print(row1);
}

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

/**
 * @brief  Short 3-4 char mode abbreviation that fits the 16-char line.
 */
static const char *inverter_mode_string(inverter_state_t state)
{
    switch (state)
    {
    case INVERTER_OFF:
        return "OFF ";
    case INVERTER_STANDBY:
        return "STBY";
    case INVERTER_ON:
        return "ON ";
    case INVERTER_FAULT:
        return "ERR ";
    case INVERTER_STARTING:
        return "STRT";
    default:
        return "??? ";
    }
}

/*------------------------------------------------------------------------------
  SCREEN 2: BATTERY DETAILS
------------------------------------------------------------------------------*/

void lcd_draw_battery_screen(void)
{
    lcd_set_cursor(0, 0);

    // Create simple bar
    char bar[11];
    float percent = ((sys_state.inverter.battery.voltage - 10.5f) / 2.1f) * 100.0f;
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    int filled = (int)(percent / 12.5f); // 8 segments
    bar[0] = '[';
    for (int i = 0; i < 8; i++)
    {
        bar[i + 1] = (i < filled) ? '\xFF' : '-';
    }
    bar[9] = ']';
    bar[10] = '\0';
    lcd_printf(0, 0, "BAT: %5.2fV      ", sys_state.inverter.battery.voltage);
    lcd_set_cursor(1, 0);
    lcd_print_string("LEV: ");
    lcd_set_cursor(1, 5);
    lcd_print_string(bar);

#if LCD_ROWS >= 4
    const char *direction = (sys_state.actual_current > 0) ? "Disch" : "Chrg ";
    lcd_printf(2, 0, "%s:%5.2fA        ",
               direction, fabs(sys_state.actual_current));

    if (sys_state.inverter.battery.voltage < 10.8f)
    {
        lcd_printf(3, 0, "Status: CRITICAL!  ");
    }
    else if (sys_state.inverter.battery.voltage < 11.5f)
    {
        lcd_printf(3, 0, "Status: Low        ");
    }
    else if (sys_state.inverter.battery.voltage < 12.0f)
    {
        lcd_printf(3, 0, "Status: Fair       ");
    }
    else if (sys_state.inverter.battery.voltage < 12.6f)
    {
        lcd_printf(3, 0, "Status: Good       ");
    }
    else
    {
        lcd_printf(3, 0, "Status: Full       ");
    }
#endif
}

/*------------------------------------------------------------------------------
  SCREEN 3: POWER DETAILS
------------------------------------------------------------------------------*/

void lcd_draw_power_screen(void)
{
    float power_w = sys_state.inverter.output_voltage * sys_state.actual_current;
    lcd_set_cursor(0, 0);
    lcd_print_string("PWR CONSUMPTION  ");
    if (power_w < 1000)
    {
        lcd_printf(1, 0, "AC Out: %6.1fW  ", power_w);
    }
    else
    {
        lcd_printf(1, 0, "AC Out: %5.2fkW   ", power_w / 1000.0f);
    }

#if LCD_ROWS >= 4
    if (bat_power < 1000)
    {
        lcd_printf(2, 0, "DC In:  %6.1fW     ", bat_power);
    }
    else
    {
        lcd_printf(2, 0, "DC In:  %5.2fkW    ", bat_power / 1000.0f);
    }

    float efficiency = (power_w > 0 && bat_power > 0) ? (power_w / bat_power) * 100.0f : 0.0f;
    if (efficiency > 100)
        efficiency = 100;

    lcd_printf(3, 0, "Efficiency: %5.1f%%  ", efficiency);
#endif
}

/**
 * @brief Draw a progress bar on the LCD
 * @param row LCD row (0-3 for 20x4, 0-1 for 16x2)
 * @param percent Progress percentage (0-100)
 */
void lcd_draw_progress_bar(uint8_t row, uint8_t percent)
{
    static int last_percent = -1;

    if (percent > 100)
        percent = 100;

    if (percent == last_percent)
        return;

    last_percent = percent;

    const uint8_t bar_width = 16; // Force full width

    uint8_t filled = (percent * bar_width + 50) / 100;
    // +50 improves rounding

    lcd_set_cursor(row, 0);

    for (uint8_t i = 0; i < bar_width; i++)
    {
        if (i < filled)
            lcd_print_char(0xFF);
        else
            lcd_print_char(' ');
    }
}

// Update menu screen header for 16x2 LCD - MAIN UPDATE FUNCTION

/**
 * @brief Display value editing screen
 */
void lcd_update_value_edit_screen(void)
{

    lcd_clear();
    char line1[32], line2[32];

    snprintf(line1, sizeof(line1), "%s", value_edit.label);
    snprintf(line2, sizeof(line2), "%.2f %s", value_edit.temp_value, value_edit.unit);

    lcd_print_centered(0, line1);
    lcd_print_centered(1, line2);

    // Blink cursor if in edit mode
    static bool blink_state = false;
    static int64_t last_blink = 0;
    int64_t current_time = esp_timer_get_time() / 1000;

    if (current_time - last_blink > LCD_BLINK_INTERVAL_MS)
    {
        blink_state = !blink_state;
        last_blink = current_time;
    }

    if (sys_state.value_edit_mode && value_edit.unit && value_edit.label)
    {
        char value_str[17];
        int value_len = snprintf(value_str, sizeof(value_str),
                                 "%.2f %s", value_edit.temp_value, value_edit.unit);
        if (blink_state && value_len < 16)
        {
            lcd_set_cursor(value_len, 1); // position after the value string on row 1
            lcd_print_char('_');
        }
    }
}

/*------------------------------------------------------------------------------
  SCREEN 4: DIAGNOSTICS
------------------------------------------------------------------------------*/

void lcd_draw_diagnostics_screen(uint8_t index)
{
    int item_count = 0;
    const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &item_count);
    // Guard: invalid list or index out of range
    if (items == NULL || item_count == 0 || index >= (uint8_t)item_count)
    {
        ESP_LOGW(TAG, "lcd_draw_diagnostics_screen: invalid index %u (count=%d)",
                 index, item_count);
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_print("  Diag Error    ");
        lcd_set_cursor(0, 1);
        lcd_print("Bad item index  ");
        return;
    }

    // Guard: label pointer must not be NULL (FIX A/B ensure this, but be safe)
    const char *label = items[index].label;
    if (label == NULL)
        label = "(no label)";

    // ── Row 0: item label, exactly 16 chars, space-padded ────────────────
    char row0[17];
    snprintf(row0, sizeof(row0), "%-16.16s", label);

    // ── Row 1: live value for this diagnostic item ────────────────────────
    char row1[17];

    switch (index)
    {
    case 0: // System Health
        snprintf(row1, sizeof(row1), "Sys:%-12s",
                 diag_data.system_ok ? "OK" : "Fault");
        break;

    case 1: // Error Logs
    {
        const error_log_entry_t *latest = error_log_get_latest();

        if (latest == NULL)
        {
            snprintf(row1, sizeof(row1), "%-16s", "No errors logged");
        }
        else
        {
            // description is 32 chars max, NUL-terminated — always safe
            snprintf(row1, sizeof(row1), "%-16.16s", latest->description);
        }
        break;
    }

    case 2: // CPU Load — "Load:  75.3%    " = 16 chars
        snprintf(row1, sizeof(row1), "Load:%6.1f%%    ",
                 diag_data.cpu_load);
        // %-10.1f%% would also work; the above pads to exactly 16
        // "Load:" (5) + " 75.3%" (6+1) = 12; 4 trailing spaces = 16 ✓
        break;

    case 3:                                                     // Device Info — pull from system constants, not a hardcoded literal
        snprintf(row1, sizeof(row1), "%-16.16s", "C-01 Rev A"); // replace with actual device info if available
        break;

    case 4: // Uptime — show as "DDd HH:MM:SS" or "HH:MM:SS" ≤ 16 chars
    {
        unsigned long s = (unsigned long)diag_data.uptime_seconds;
        unsigned long d = s / 86400UL;
        unsigned long h = (s % 86400UL) / 3600UL;
        unsigned long m = (s % 3600UL) / 60UL;
        unsigned long sec = s % 60UL;
        if (d > 0)
            snprintf(row1, sizeof(row1), "%lud %02lu:%02lu:%02lu ", d, h, m, sec);
        else
            snprintf(row1, sizeof(row1), "   %02lu:%02lu:%02lu    ", h, m, sec);
        break;
    }

    case 5: // Memory — "RAM:  62.4%     " = 16 chars
        snprintf(row1, sizeof(row1), "RAM:%6.1f%%     ",
                 diag_data.ram_usage);
        break;

    default:
        snprintf(row1, sizeof(row1), "%-16s", "Unknown item");
        break;
    }

    // Ensure both rows are exactly 16 printable chars + NUL regardless of
    // what snprintf produced (extra safety net — not a substitute for correct
    // format strings above).
    row0[16] = '\0';
    row1[16] = '\0';

    // ── Write to LCD ──────────────────────────────────────────────────────
    lcd_set_cursor(0, 0);
    lcd_print(row0);
    lcd_set_cursor(1, 0);
    lcd_print(row1);
}

/*------------------------------------------------------------------------------
  SCREEN 5: ERRORS
------------------------------------------------------------------------------*/

void lcd_draw_error_screen(void)
{
    if (sys_state.error.error_flags && sys_state.lcd_state.blink_state)
    {
        lcd_printf(0, 0, " *** ERRORS *** ");
        // Flash backlight for (int i = 0; i < 3; i++)
        {
            lcd_backlight(false);
            vTaskDelay(pdMS_TO_TICKS(200));
            lcd_backlight(true);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    else
    {
        lcd_printf(0, 0, "  ERROR STATUS  ");
    }

    uint8_t line = 1;
    bool shown = false;

    if (sys_state.error.error_flags & ERR_LOW_BAT)
    {
        lcd_printf(line++, 0, "  ! Low Battery ");
        shown = true;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (sys_state.error.error_flags & ERR_OVER_TEMP)
    {
        lcd_printf(line++, 0, "  ! Over Temp  ");
        shown = true;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (sys_state.error.error_flags & ERR_OVERLOAD)
    {
        lcd_printf(line++, 0, "  ! Overload     ");
        shown = true;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!shown)
    {
        lcd_printf(1, 0, "   No Errors    ");
#if LCD_ROWS >= 4
        lcd_printf(2, 0, "  System Normal    ");
        lcd_printf(3, 0, "                   ");
#endif
    }
    else
    {
        while (line < LCD_ROWS)
        {
            lcd_printf(line++, 0, "                   ");
        }
    }
}

/**
 * @brief Display confirmation screen
 */
void lcd_display_confirmation_screen(void)
{
    lcd_clear();
    lcd_print_centered(0, "Save Changes?");
    lcd_print_centered(1, "Enter=Yes Back=No");
}

/**
 * @brief Main screen update dispatcher
 */
/*------------------------------------------------------------------------------
  MAIN UPDATE FUNCTION
------------------------------------------------------------------------------*/
void lcd_update_display(void)
{
    static const char *TAG = "LCD_UPDATE";
    static TickType_t last_screen_change = 0;
    const TickType_t SCREEN_ROTATE_INTERVAL = pdMS_TO_TICKS(5000); // Rotate every 5 seconds

    uint32_t now = xTaskGetTickCount();

    // Auto-rotate screens if no user interaction
    if ((now - last_screen_change) >= SCREEN_ROTATE_INTERVAL)
    {
        // Cycle through screens (exclude LCD_SCREEN_COUNT)
        sys_state.lcd_state.current_screen =
            (sys_state.lcd_state.current_screen + 1) % LCD_SCREEN_COUNT;

        last_screen_change = now;
        ESP_LOGD(TAG, "Screen rotated to: %d", sys_state.lcd_state.current_screen);
    }

    // Update blink state for error screen
    sys_state.lcd_state.blink_state = ((now / pdMS_TO_TICKS(500)) % 2 == 0);

    ESP_LOGD(TAG, "Current screen: %d", sys_state.lcd_state.current_screen);

    // Draw screen
    switch (sys_state.lcd_state.current_screen)
    {
    case LCD_SCREEN_MAIN:
        sync_lcd_state();
        lcd_draw_main_screen(&sys_state.lcd_state);
        break;
    case LCD_SCREEN_BATTERY:
        lcd_draw_battery_screen();
        break;
    case LCD_SCREEN_POWER:
        lcd_draw_power_screen();
        break;
    case LCD_SCREEN_DIAGNOSTICS:
        lcd_draw_diagnostics_screen(sys_state.menu_selection);
        break;
    case LCD_SCREEN_ERRORS:
        lcd_draw_error_screen();
        break;
    case LCD_SCREEN_NONE:
        break;
    default:
        ESP_LOGW(TAG, "Invalid screen %d, resetting to MAIN", sys_state.lcd_state.current_screen);
        sys_state.lcd_state.current_screen = LCD_SCREEN_NONE;
        last_screen_change = now;
        break;
    }
}

/**
 * @brief Handle boot sequence screens
 * @return true when boot sequence is complete
 */
bool lcd_handle_boot_sequence(void)
{
    static const char *TAG = "BOOT_SEQ";
    static TickType_t screen_start_time = 0;
    static boot_screen_t current_screen = BOOT_SCREEN_BRAND;
    static bool screen_initialized = false;

    const TickType_t now = xTaskGetTickCount();
    const TickType_t BRAND_SCREEN_DURATION = pdMS_TO_TICKS(1500);

    // Initialize screen on first entry or screen change
    if (!screen_initialized)
    {
        screen_start_time = now;
        screen_initialized = true;
        lcd_clear();
    }

    switch (current_screen) // ← Use local static variable
    {

    case BOOT_SCREEN_BRAND:
        lcd_draw_brand_screen();

        if ((now - screen_start_time) >= BRAND_SCREEN_DURATION)
        {
            ESP_LOGI(TAG, "Brand screen done → Init screen");
            current_screen = BOOT_SCREEN_INIT;
            screen_initialized = false;
        }
        break;

    case BOOT_SCREEN_INIT:
        ESP_LOGI(TAG, "ADC ready → Main screen");
        current_screen = BOOT_SCREEN_MAIN;
        screen_initialized = false;
        break;

    case BOOT_SCREEN_MAIN:
        ESP_LOGI(TAG, "Boot sequence complete!");

        // Update sys_state WITH mutex protection
        if (xSemaphoreTake(sys_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            sys_state.lcd_state.current_screen = LCD_SCREEN_MAIN;
            sys_state.lcd_state.last_update_time = now;
            sys_state.lcd_state.last_screen_change = now;
            xSemaphoreGive(sys_state_mutex);
        }

        screen_initialized = false;
        return true; // Boot complete

    default:
        ESP_LOGW(TAG, "Invalid boot state %d, resetting", current_screen);
        current_screen = BOOT_SCREEN_BRAND;
        screen_initialized = false;
        break;
    }

    return false; // Boot still in progress
}

/*------------------------------------------------------------------------------
  LCD TASK - CONTINUOUS UPDATE
------------------------------------------------------------------------------*/

void lcd_task(void *arg)
{
    const char *TAG = "LCD_TASK";
    ESP_LOGI(TAG, "LCD task started");

    if (sys_state_mutex == NULL)
    {
        ESP_LOGE(TAG, "MUTEX IS NULL!");
        vTaskDelete(NULL);
        return;
    }

    if (xSemaphoreTake(sys_state_mutex, portMAX_DELAY) == pdTRUE)
    {
        sys_state.lcd_boot_state.current_screen = BOOT_SCREEN_BRAND;
        sys_state.lcd_boot_state.boot_screen_timestamp_ms = xTaskGetTickCount();
        sys_state.inverter.boot_complete = false;
        xSemaphoreGive(sys_state_mutex);
    }

    bool boot_complete = false;

    while (1)
    {
        if (xSemaphoreTake(sys_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            boot_complete = sys_state.inverter.boot_complete;
            xSemaphoreGive(sys_state_mutex);
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!boot_complete)
        {
            bool boot_finished = lcd_handle_boot_sequence();

            if (boot_finished)
            {
                if (xSemaphoreTake(sys_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                {
                    sys_state.inverter.boot_complete = true;
                    sys_state.lcd_state.current_screen = LCD_SCREEN_MAIN;
                    xSemaphoreGive(sys_state_mutex);
                }
            }
        }
        else
        {
            // Only update display at defined interval to prevent flicker
            if (sys_state.menu_state == MENU_NONE &&
                !sys_state.in_detail_view &&
                !sys_state.value_edit_mode)
            {
                lcd_update_display();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// System initialization
void button_system_init(void)
{
    sys_state.menu_selection = 0;
    sys_state.safety_conditions_met = true; // Set based on actual conditions
    sys_state.last_activity_time = 0;
    sys_state.power_button_sequence_count = 0;

    // Initialize value adjustment context
    sys_state.current_value_type.edit_type = value_edit.edit_type;
    sys_state.value_edit_mode = false;
    sys_state.value_changed = false;
    sys_state.pending_confirmation = false;
    sys_state.repeat_count = 0;
    sys_state.fast_increment_active = false;

    // Initialize system parameters with default values
    sys_state.inverter.output_voltage = 220.0f;
    sys_state.inverter.output_frequency = 50.0f;
    sys_state.current_limit = 20.0f;
    sys_state.temperature_limit = 70.0f;
    sys_state.cutoff_voltage = 11.5f;

    printf("Button system initialized\n");
    printf("Default values loaded:\n");
    printf("- Output Voltage: %.1f V\n", sys_state.inverter.output_voltage);
    printf("- Frequency: %.2f Hz\n", sys_state.inverter.output_frequency);
    printf("- Current Limit: %.1f A\n", sys_state.current_limit);
    printf("- Temperature Limit: %.1f °C\n", sys_state.temperature_limit);
    printf("- Battery Cutoff: %.2f V\n", sys_state.cutoff_voltage);
}

// Advanced value adjustment implementation
void increase_value(bool fast_mode, bool precision_mode)
{
    if (!sys_state.value_edit_mode)
        return;

    ets_printf("Increase value called (fast: %d, precision: %d)\n",
               fast_mode, precision_mode);
    value_edit_context_t *config = get_current_value_config();
    float *current_value = get_current_value_pointer();
    ets_printf("Current value: %.3f\n", *current_value);
    if (!config || !current_value)
        return;

    float increment = calculate_increment(fast_mode, precision_mode);
    float new_value = *current_value + increment;

    // Apply acceleration based on repeat count
    if (sys_state.repeat_count > 10)
    {
        increment *= 3;
    }
    else if (sys_state.repeat_count > 5)
    {
        increment *= 2;
    }

    new_value = *current_value + increment;

    // Validate range
    if (validate_value_range(new_value))
    {
        ets_printf("Value change valid: %.3f -> %.3f\n", *current_value, new_value);
        *current_value = new_value;
        value_edit.current_value = new_value;
        sys_state.value_changed = true;

        // Apply live update if enabled
        if (config->live_update && !config->is_critical)
        {
            update_system_parameter(config, new_value);
        }

        // Set pending confirmation for critical values
        if (config->is_critical)
        {
            sys_state.pending_confirmation = true;
        }

        printf("Value increased to: %.*f %s\n",
               config->decimal_places, new_value, config->unit);
    }
    else
    {
        printf("Value at maximum limit: %.*f %s\n",
               config->decimal_places, config->max_value, config->unit);
    }
}

void decrease_value(bool fast_mode, bool precision_mode)
{
    if (!sys_state.value_edit_mode)
        return;

    value_edit_context_t *config = get_current_value_config();
    float *current_value = get_current_value_pointer();
    if (!config || !current_value)
        return;

    float increment = calculate_increment(fast_mode, precision_mode);
    float new_value = *current_value - increment;

    // Apply acceleration based on repeat count
    if (sys_state.repeat_count > 10)
    {
        increment *= 3;
    }
    else if (sys_state.repeat_count > 5)
    {
        increment *= 2;
    }

    new_value = *current_value - increment;

    // Validate range
    if (validate_value_range(new_value))
    {
        *current_value = new_value;
        value_edit.current_value = new_value;
        sys_state.value_changed = true;

        // Apply live update if enabled
        if (config->live_update && !config->is_critical)
        {
            update_system_parameter(config, new_value);
        }

        // Set pending confirmation for critical values
        if (config->is_critical)
        {
            sys_state.pending_confirmation = true;
        }

        printf("Value decreased to: %.*f %s\n",
               config->decimal_places, new_value, config->unit);
    }
    else
    {
        printf("Value at minimum limit: %.*f %s\n",
               config->decimal_places, config->min_value, config->unit);
    }
}

void enter_value_edit_mode(value_edit_context_t *value_type)
{
    if (sys_state.value_edit_mode)
        return;

    sys_state.current_value_type.edit_type = value_type->edit_type;
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

    value_edit_context_t *config = get_current_value_config();
    printf("Entering edit mode for %s (%.3f %s)\n",
           config->label, *current_value, config->unit);
    printf("Use UP/DOWN to adjust, ENTER to save, BACK to cancel\n");
}

void exit_value_edit_mode(bool save_changes)
{
    if (!sys_state.value_edit_mode)
        return;

    value_edit_context_t *config = get_current_value_config();
    float *current_value = get_current_value_pointer();

    if (save_changes && sys_state.value_changed)
    {
        if (config->is_critical && sys_state.pending_confirmation)
        {
            printf("Saving critical value change: %s = %.*f %s\n",
                   config->label, config->decimal_places,
                   *current_value, config->unit);
        }

        // Apply the change to system
        apply_value_change();
        printf("Value saved successfully\n");
    }
    else if (!save_changes && sys_state.value_changed)
    {
        // Restore backup value
        reset_value_to_backup();
        printf("Changes cancelled, value restored\n");
    }

    sys_state.value_edit_mode = false;
    sys_state.current_value_type.edit_type = VALUE_TYPE_NONE;
    sys_state.value_changed = false;
    sys_state.pending_confirmation = false;
}

void apply_value_change(void)
{
    if (!sys_state.value_edit_mode || !sys_state.value_changed)
        return;

    value_edit_context_t *config = get_current_value_config();
    float *current_value = get_current_value_pointer();

    if (config && current_value)
    {
        update_system_parameter(config, *current_value);
        sys_state.pending_confirmation = false;
        printf("Applied %s: %.*f %s\n",
               config->label, config->decimal_places,
               *current_value, config->unit);
    }
}

void reset_value_to_backup(void)
{
    if (!sys_state.value_edit_mode)
        return;

    float *current_value = get_current_value_pointer();
    value_edit_context_t *config = get_current_value_config();

    if (current_value && config)
    {
        *current_value = sys_state.edit_backup_value;
        config->current_value = sys_state.edit_backup_value;
        sys_state.value_changed = false;
        sys_state.pending_confirmation = false;

        printf("Value reset to: %.*f %s\n",
               config->decimal_places, sys_state.edit_backup_value, config->unit);
    }
}

float *get_current_value_pointer(void)
{
    value_edit_context_t *config = get_current_value_config();
    if (config)
    {
        return &config->current_value;
    }
    return NULL;
}

value_edit_context_t *get_current_value_config(void)
{
    if (!sys_state.value_edit_mode)
        return NULL;
    if (value_edit.label == NULL)
        return NULL;
    return &value_edit;
}

float calculate_increment(bool fast_mode, bool precision_mode)
{
    value_edit_context_t *config = get_current_value_config();
    if (!config)
        return 0.0f;

    if (precision_mode)
    {
        return config->increment_precision;
    }
    else if (fast_mode)
    {
        return config->increment_large;
    }
    else
    {
        return config->increment_small;
    }
}

bool validate_value_range(float new_value)
{
    value_edit_context_t *config = get_current_value_config();
    if (!config)
        return false;
    return (new_value >= config->min_value && new_value <= config->max_value);
}

void handle_value_confirmation(void)
{
    if (!sys_state.value_edit_mode || !sys_state.pending_confirmation)
        return;

    value_edit_context_t *config = get_current_value_config();
    void *current_value_ptr = get_current_value_pointer();

    if (!config || !current_value_ptr)
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
    switch (config->edit_type)
    {
    case VALUE_EDIT_NUMERIC:
    {
        float *current_value = (float *)current_value_ptr;

        // Perform category-based safety checks
        if (strstr(config->label, "Voltage"))
        {
            if (*current_value < 10.0f || *current_value > 260.0f)
            {
                printf("Voltage out of safe range: %.2fV\n", *current_value);
                safety_check_passed = false;
            }
        }
        else if (strstr(config->label, "Current"))
        {
            if (*current_value < 0.1f || *current_value > 50.0f)
            {
                printf("Current limit unsafe: %.1fA\n", *current_value);
                safety_check_passed = false;
            }
        }
        else if (strstr(config->label, "Temperature"))
        {
            if (*current_value < 20.0f || *current_value > 80.0f)
            {
                printf("Temperature alarm outside reasonable range: %.1f°C\n", *current_value);
                safety_check_passed = false;
            }
        }
        else if (strstr(config->label, "Timeout"))
        {
            if (*current_value < 1000.0f || *current_value > 600000.0f)
            {
                printf("System timeout invalid (%.0f ms)\n", *current_value);
                safety_check_passed = false;
            }
        }

        if (safety_check_passed)
        {
            update_system_parameter(config, *current_value);
            printf("Numeric value confirmed: %s = %.3f %s\n",
                   config->label, *current_value, config->unit);
        }
        break;
    }

    case VALUE_EDIT_SELECT:
    {
        int selected_index = *(int *)current_value_ptr;
        printf("Selected option for %s: %d (%s)\n",
               config->label, selected_index, config->options[selected_index]);

        update_system_parameter(config, (float)selected_index);
        break;
    }

    case VALUE_EDIT_BOOL:
    {
        bool state = *(bool *)current_value_ptr;
        printf("%s set to: %s\n", config->label, state ? "ON" : "OFF");

        update_system_parameter(config, (float)state);
        if (strcmp(config->label, "WiFi") == 0)
            start_wifi_scan();
        break;
    }

    case VALUE_EDIT_LIST:
    {
        const char *selected_str = (const char *)current_value_ptr;
        printf("%s selected: %s\n", config->label, selected_str);

        update_system_parameter(config, 0); // store index if needed
        // eeprom_save_string(config->eeprom_addr, selected_str);
        break;
    }

    default:
        printf("Unknown value edit type for %s\n", config->label);
        safety_check_passed = false;
        break;
    }

    // ======== Post-confirmation handling ========
    if (safety_check_passed)
    {
        printf("Value confirmed and applied for %s\n", config->label);
        sys_state.pending_confirmation = false;
        sys_state.value_changed = true;
        exit_value_edit_mode(true);

        printf("AUDIT: Parameter changed - %s\n", config->label);
        lcd_show_message("Value Saved", config->label);
    }
    else
    {
        printf("Safety check failed or value invalid, reverting changes\n");
        reset_value_to_backup();
        sys_state.pending_confirmation = false;
        exit_value_edit_mode(false);
        lcd_show_message("Change Rejected", config->label);
    }

    sys_state.menu_state = MENU_NONE;
    lcd_update_menu_screen();
}

void update_system_parameter(value_edit_context_t *context_type, float value)
{
    // Update the actual inverter system parameters
    switch (context_type->edit_type)
    {

    case VALUE_EDIT_NUMERIC:
        if (strcmp(context_type->label, "Voltage Threshold") == 0)
        {
            printf("System: Setting output voltage to %.1f V\n", value);
            inverter_set_output_voltage(value);
        }
        else if (strcmp(context_type->label, "Frequency Range") == 0)
        {
            printf("System: Setting output frequency to %.2f Hz\n", value);
            inverter_set_output_frequency(value);
        }
        else if (strcmp(context_type->label, "Current Limit") == 0)
        {
            printf("System: Setting current limit to %.1f A\n", value);
            inverter_set_current_limit(value);
        }
        else if (strcmp(context_type->label, "Temp Alarm") == 0)
        {
            printf("System: Setting temperature limit to %.1f °C\n", value);
            thermal_protection_set_limit(value);
        }
        else if (strcmp(context_type->label, "Battery Cutoff") == 0)
        {
            printf("System: Setting battery cutoff to %.2f V\n", value);
            battery_monitor_set_cutoff(value);
        }
        else if (strcmp(context_type->label, "Sys Timeout") == 0)
        {
            printf("System: Setting system timeout to %.0f ms\n", value);
            set_system_timeout((uint32_t)value);
        }
        break;
    case VALUE_EDIT_BOOL:
        if (strcmp(context_type->label, "WiFi") == 0)
        {
            sys_state.wifi.enabled = (bool)value;
            printf("System: WiFi %s\n", sys_state.wifi.enabled ? "Enabled" : "Disabled");
        }
        break;
    case VALUE_EDIT_SELECT:
        // Handle selection updates if needed
        break;
    default:
        break;
    }
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
    printf("!!! EMERGENCY SHUTDOWN TRIGGERED !!!\n");
    // Turn off the gpios controlling the inverter
    gpio_set_level(GPIO_POWER_RELAY, 0);
    sys_state.inverter.inverter_state = INVERTER_OFF;
    sys_state.inverter.inverter_active = false;
    sys_state.system_ready = false;

    // Display error screen
    lcd_draw_error_screen();

    printf("All outputs disabled\n");
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
    config.triple_click_ms = 500;
    config.controller_name = "Power Button";
    config.active_low = true;
    config.enable_pullup = true;
    config.enable_power_management = false;
    config.hold_repeat_ms = 100;                     // Fast repeat for smooth volume control
    config.active_level = config.active_low ? 0 : 1; // Active level based on wiring

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
    ESP_LOGI(APP_TAG, "🔋 Power button event callback registered ✓");

    // Step 2.6: Register error callback
    ret = button_controller_register_error_callback(g_app_state.power_button,
                                                    button_error_handler,
                                                    "PowerButton");
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔋 Failed to register power button error callback: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(APP_TAG, "🔋 Power button error callback registered ✓");

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
    config.triple_click_ms = 400;
    config.controller_name = "Menu Button";
    config.active_low = true;
    config.enable_pullup = true;
    config.enable_power_management = false;
    config.hold_repeat_ms = 100;                     // Fast repeat for smooth volume control
    config.active_level = config.active_low ? 0 : 1; // Active level based on wiring

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

    ret = button_controller_register_error_callback(g_app_state.menu_button,
                                                    button_error_handler,
                                                    "MenuButton");
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "📱 Failed to register menu button error callback: %s", esp_err_to_name(ret));
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
    config.triple_click_ms = 400;
    config.controller_name = "Volume Up Button";
    config.active_low = true;
    config.enable_pullup = true;
    config.enable_power_management = false;
    config.hold_repeat_ms = 100;                     // Fast repeat for smooth volume control
    config.active_level = config.active_low ? 0 : 1; // Active level based on wiring
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

    ret = button_controller_register_error_callback(g_app_state.button_up,
                                                    button_error_handler,
                                                    "VolumeUpButton");
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔊 Failed to register volume up error callback: %s", esp_err_to_name(ret));
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
    config.triple_click_ms = 400;
    config.controller_name = "Volume Down Button";
    config.active_low = true;
    config.enable_pullup = true;
    config.enable_power_management = false;
    config.hold_repeat_ms = 100;                     // Fast repeat for smooth volume control
    config.active_level = config.active_low ? 0 : 1; // Active level based on wiring

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

    ret = button_controller_register_error_callback(g_app_state.button_down,
                                                    button_error_handler,
                                                    "VolumeDownButton");
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔉 Failed to register volume down error callback: %s", esp_err_to_name(ret));
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
    config.triple_click_ms = 400;
    config.controller_name = "Back Button";
    config.active_low = true;
    config.enable_pullup = true;
    config.enable_power_management = false;
    config.hold_repeat_ms = 100;                     // Fast repeat for smooth volume control
    config.active_level = config.active_low ? 0 : 1; // Active level based on wiring

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

    ret = button_controller_register_error_callback(g_app_state.button_back,
                                                    button_error_handler,
                                                    "BackButton");
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "🔙 Failed to register back button error callback: %s", esp_err_to_name(ret));
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

    // System information
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(APP_TAG, "│ Chip: %s Rev %d, %d cores, %s Flash           │",
             CONFIG_IDF_TARGET, chip_info.revision, chip_info.cores,
             (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "Embedded" : "External");

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

// ================== UNIFIED MENU INPUT HANDLER ==================

// Helper functions for menu handling
void toggle_display()
{
    sys_state.display.display_on = !sys_state.display.display_on;
    LCD_power(sys_state.display.display_on);
    update_led(LED_STATUS, sys_state.display.display_on ? 100 : 0);
}

void navigate_to_menu(menu_state_t menu)
{
    sys_state.display.current_menu = menu;
    lcd_clear();
    // Optionally refresh the menu screen here
    lcd_update_menu_display();
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

void clamp_values()
{
    sys_state.battery_profile.cutoff_voltage_12v = fmaxf(9.0f, fminf(12.0f, sys_state.battery_profile.cutoff_voltage_12v));
    sys_state.battery_profile.recharge_voltage_12v = fmaxf(12.0f, fminf(15.0f, sys_state.battery_profile.recharge_voltage_12v));
}

int get_setting_value(int index)
{
    nvs_handle_t nvs;
    char key[16];
    int32_t value = 0;
    esp_err_t err;

    snprintf(key, sizeof(key), "setting_%d", index);

    if (nvs_open(NVS_NS_SYSTEM, NVS_READONLY, &nvs) == ESP_OK)
    {
        err = nvs_get_i32(nvs, key, &value);
        nvs_close(nvs);

        if (err == ESP_OK)
            return value;
    }

    // Return default value if not found or read fails
    return default_settings[index];
}

void set_setting_value(int index, int value)
{
    nvs_handle_t nvs;
    char key[16];

    snprintf(key, sizeof(key), "setting_%d", index);

    if (nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_i32(nvs, key, value);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void menu_exit()
{
    // Replace with actual code to return to the main screen
    lcd_clear();
    lcd_print("Exiting menu...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    sys_state.display.current_menu = MAIN_MENU;
}

#define MAX_PROFILES 3
void show_profile_on_lcd(battery_profile_t *profile);

void show_profile_on_lcd(battery_profile_t *profile)
{
    lcd_clear();
    lcd_set_cursor(0, 0);
    char display_char[16];
    snprintf(display_char, sizeof(display_char), "Battery: %.1fV", (float)profile->nominal_voltage * 10);
    lcd_print(display_char);

    lcd_set_cursor(1, 0);
    snprintf(display_char, sizeof(display_char), "Cutoff: %.1fV", profile->cutoff_voltage_12v * 10);
    lcd_print(display_char);
}

void adjust_factory_reset(button_event_info_t btn)
{
    static factory_reset_state_t state = FACTORY_RESET_CONFIRM;
    static bool selection_yes = true; // true = YES, false = NO

    switch (btn.gpio_pin)
    {
    case BTN_UP:
    case BTN_DOWN:
        // Toggle between YES and NO
        selection_yes = !selection_yes;
        lcd_clear();
        lcd_print("Factory Reset?");
        lcd_set_cursor(1, 0);
        lcd_print(selection_yes ? "> Yes   No" : "  Yes > No");
        break;

    case BTN_ENTER_MENU:
        if (state == FACTORY_RESET_CONFIRM)
        {
            if (selection_yes)
            {
                lcd_clear();
                lcd_print("Resetting...");
                // TODO: perform actual factory reset logic here
                perform_factory_reset();

                vTaskDelay(pdMS_TO_TICKS(1000));
                lcd_clear();
                lcd_print("Reset Complete!");
                vTaskDelay(pdMS_TO_TICKS(1000));
                navigate_to_menu(MAIN_MENU); // Go back to home menu
            }
            else
            {
                navigate_to_menu(sys_state.display.current_menu); // Cancel reset
            }
        }
        break;

    case BTN_BACK:
        navigate_to_menu(sys_state.display.current_menu); // Go back to previous menu
        break;

    default:
        break;
    }
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

    // lcd_display_state
    sys_state.lcd_state.current_screen = LCD_SCREEN_MAIN;
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
    // Check if RTC memory is valid
    if (rtc_mem.magic_flag == RTC_MAGIC_FLAG)
    {
        sys_state.inverter.inverter_active = rtc_mem.was_inverter_active;
        sys_state.inverter.connected = rtc_mem.ac_was_connected;
        sys_state.error.error_flags |= rtc_mem.last_error;

        // Optionally clear RTC data after restoring
        rtc_mem.magic_flag = 0;
    }
    else
    {
        // If not valid, assume fresh boot
        sys_state.inverter.inverter_active = false;
        sys_state.inverter.connected = false;
        sys_state.error.error_flags = 0;
    }
}

// ================== DEEP SLEEP IMPLEMENTATION ==================
void enter_deep_sleep(uint32_t sleep_seconds)
{
    // === 1. Store Current Runtime State in RTC Memory ===
    rtc_mem.magic_flag = RTC_MAGIC_FLAG;
    rtc_mem.last_sleep_time = xTaskGetTickCount();
    rtc_mem.was_inverter_active = sys_state.inverter.inverter_active;
    rtc_mem.ac_was_connected = sys_state.inverter.connected;
    rtc_mem.last_error = (uint32_t)sys_state.error.error_flags;

    // === 2. Notify User Before Sleep ===
    lcd_clear();
    lcd_print("Entering Deep Sleep");
    lcd_set_cursor(0, 1);
    lcd_print("Wake in: ");
    lcd_write_int(sleep_seconds, 0, 0, 10); // Write seconds as integer
    lcd_print("s");

    // === 3. Save Persistent Settings if Necessary ===
    save_settings();

    // === 4. Shut Down Peripherals to Save Power ===
    update_buzzer(0, 0);       // Ensure buzzer is off
    update_led(LED_STATUS, 0); // Ensure status LED is off
    gpio_set_level(GPIO_POWER_RELAY, 0);

    // === 5. Configure Wakeup Sources ===
    esp_sleep_enable_timer_wakeup(sleep_seconds * 1000000); // Convert seconds to microseconds
    // Enable wakeup on any button press (LOW level)
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(wakeup_pin_mask, ESP_EXT1_WAKEUP_ALL_LOW));

    // === 6. Optionally Reset GPIOs Before Sleep ===
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
    // === 7. Delay to Allow LCD Message to Be Seen ===
    vTaskDelay(pdMS_TO_TICKS(2000)); // Wait 2 seconds
    // === 8. Enter Deep Sleep ===
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

// ================== WAKE FROM SLEEP HANDLER ==================
void handle_wakeup()
{
    // for truncation prevention
    int required_size; // buffer size
    char *buffer;      // storage size
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    char buf[LCD_COLS + 1];
    lcd_clear();
    switch (cause)
    {
    case ESP_SLEEP_WAKEUP_TIMER:
        lcd_print("Woke by Timer");
        // snprintf(buf, sizeof(buf), "Timer Wake #%lu", rtc_mem.wake_count);
        break;
    case ESP_SLEEP_WAKEUP_EXT0:
        lcd_print("Woke by Power Button");
        // snprintf(buf, sizeof(buf), "Button Wake #%lu", rtc_mem.wake_count);
        break;
    default:
        lcd_print("Cold Boot");
        snprintf(buf, sizeof(buf), "Cold Boot #%lu", rtc_mem.wake_count);
    }
    // Show sleep duration if available
    if (rtc_mem.last_sleep_time > 0)
    {
        uint32_t sleep_duration = (xTaskGetTickCount() - rtc_mem.last_sleep_time) / 1000;
        lcd_set_cursor(0, 1);
        // truncation prevention
        required_size = snprintf(NULL, 0, "Slept %ld seconds", sleep_duration) + 1;
        buffer = malloc(required_size);
        if (buffer != NULL)
        {
            snprintf(buffer, required_size, "Slept %ld seconds", sleep_duration);
            lcd_set_cursor(0, 1);
            lcd_print(buffer);
            free(buffer);
        }
    }

    // Restore critical state
    if (rtc_mem.last_error)
    {
        sys_state.error.error_flags |= rtc_mem.last_error;
        lcd_set_cursor(0, 1);
        lcd_print("Recovered from error");
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
}

void adjust_calibration_setting(button_event_info_t btn)
{
    static uint8_t calib_step = 0;
    button_id_t button_id = gpio_to_button_id(btn.gpio_pin);
    switch (calib_step)
    {
    case 0: // Calibration menu
        lcd_clear();
        lcd_print("Calibration Menu");
        lcd_set_cursor(0, 1);
        lcd_print("1. Battery Voltage");
        lcd_set_cursor(0, 0);
        lcd_print("2. Output Current");
        lcd_set_cursor(0, 1);
        lcd_print("ENTER:Select BACK:Exit");

        if (button_id == BTN_ENTER_MENU)
        {
            calib_step = sys_state.display.menu_position + 1;
        };
        if (button_id == BTN_BACK)
        {
            sys_state.display.current_menu = MAIN_MENU;
            calib_step = 0;
        };
        if (sys_state.display.menu_position == 8)
        { // Factory Reset option
            calib_step = 10;
        }
        else
        {
            calib_step = sys_state.display.menu_position + 1;
        };
        break;

    case 1: // Battery voltage calibration
        lcd_clear();
        lcd_print("Battery Calibration");
        lcd_set_cursor(0, 1);
        lcd_print("Connect known 12.0V");
        lcd_set_cursor(0, 0);
        lcd_print("Press ENTER when ready");
        lcd_set_cursor(0, 1);
        lcd_print("BACK:Cancel");

        if (button_id == BTN_ENTER_MENU)
        {
            // Perform battery voltage calibration
            float known_voltage = 12.0f; // Replace with actual known voltage
            sys_state.inverter.battery_voltage_calibration = known_voltage - sys_state.inverter.battery.voltage;
            sys_state.inverter.battery.voltage += sys_state.inverter.battery_voltage_calibration;
            save_settings(); // Save calibration to NVS
            lcd_clear();
            lcd_print("Calibration Done!");
            calib_step = 0;
            sys_state.display.current_menu = MAIN_MENU;
        }
        if (button_id == BTN_BACK)
        {
            calib_step = 0;
            sys_state.display.current_menu = MAIN_MENU;
            navigate_to_menu(MAIN_MENU);
        }
        break;

        // Additional calibration steps...
    case 10: // Factory reset confirmation
        perform_factory_reset();
        calib_step = 0;
        sys_state.display.current_menu = MAIN_MENU;
        lcd_clear();
        navigate_to_menu(MAIN_MENU);
        break;
    };
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

// ================== SYSTEM STATE INITIALIZATION ==================
void init_system_state()
{
    // Step 1: Load persisted settings from NVS
    // Step 2: Apply fallback defaults (only if needed)
    memset(&sys_state, 0, sizeof(sys_state)); // Clear system state
    sys_state.inverter.temperature = 25.0f;   // Default temperature
    // Step 2: Restore runtime-only data from RTC memory
    sys_state.inverter.inverter_active = false;
    sys_state.inverter.connected = false;
    /* System flags */
    sys_state.system_ready = false;
    sys_state.system_active = false;
    sys_state.output_enabled = false;
    sys_state.calibration_valid = false;
    sys_state.adc_ready = false;

    /* Battery defaults */
    sys_state.battery_voltage_system = 12.0f;
    sys_state.battery_cutoff = 11.05f;
    sys_state.low_battery = false;

    /* Inverter defaults */
    sys_state.inverter.battery.voltage = 0.0f;
    sys_state.inverter.output_voltage = 230.0f;
    sys_state.inverter.inverter_output_voltage = 0.0f;
    sys_state.inverter.fan_voltage = 0;
    sys_state.inverter.over_under_voltage = 0.0f;
    sys_state.temperature_limit = 60.0f;
    sys_state.current_limit = 30.0f;
    sys_state.cutoff_voltage = 48.0f;

    /* UI / menu */
    sys_state.menu_state = MAIN_MENU;
    sys_state.menu_selection = 0;
    sys_state.in_detail_view = false;

    /* Error & fault handling */
    sys_state.error.error_flags = 0;
    sys_state.fault_flags = 0;
    sys_state.error_count = 0;

    /* Timing */
    sys_state.last_activity_time = esp_timer_get_time();
    sys_state.error.error_flags |= rtc_mem.last_error;
    // Step 3: Initialize session state
    sys_state.flags.last_user_activity = xTaskGetTickCount();
    sys_state.flags.last_power_event = xTaskGetTickCount();
    sys_state.display.display_on = true;
    sys_state.display.scroll_enabled = false;
    sys_state.display.scroll_speed = DEFAULT_SCROLL_SPEED;
    // Set dummy output voltage and current for testing
    sys_state.inverter.output_voltage = 230.0f;
    sys_state.inverter.output_current = 0.0f;

    // Clear non-persistent error flags on startup
    sys_state.error.error_flags &= (ERR_EEPROM | ERR_FAN_FAIL);

    // Step 4: Wake cause check
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED)
    {
        ESP_LOGI(TAG_SYS, "Cold boot detected");
        memset(&rtc_mem, 0, sizeof(rtc_mem));
        rtc_mem.wake_count = 0;
    }
    else
    {
        ESP_LOGI(TAG_SYS, "Wake from sleep detected");
        rtc_mem.wake_count++;
    }
}

// ================== CRITICAL ERROR HANDLER ==================
void handle_critical_error()
{
    // 1. Log the error state

    ESP_LOGE(TAG_ERROR, "Critical Error: 0x%02X", sys_state.error.error_flags);
    log_error_to_nvs(sys_state.error.error_flags);

    // 2. Visual and audible alert
    update_buzzer(3000, 80);           // High-pitched alert
    blink_led(LED_ERROR, 200, 200, 5); // Blink error LED

    // 3. Show error on display
    lcd_clear();
    char buf[LCD_COLS + 1];
    uint32_t required_size;
    char *buffer; // used to check the value of the str to avoid truncation
    if (sys_state.error.error_flags & ERR_OVER_TEMP)

    {
        snprintf(buf, sizeof(buf), "Error: Over Temp");
        lcd_set_cursor(0, 0);
        lcd_print(buf);
        required_size = snprintf(NULL, 0, "Cur: %.1fC Max: %.1fC", sys_state.inverter.temperature, MAX_TEMPERATURE) + 1; // +1 for '\0'
        buffer = malloc(required_size);
        if (buffer != NULL)
        {
            snprintf(buffer, required_size, "Cur: %.1fC Max: %.1fC", sys_state.inverter.temperature, MAX_TEMPERATURE);
            if (required_size > strlen(buf))
            {
                lcd_text_scroll(buffer);
            }
            else
            {
                lcd_print(buffer);
            }
            free(buffer);
        }
    }
    else if (sys_state.error.error_flags & ERR_OVERLOAD)
    {
        snprintf(buf, sizeof(buf), "Error: Overload"); // Load first instance
        lcd_set_cursor(0, 0);
        lcd_print(buf);
        required_size = snprintf(NULL, 0, "Cur: %.1fA Max: %.1fA", sys_state.inverter.output_current, MAX_CURRENT) + 1; // +1 for '\0'
        buffer = malloc(required_size);
        if (buffer != NULL)
        {
            snprintf(buffer, required_size, "Cur: %.1fA Max: %.1fA", sys_state.inverter.output_current, MAX_CURRENT);
            if (required_size > strlen(buf))
            {
                lcd_set_cursor(0, 0);
                lcd_text_scroll(buffer);
            }
            else
            {
                lcd_set_cursor(0, 1);
                lcd_print(buffer);
            }
            free(buffer);
        }
    }
    // Add other error cases...

    // 4. Save state and restart
    rtc_mem.last_error = sys_state.error.error_flags;
    save_settings();
    vTaskDelay(pdMS_TO_TICKS(5000)); // Allow error to be seen
    // perform_system_restart(false);   // Normal restart
}

// ================== UPDATED MENU DISPLAY ==================
void display_battery_settings()
{
    lcd_clear();
    lcd_print("Battery Settings");

    // Cutoff voltage with different decimal precision based on edit mode
    lcd_set_cursor(0, 1);
    lcd_print("Cutoff: ");
    lcd_write_float(menu_edit.edit_step ? menu_edit.temp_value : sys_state.battery_profile.cutoff_voltage_12v,
                    menu_edit.edit_step ? 2 : 1);
    lcd_print("V");

    // Current reading with full precision
    lcd_set_cursor(0, 1);
    lcd_print("Current: ");
    lcd_write_float(sys_state.inverter.battery.voltage, 3);
    lcd_print("V");
    lcd_set_cursor(0, 0);
    lcd_print("UP/DOWN: Adjust ENTER:Save");
}

void register_task_to_wdt(TaskHandle_t task)
{
    if (esp_task_wdt_add(task) != ESP_OK)
    {
        ESP_LOGE("WDT", "Failed to add task %p to watchdog", task);
    }
}

// ================== USAGE EXAMPLES ==================
void show_battery_voltage()
{
    lcd_set_cursor(0, 0);
    lcd_print("Voltage: ");
    lcd_write_float(sys_state.inverter.battery.voltage, 2); // Shows 2 decimal places
    lcd_print("V");
}

void show_temperature()
{
    lcd_set_cursor(0, 1);
    lcd_print("Temp: ");
    lcd_write_float(sys_state.inverter.temperature, 1); // Shows 1 decimal place
    lcd_print("C");
}

// =============== SYSTEM RESTART IMPLEMENTATION ===============
_Noreturn void system_restart(void)
{ // C11 standard syntax
    esp_restart();
    __builtin_unreachable(); // GCC/Clang intrinsic
}

// =============== ENHANCED RESTART WRAPPER ===============
void perform_system_restart(bool factory_reset)
{
    // 1. Notify all subsystems about impending restart
    lcd_clear();
    lcd_print("System Restarting...");
    update_buzzer(2000, 30);

    // 2. Save critical state if needed
    if (!factory_reset)
    {
        save_settings();
        save_calibration();
    };

    // 3. Gracefully stop all tasks
    vTaskSuspendAll();
    // 4. Disconnect from peripherals
    esp_err_t ret = i2c_driver_delete(I2C_NUM_0);
    if (ret != ESP_OK)
    {
        ESP_LOGE("I2C", "Failed to delete driver (0x%x)", ret);
        return;
    };
    ESP_LOGI("I2C", "I2C%d driver removed", 1);
    gpio_reset_pin(GPIO_BUZZER);
    gpio_reset_pin(GPIO_STATUS_LED);
    // Add other peripheral cleanup as needed

    // 5. Wait for pending operations to complete
    esp_task_wdt_reset(); // Handle any watchdog
    vTaskDelay(pdMS_TO_TICKS(100));

    // 6. Actually restart
    log_error_to_nvs(90);
    system_restart();
}

void show_system_info()
{
    char line[17]; // 16 characters max for 16x2 LCD

    // Line 1: Firmware Version
    snprintf(line, sizeof(line), "Firmware: %s", FIRMWARE_VERSION);
    lcd_set_cursor(0, 0);
    lcd_print(line);

    // Line 2: Battery Type and Voltage
    battery_profile_t profile = battery_profiles[sys_state.battery_profile.chemistry];
    snprintf(line, sizeof(line), "%s %dV", profile.name_prefix,
             (int)sys_state.battery_profile.nominal_voltage);
    lcd_set_cursor(0, 1);
    lcd_print(line);
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
        snprintf(key, sizeof(key), "err_%04ld", error_count % 1000);
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

// Clean shutdown:C:\Users\hp\Documents\PlatformIO\Projects\INVERTER_SYSTEM\src\main.c
void shutdown()
{
    update_buzzer(0, 0);                 // Ensure buzzer off
    update_led(LED_STATUS, 0);           // Turn LED off
    gpio_set_level(GPIO_POWER_RELAY, 0); // turn OFF the power on pin
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

int fan_disconnect_count = 0;
bool fan_connected_last_state = true;

// 🔍 Check fan connection
static void check_fan_connection()
{
    // Send test pulse (briefly power the fan circuit)
    gpio_set_level(GPIO_FAN_TEST, 1);
    vTaskDelay(pdMS_TO_TICKS(1)); // 1ms ON (non-blocking)
    gpio_set_level(GPIO_FAN_TEST, 0);

    vTaskDelay(pdMS_TO_TICKS(1)); // Let voltage stabilize

    // Read averaged ADC
    int adc_value = sys_state.inverter.fan_connection;

    if (adc_value < FAN_DISCONNECTED_THRESHOLD)
    {
        fan_disconnect_count++;
        if (fan_disconnect_count >= FAN_DISCONNECT_RETRIES && fan_connected_last_state)
        {
            fan_connected_last_state = false;
            ESP_LOGE("FAN", "❌ Fan disconnected! ADC = %d", adc_value);
            update_led(LED_ERROR, 100); // Turn on error LED
            sys_state.error.error_flags |= ERR_FAN_FAIL;
        }
    }
    else
    {
        if (!fan_connected_last_state)
        {
            ESP_LOGI("FAN", "✅ Fan reconnected. ADC = %d", adc_value);
        }
        fan_disconnect_count = 0;
        fan_connected_last_state = true;
        update_led(LED_ERROR, 0);                     // Turn off error LED
        sys_state.error.error_flags &= ~ERR_FAN_FAIL; // Clear the fan fail error flag
    }
}

// 📡 Fan monitoring task
static void fan_monitor_task(void *arg)
{
    while (1)
    {
        check_fan_connection();
        vTaskDelay(pdMS_TO_TICKS(FAN_CHECK_INTERVAL_MS));
    }
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

void error_handler()
{
    typedef struct
    {
        system_errors_t flag;
        const char *line1;
        const char *line2;
        int buzzer_freq;
        int buzzer_duration;
    } ErrorMessage;

    ErrorMessage errors[] = {
        {ERR_OVER_TEMP, "ERROR: Over Temp", "System Shutdown!", 3000, 100},
        {ERR_SHORT_CIRCUIT, "ERROR: Short Circuit", "System Shutdown!", 3000, 100},
        {ERR_OVERLOAD, "ERROR: Overload", "System Overload", 2000, 80},
        {ERR_LOW_BAT, "ERROR: Low Battery", "Voltage too low", 1500, 100},
        {ERR_HIGH_BAT, "ERROR: High Battery", "Voltage too high", 1500, 100},
        {ERR_FAN_FAIL, "ERROR: Fan Failure", "Fan not running", 2000, 80},
        {ERR_OVER_UNDER_VOLTAGE, "ERROR: Voltage Fault", "Over/Under Voltage", 2000, 80}};

    bool error_found = false;

    // Track timers separately
    static TickType_t low_bat_start_time = 0;
    static bool low_bat_timer_started = false;

    static TickType_t fan_fail_start_time = 0;
    static bool fan_fail_timer_started = false;

    for (int i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
    {
        if (sys_state.error.error_flags == errors[i].flag)
        {
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_print(errors[i].line1);
            lcd_set_cursor(0, 1);
            lcd_print(errors[i].line2);

            update_buzzer(errors[i].buzzer_freq, errors[i].buzzer_duration);
            error_found = true;

            // Low battery shutdown logic
            if (errors[i].flag == ERR_LOW_BAT)
            {
                if (sys_state.inverter.battery.voltage < sys_state.battery_profile.cutoff_voltage_12v)
                {
                    if (!low_bat_timer_started)
                    {
                        low_bat_start_time = xTaskGetTickCount();
                        low_bat_timer_started = true;
                        ESP_LOGW("SYS", "Low battery timer started");
                    }
                    else if ((xTaskGetTickCount() - low_bat_start_time) >= pdMS_TO_TICKS(60000))
                    {
                        shutdown();
                        sys_state.display.current_menu = MAIN_MENU;
                        low_bat_timer_started = false;
                    }
                }
            }

            // Fan failure shutdown logic
            if (errors[i].flag == ERR_FAN_FAIL)
            {
                lcd_clear();
                lcd_set_cursor(0, 0);
                if (!fan_fail_timer_started)
                {
                    fan_fail_start_time = xTaskGetTickCount();
                    fan_fail_timer_started = true;
                    ESP_LOGE("SYS", "Fan fail timer started");
                }
                else if ((xTaskGetTickCount() - fan_fail_start_time) >= pdMS_TO_TICKS(120000))
                {
                    shutdown();
                    sys_state.display.current_menu = MAIN_MENU;
                    fan_fail_timer_started = false;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(2000)); // Delay after showing each error
        }
    }

    if (!error_found)
    {
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_print("Error: Unknown");
        lcd_set_cursor(0, 1);
        lcd_print("System error!");
        ESP_LOGE("SYS", "Unknown System Error");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    // Prompt for manual reset
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print("Press BACK to");
    lcd_set_cursor(0, 1);
    lcd_print("Reset system...");

    uint32_t elapsed = 0;
    const TickType_t interval = pdMS_TO_TICKS(100);

    while (elapsed < RESET_TIMEOUT_MS)
    {
        if (gpio_get_level(BTN_ENTER_MENU) == 0)
        {
            ESP_LOGI("SYS", "System reset by ENTER button");
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_print("System Reset");
            lcd_set_cursor(0, 1);
            lcd_print("Please wait...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            perform_system_restart(false);
            update_activity();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(interval));
        elapsed += 100;
    }
}

// Value editing functions
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
    lcd_update_value_edit_screen();
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
    lcd_update_value_edit_screen();
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
    lcd_update_value_edit_screen();
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
    lcd_update_value_edit_screen();
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
    lcd_update_value_edit_screen();
}

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

// =============== MAIN APPLICATION ===============
void app_main()
{
    sys_event_group = xEventGroupCreate();
    configASSERT(sys_event_group);
    sys_state_mutex = xSemaphoreCreateMutex();
    if (sys_state_mutex == NULL)
    {
        ESP_LOGE(APP_TAG, "💥 FATAL: Failed to create system state mutex");
        return;
    }

    init_hardware();     // Initialize ADC, I/O, Peripherals, led, buzzer etc.
    nvs_init(false);     // Don't erase, just fail
    init_system_state(); // Load system state from NVS
    init_menu_system();  // Initialize menu logic
    nvs_print_stats();
    // Check if initialized
    if (nvs_is_initialized())
    {
        ESP_LOGI("MAIN", "NVS is ready to use");
    }

    // init_watchdog(true, false);
    //  === Restore System State if Waking from Deep Sleep ===
    restore_from_deep_sleep();
    ESP_LOGI(APP_TAG, "\n"
                      "╔═══════════════════════════════════════════════════════════╗\n"
                      "║                ESP32 ADVANCED BUTTON DEMO                ║\n"
                      "║              Multi-Button Controller System               ║\n"
                      "║                                                           ║\n"
                      "║  Power Button: GPIO 0  |  Menu Button: GPIO 2            ║\n"
                      "║  Volume Up: GPIO 4     |  Volume Down: GPIO 5            ║\n"
                      "╚═══════════════════════════════════════════════════════════╝");

    ESP_LOGI(APP_TAG, "🚀 Application starting...");
    ESP_LOGI(APP_TAG, "🚀 Chip: %s", CONFIG_IDF_TARGET);
    ESP_LOGI(APP_TAG, "🚀 IDF Version: %s", esp_get_idf_version());
    ESP_LOGI(APP_TAG, "🚀 Free heap: %lu bytes", esp_get_free_heap_size());

    // Initialize the complete button system
    esp_err_t ret = initialize_button_system();
    if (ret != ESP_OK)
    {
        ESP_LOGE(APP_TAG, "💥 FATAL: Button system initialization failed!");
        ESP_LOGE(APP_TAG, "💥 System will restart in 5 seconds...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }

    ets_printf("🎯 Main application loop starting...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    lcd_power_init();        // Power GPIO setup
    LCD_power(true);         // Turn on screen
    lcd_set_brightness(200); // Set brightness (out of 255)
    LCD_power(false);        // Default OFF
    // enable_brownout();       // Setup brownout detection
    // === Create System Tasks ===
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
    {

        vTaskDelay(pdMS_TO_TICKS(1000)); // 100ms main loop
    }
    ESP_LOGW(APP_TAG, "⚠️ Main application loop ended - system no longer ready");
    cleanup_button_system();

    ESP_LOGI(APP_TAG, "🛑 Application terminated");
}