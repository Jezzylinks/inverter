/*==============================================================================
  lcd_writer.c
  Implements the public API declared in lcd_writer.h.
  All functions write to sys_state.lcd_render under sys_state_mutex,
  then return immediately.  Zero hardware access here.
==============================================================================*/
#include "lcd_writer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "system_state.h"
#include "lcd_flash_queue.h"

/* These are defined in main.c */
extern SemaphoreHandle_t sys_state_mutex;
extern lcd_render_state_t sys_lcd; /* the single render-state instance */
extern system_state_t sys_state;   /* the single system-state instance */

/**
 * Get current time in milliseconds (using FreeRTOS ticks)
 */
static uint32_t _lcd_get_time_ms(void)
{
    TickType_t ticks = xTaskGetTickCount();

    return (uint32_t)(ticks * portTICK_PERIOD_MS);
}

/* Safely copy a string into the active LCD row width. */
static void set_line(char *dst, const char *src)
{
    snprintf(dst, LCD_LINE_SIZE, "%-*.*s", LCD_COLS, LCD_COLS,
             src ? src : "");
}

/*----------------------------------------------------------------------------*/
void lcd_writer_init(void)
{
    LCD_LOCK();
    memset(&sys_lcd, 0, sizeof(sys_lcd));
    sys_lcd.screen = LCD_SCREEN_BOOT_BRAND;
    sys_lcd.main.sub_page_interval_ms = 3000;
    LCD_UNLOCK();
}

/* ── Boot ────────────────────────────────────────────────────────────────── */
void lcd_show_boot_brand(void)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_BOOT_BRAND;
    LCD_UNLOCK();
}

void lcd_show_boot_init(uint8_t pct)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_BOOT_INIT;
    sys_lcd.boot_init.progress_pct = pct;
    LCD_UNLOCK();
}

void lcd_boot_complete(void)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_MAIN;
    LCD_UNLOCK();
}

/* ── Normal operating ────────────────────────────────────────────────────── */
void lcd_update_main_data(float bat_v, float out_v, float out_a,
                          float out_hz, float bat_temp,
                          uint8_t load_pct, uint8_t bat_pct,
                          bool inv_active, bool ac_connected,
                          bool bat_charging)
{
    LCD_LOCK();
    lcd_main_data_t *m = &sys_lcd.main;
    m->battery_voltage = bat_v;
    m->output_voltage = out_v;
    m->output_current = out_a;
    m->output_frequency = out_hz;
    m->battery_temperature = bat_temp;
    m->load_pct = load_pct;
    m->battery_pct = bat_pct;
    m->inverter_active = inv_active;
    m->ac_connected = ac_connected;
    m->battery_charging = bat_charging;
    LCD_UNLOCK();
}

void lcd_show_main(void)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_MAIN;
    LCD_UNLOCK();
}

/* ── Menu ────────────────────────────────────────────────────────────────── */
void lcd_show_menu_rows(const char *const rows[], uint8_t row_count)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_MENU;
    if (row_count > LCD_ROWS)
        row_count = LCD_ROWS;
    sys_lcd.menu.row_count = row_count;
    for (uint8_t row = 0; row < LCD_ROWS; ++row)
        set_line(sys_lcd.menu.rows[row],
                 (rows != NULL && row < row_count) ? rows[row] : "");
    LCD_UNLOCK();
}

void lcd_show_menu(const char *row0, const char *row1)
{
    const char *rows[] = {row0, row1};
    lcd_show_menu_rows(rows, 2);
}

void lcd_request_geometry_reconfigure(void)
{
    LCD_LOCK();
    sys_lcd.geometry_reinit_requested = true;
    LCD_UNLOCK();
}

/* ── Value edit ──────────────────────────────────────────────────────────── */
void lcd_show_value_edit(const char *label, const char *value_str,
                         bool pending_confirm)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_VALUE_EDIT;
    set_line(sys_lcd.value_edit.label, label);
    set_line(sys_lcd.value_edit.value_str, value_str);
    sys_lcd.value_edit.pending_confirm = pending_confirm;
    LCD_UNLOCK();
}

/* ── Detail screens ──────────────────────────────────────────────────────── */
void lcd_show_monitor_detail(const char *label, const char *value_str)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_MONITORING_DETAIL;
    set_line(sys_lcd.monitor_detail.label, label);
    set_line(sys_lcd.monitor_detail.value_str, value_str);
    LCD_UNLOCK();
}

void lcd_show_diagnostic_detail(const char *label, const char *value_str)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_DIAGNOSTIC;
    set_line(sys_lcd.diagnostic.label, label);
    set_line(sys_lcd.diagnostic.value_str, value_str);
    LCD_UNLOCK();
}

void lcd_show_settings_view_detail(const char *label, const char *value_str)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_SETTINGS_VIEW;
    set_line(sys_lcd.settings_view.label, label);
    set_line(sys_lcd.settings_view.value_str, value_str);
    LCD_UNLOCK();
}

/* ── Inverter sequences ──────────────────────────────────────────────────── */
void lcd_show_startup_progress(uint8_t pct)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_STARTUP_SEQ;
    sys_lcd.startup.progress_pct = pct;
    LCD_UNLOCK();
}

void lcd_show_shutdown_progress(uint8_t pct, bool load_warn, float load_a)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_SHUTDOWN_SEQ;
    sys_lcd.shutdown.progress_pct = pct;
    sys_lcd.shutdown.load_warning = load_warn;
    sys_lcd.shutdown.load_current = load_a;
    LCD_UNLOCK();
}

