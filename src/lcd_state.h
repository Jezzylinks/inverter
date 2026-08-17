#ifndef LCD_STATE_H
#define LCD_STATE_H
/*==============================================================================
  lcd_state.h
  All data the lcd_task needs to draw any screen.
  NO other file calls lcd_* functions directly.
  Every other task writes to lcd_render_state_t; lcd_task reads and draws.
==============================================================================*/
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "stdatomic.h"
#include "security/factory_reset.h"
#include "lcd_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ── Screen identifiers ─────────────────────────────────────────────────── */
    typedef enum
    {
        LCD_SCREEN_BOOT_BRAND = 0,
        LCD_SCREEN_BOOT_INIT,
        LCD_SCREEN_MAIN,
        LCD_SCREEN_MENU,
        LCD_SCREEN_VALUE_EDIT,
        LCD_SCREEN_MONITORING_DETAIL,
        LCD_SCREEN_DIAGNOSTIC,
        LCD_SCREEN_SETTINGS_VIEW,
        LCD_SCREEN_STARTUP_SEQ,
        LCD_SCREEN_STARTUP_STATUS,
        LCD_SCREEN_SHUTDOWN_SEQ,
        LCD_SCREEN_FAULT,
        LCD_SCREEN_FACTORY_RESET,
        LCD_SCREEN_WIFI_SCAN,
        LCD_SCREEN_WIFI_NETWORK_DETAILS,
        LCD_SCREEN_WIFI_PASSWORD,
        LCD_SCREEN_WIFI_STATUS,
        LCD_SCREEN_WIFI_CONNECTING,
        LCD_SCREEN_WIFI_CLIENTS,
        LCD_SCREEN_CONFIRMATION,
        LCD_SCREEN_FLASH_MSG, /* timed 2-line message then returns */
        LCD_SCREEN_STANDBY,
        LCD_SHOW_TEMP,
        LCD_SHOW_SETTINGS,
        LCD_SHOW_BAT_VOLTAGE,
        LCD_DISPLAY_BATTERY_SETTINGS,
        LCD_SCREEN_LOADING,
        LCD_SCREEN_OTA,
        LCD_SCREEN_SECURITY,
        LCD_SCREEN_PROTECTION,
        LCD_SCREEN_SYSTEM_EVENT,
        LCD_SCREEN_LOGIN,
        LCD_SCREEN_COUNT
    } lcd_screen_id_t;

