#ifndef SYSTEM_STATE_H
#define SYSTEM_STATE_H

#include "unistd.h"
#include "stdatomic.h"
#include <lcd_state.h>

#ifdef __cplusplus
extern "C"
{
#endif

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

    // Menu system states
    typedef enum
    {
        MENU_NONE,
        MAIN_MENU,
        MENU_SETTINGS,
        MENU_MONITORING,
        MENU_DIAGNOSTIC,
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
        // Battery
        float battery_voltage;
        bool battery_fresh;
        uint8_t load_percentage;
        float max_power_w; // Maximum power rating (e.g., 1000W)
        inverter_state_t inverter_state;
        bool blink_state;
        // Fault info
        uint16_t fault_code;
        uint32_t last_update_time;
        uint32_t last_screen_change;
    } lcd_display_state_t;

    typedef enum
    {
        VALUE_EDIT_NUMERIC = 0,
        VALUE_EDIT_BOOL,
        VALUE_EDIT_SELECT,
        VALUE_EDIT_LIST,
        VALUE_TYPE_NONE
    } value_edit_type_t;

    typedef void (*param_apply_fn)(float value);
    // Value configuration structure

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
        param_apply_fn apply; // NULL => no live hardware effect
    } value_edit_context_t;

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
        bool connected;
        float control_level;
        float speed; // Fan speed in volts
    } fan_status_t;

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
        ERR_AC_FAULT = 0x08,
        ERR_FAN_FAIL = 0x10,
        ERR_EEPROM = 0x20,
        ERR_HIGH_BAT = 0x40,
        ERR_SHORT_CIRCUIT = 0x80,
        ERR_SYSTEM_FAILURE = 0x90,
        ERR_OVER_UNDER_VOLTAGE = 0x50,
        ERR_COUNT
    } system_errors_t;

    typedef struct
    {
        system_errors_t error_flags; // Bitmask of active faults (ERR_OVER_TEMP etc.)
        char last_error_msg[32];     // Short description of last fault, always NUL-terminated
                                     // 32 chars fits "Over/Under Voltage\0" with room to spare
                                     // Replaces last_error_log[256] (wasteful) and msg (unsafe ptr)
    } system_fault_state_t;

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

    typedef struct
    {
        bool enabled;
        char ssid[32];
        char password[64];
    } wifi_state_t;

    typedef struct
    {
        bool enabled;
        char device_name[32];
        char pairing_code[16];
    } bluetooth_state_t;

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

    typedef struct
    {
        factory_reset_action_t pending_action;
        bool active;
    } factory_reset_t;

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
        value_edit_context_t *current_value_type;
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

        factory_reset_t factory_reset;

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
        bluetooth_state_t bluetooth;
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

        // LCD status
        lcd_render_state_t lcd_render;
        uint32_t hold_start_time;

    } system_state_t;

#ifdef __cplusplus
}
#endif

#endif // SYSTEM_STATE_H