/* ── Faults ──────────────────────────────────────────────────────────────── */
void lcd_show_fault(const char *line0, const char *line1)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_FAULT;
    set_line(sys_lcd.fault.line0, line0);
    set_line(sys_lcd.fault.line1, line1);
    sys_lcd.fault.blink = true;
    LCD_UNLOCK();
}

void lcd_clear_fault(void)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_MAIN;
    memset(&sys_lcd.fault, 0, sizeof(sys_lcd.fault));
    LCD_UNLOCK();
}

/* ── Factory reset ───────────────────────────────────────────────────────── */
void lcd_show_factory_confirm(void)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_FACTORY_RESET;
    sys_lcd.factory_reset.phase = FACTORY_PHASE_CONFIRM;
    sys_lcd.factory_reset.progress_pct = 0;
    LCD_UNLOCK();
}

void lcd_show_factory_progress()
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_FACTORY_RESET;
    sys_lcd.factory_reset.phase = FACTORY_PHASE_PROGRESS;
    LCD_UNLOCK();
}

void lcd_show_factory_done(void)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_FACTORY_RESET;
    sys_lcd.factory_reset.phase = FACTORY_PHASE_DONE;
    LCD_UNLOCK();
}

/* ── Wi-Fi ───────────────────────────────────────────────────────────────── */
void lcd_show_wifi_scan(uint8_t count,
                        const char ssids[][9], const int8_t rssi[],
                        uint8_t selected, uint8_t top)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_WIFI_SCAN;
    lcd_wifi_scan_data_t *w = &sys_lcd.wifi_scan;
    uint8_t n = count < LCD_WIFI_MAX_AP ? count : LCD_WIFI_MAX_AP;
    w->count = n;
    w->selected_index = selected;
    w->top_index = top;
    for (uint8_t i = 0; i < n; i++)
    {
        strncpy(w->ssid[i], ssids[i], 8);
        w->ssid[i][8] = '\0';
        w->rssi[i] = rssi[i];
    }
    LCD_UNLOCK();
}

void lcd_update_wifi_selection(uint8_t selected, uint8_t top)
{
    LCD_LOCK();
    sys_lcd.wifi_scan.selected_index = selected;
    sys_lcd.wifi_scan.top_index = top;
    LCD_UNLOCK();
}

void lcd_show_wifi_connecting(const char *ssid)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_WIFI_CONNECTING;
    strncpy(sys_lcd.wifi_connect.ssid, ssid, 32);
    sys_lcd.wifi_connect.ssid[32] = '\0';
    sys_lcd.wifi_connect.connected = false;
    sys_lcd.wifi_connect.failed = false;
    sys_lcd.wifi_connect.timed_out = false;
    LCD_UNLOCK();
}

void lcd_show_wifi_result(bool connected, bool failed, bool timed_out,
                          const char *ssid)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_WIFI_CONNECTING;
    strncpy(sys_lcd.wifi_connect.ssid, ssid, 32);
    sys_lcd.wifi_connect.ssid[32] = '\0';
    sys_lcd.wifi_connect.connected = connected;
    sys_lcd.wifi_connect.failed = failed;
    sys_lcd.wifi_connect.timed_out = timed_out;
    LCD_UNLOCK();
}

/* ── Confirmation prompt ─────────────────────────────────────────────────── */
void lcd_show_confirm(const char *line0, const char *line1)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_CONFIRMATION;
    set_line(sys_lcd.confirm.row0, line0);
    set_line(sys_lcd.confirm.row1, line1);
    LCD_UNLOCK();
}

/* ── Flash message ───────────────────────────────────────────────────────── */
void lcd_flash_message(const char *line0, const char *line1,
                       uint32_t duration_ms)
{
    lcd_flash_enqueue_to(line0, line1, duration_ms, FLASH_PRIORITY_NORMAL,
                         LCD_FLASH_RETURN_AUTO);
}

void lcd_flash_saved(const char *label, const char *value_str)
{
    /* Show "Value Saved! / <label>: <value>" for 800ms */
    char line1[LCD_LINE_SIZE];
    snprintf(line1, sizeof(line1), "%-*.*s", LCD_COLS, LCD_COLS, value_str ? value_str : "");
    lcd_flash_message("Value Saved!    ", line1, 800);
}

void lcd_flash_cancelled(void)
{
    lcd_flash_message("Edit Cancelled  ", "                ", 1000);
}

/* ── Standby ─────────────────────────────────────────────────────────────── */
void lcd_show_standby(float bat_v, uint8_t bat_pct, bool ac_connected)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_STANDBY;
    sys_lcd.standby.battery_voltage = bat_v;
    sys_lcd.standby.low_voltage_threshold =
        sys_state.battery_profile.low_voltage_warning_12v;
    sys_lcd.standby.battery_pct = bat_pct;
    sys_lcd.standby.ac_connected = ac_connected;
    LCD_UNLOCK();
}

void lcd_show_loading(const char *title,
                      uint32_t duration_ms,
                      lcd_screen_id_t next_screen)
{
    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);

    memset(&sys_lcd.loading, 0, sizeof(sys_lcd.loading));

    snprintf(sys_lcd.loading.title,
             sizeof(sys_lcd.loading.title),
             "%-16.16s",
             title);

    sys_lcd.loading.start_ms = _lcd_get_time_ms();
    sys_lcd.loading.duration_ms = duration_ms;
    sys_lcd.loading.next_screen = next_screen;
    sys_lcd.loading.active = true;

    sys_lcd.screen = LCD_SCREEN_LOADING;

    xSemaphoreGive(sys_state_mutex);
}