#define LCD_FLASH_RETURN_AUTO ((lcd_screen_id_t) - 1)

    /* ── Per-screen data payloads ────────────────────────────────────────────── */

    typedef struct
    {
        uint8_t progress_pct;
        char stage[LCD_COLS + 1];
    } lcd_boot_init_data_t;

    typedef enum
    {
        MAIN_SUB_OUTPUT = 0,
        MAIN_SUB_BATTERY,
        MAIN_SUB_SYSTEM,
        MAIN_SUB_NETWORK,
        MAIN_SUB_COUNT
    } main_sub_page_t;

    typedef struct
    {
        float battery_voltage;
        float output_voltage;
        float output_current;
        float output_frequency;
        float battery_temperature;
        uint8_t load_pct;
        uint8_t battery_pct;
        bool inverter_active;
        bool ac_connected;
        bool battery_charging;
        bool battery_low;
        bool battery_critical;
        bool wifi_connected;
        int8_t wifi_rssi;
        float pv_power_kw;
        float grid_power_kw;
        float load_power_kw;
        float ac_voltage;
        uint16_t battery_remaining_minutes;
        uint8_t voltage_system;
        uint8_t operating_mode;
        main_sub_page_t sub_page;
        uint32_t sub_page_last_change_ms;
        uint32_t sub_page_interval_ms;
    } lcd_main_data_t;

    typedef struct
    {
        char title[LCD_LINE_SIZE];

        uint32_t start_ms;
        uint32_t duration_ms;

        lcd_screen_id_t next_screen;

        bool active;

    } lcd_loading_data_t;

    typedef struct
    {
        char row0[LCD_LINE_SIZE];
        char row1[LCD_LINE_SIZE];
    } lcd_two_line_t; /* reused for confirm, saved, and other two-line screens */

    typedef enum
    {
        LCD_OTA_VIEW_CHECKING = 0,
        LCD_OTA_VIEW_PREPARING,
        LCD_OTA_VIEW_DOWNLOADING,
        LCD_OTA_VIEW_VERIFYING,
        LCD_OTA_VIEW_CANCELLING,
        LCD_OTA_VIEW_AVAILABLE,
        LCD_OTA_VIEW_CURRENT,
        LCD_OTA_VIEW_ERROR,
        LCD_OTA_VIEW_CANCELLED,
        LCD_OTA_VIEW_COMPLETE,
    } lcd_ota_view_state_t;

    typedef struct
    {
        lcd_ota_view_state_t state;
        uint8_t progress_pct;
        char current_version[LCD_LINE_SIZE];
        char available_version[LCD_LINE_SIZE];
        char detail[LCD_LINE_SIZE];
        bool retry_available;
        uint32_t entered_ms;
    } lcd_ota_data_t;

    typedef struct
    {
        char rows[LCD_ROWS][LCD_LINE_SIZE];
        uint8_t row_count;
    } lcd_menu_data_t;

    typedef struct
    {
        char label[LCD_LINE_SIZE];
        char value_str[LCD_LINE_SIZE];
        float min_value;
        float max_value;
        float value;
        uint8_t decimal_places;
        char unit[9];
        bool pending_confirm;
    } lcd_value_edit_data_t;

    typedef struct
    {
        char label[LCD_LINE_SIZE];
        char value_str[LCD_LINE_SIZE];
    } lcd_detail_data_t; /* monitoring detail, diagnostic detail, AND settings view */

    typedef struct
    {
        uint8_t progress_pct;
    } lcd_startup_data_t;

    typedef enum
    {
        LCD_STARTUP_STAGE_HARDWARE = 0,
        LCD_STARTUP_STAGE_POWER,
        LCD_STARTUP_STAGE_NETWORK,
        LCD_STARTUP_STAGE_SERVICES,
        LCD_STARTUP_STAGE_SELF_CHECK,
        LCD_STARTUP_STAGE_READY,
        LCD_STARTUP_STAGE_COUNT,
    } lcd_startup_stage_t;

    typedef struct
    {
        lcd_startup_stage_t stage;
        uint32_t stage_started_ms;
        bool post_complete;
        bool post_passed;
        bool lcd_ok;
        bool adc_ok;
        bool fan_ok;
    } lcd_startup_status_data_t;

    typedef struct
    {
        uint8_t progress_pct;
        bool load_warning;
        float load_current;
    } lcd_shutdown_data_t;

    typedef struct
    {
        char line0[LCD_LINE_SIZE];
        char line1[LCD_LINE_SIZE];
        bool blink;
        bool system_error;
    } lcd_fault_data_t;

    typedef enum
    {
        SECURITY_PHASE_IDLE = 0,
        SECURITY_PHASE_PIN_FLOW,    // change_pin_ctx_t owns Enter/Up/Down/Back
        SECURITY_PHASE_VIEW_STATUS, // read-only status screen
    } security_phase_t;

    typedef enum
    {
        SECURITY_ACTION_NONE = 0,
        SECURITY_ACTION_CHANGE_PIN,
        SECURITY_ACTION_VIEW_STATUS,
        SECURITY_ACTION_RESET_PIN,
        SECURITY_ACTION_OTA_AUTH,
    } security_action_t;

    typedef struct
    {
        _Atomic security_phase_t phase;
        _Atomic security_action_t action;
        _Atomic uint8_t menu_selection;
    } lcd_security_data_t;

/* Match the existing Wi-Fi scan capacity so every discovered result remains selectable. */
#define LCD_WIFI_MAX_AP 20
#define LCD_WIFI_SSID_MAX_LEN 32U
#define LCD_WIFI_PASSWORD_MAX_LEN 63U

    typedef enum
    {
        LCD_WIFI_SCAN_SCANNING = 0,
        LCD_WIFI_SCAN_COMPLETE,
        LCD_WIFI_SCAN_FAILED,
    } lcd_wifi_scan_stage_t;

    typedef struct
    {
        char ssid[LCD_WIFI_MAX_AP][LCD_WIFI_SSID_MAX_LEN + 1U];
        int8_t rssi[LCD_WIFI_MAX_AP];
        uint8_t channel[LCD_WIFI_MAX_AP];
        uint8_t authmode[LCD_WIFI_MAX_AP];
        uint8_t count;
        uint8_t selected_index;
        uint8_t top_index;
        uint8_t spinner_frame;
        lcd_wifi_scan_stage_t stage;
        uint32_t entered_ms;
    } lcd_wifi_scan_data_t;

    typedef struct
    {
        char ssid[LCD_WIFI_SSID_MAX_LEN + 1U];
        int8_t rssi;
        uint8_t channel;
        uint8_t authmode;
        uint8_t page;
        uint32_t entered_ms;
    } lcd_wifi_network_detail_data_t;

    typedef struct
    {
        char ssid[LCD_WIFI_SSID_MAX_LEN + 1U];
        char password[LCD_WIFI_PASSWORD_MAX_LEN + 1U];
        int8_t rssi;
        uint8_t authmode;
        uint8_t length;
        char current_char;
        uint32_t entered_ms;
    } lcd_wifi_password_data_t;

    typedef struct
    {
        char state[LCD_LINE_SIZE];
        char ssid[LCD_WIFI_SSID_MAX_LEN + 1U];
        char ip[LCD_LINE_SIZE];
        char gateway[LCD_LINE_SIZE];
        int8_t rssi;
        bool connected;
        bool got_ip;
        bool internet_available;
        uint8_t page;
        uint32_t entered_ms;
    } lcd_wifi_status_data_t;

    typedef struct
    {
        char ssid[LCD_WIFI_SSID_MAX_LEN + 1U];
        char detail[LCD_LINE_SIZE];
        int8_t rssi;
        bool connected;
        bool failed;
        bool timed_out;
        uint32_t entered_ms;
    } lcd_wifi_connect_data_t;

#define LCD_WIFI_MAX_CLIENTS 4U
    typedef struct
    {
        char mac[LCD_WIFI_MAX_CLIENTS][18];
        uint8_t count;
        uint8_t selected;
        uint32_t entered_ms;
    } lcd_wifi_clients_data_t;

    /* timed flash message (SAVED / CANCELLED / any short notice) */
    typedef struct
    {
        char line0[LCD_LINE_SIZE];
        char line1[LCD_LINE_SIZE];
        uint32_t duration_ms;
        lcd_screen_id_t return_to; /* screen to restore when timer expires */
    } lcd_flash_data_t;

#define LCD_STANDBY_PAGE_STATUS 0U
#define LCD_STANDBY_PAGE_BATTERY 1U
#define LCD_STANDBY_PAGE_COUNT 2U

    typedef struct
    {
        float battery_voltage;
        float low_voltage_threshold;
        uint8_t battery_pct;
        bool ac_connected;
        bool wifi_connected;
        int8_t wifi_rssi;
        uint8_t page;
    } lcd_standby_data_t;

    /* ── Master render-state ─────────────────────────────────────────────────── */
    typedef struct
    {
        lcd_screen_id_t screen;
        lcd_boot_init_data_t boot_init;
        lcd_main_data_t main;
        lcd_menu_data_t menu;
        lcd_value_edit_data_t value_edit;
        lcd_detail_data_t monitor_detail;
        lcd_detail_data_t diagnostic;
        lcd_detail_data_t settings_view;
        lcd_startup_data_t startup;
        lcd_startup_status_data_t startup_status;
        lcd_shutdown_data_t shutdown;
        lcd_fault_data_t fault;
        factory_reset_ctx_t factory_reset;
        lcd_security_data_t security;
        lcd_wifi_scan_data_t wifi_scan;
        lcd_wifi_network_detail_data_t wifi_network_detail;
        lcd_wifi_password_data_t wifi_password;
        lcd_wifi_status_data_t wifi_status;
        lcd_wifi_connect_data_t wifi_connect;
        lcd_wifi_clients_data_t wifi_clients;
        lcd_two_line_t confirm;
        lcd_flash_data_t flash;
        lcd_standby_data_t standby;
        lcd_loading_data_t loading;
        lcd_ota_data_t ota;

        /* ✅ Activity tracking for sub-page cycling */
        bool should_cycle_subpages;
        TickType_t last_user_activity;
        bool inverter_active;
        bool inverter_connected;

    } lcd_render_state_t;

#ifdef __cplusplus
}
#endif

#endif // LCD_STATE_H