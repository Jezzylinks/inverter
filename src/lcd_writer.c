/*==============================================================================
  lcd_writer.c
  Implements the public API declared in lcd_writer.h.
  All functions write to sys_state.lcd_render under sys_state_mutex,
  then return immediately.  Zero hardware access here.
==============================================================================*/
#include "lcd_writer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "system_state.h"
#include "lcd_flash_queue.h"
#include "lcd.h"

/* The LCD render instance and mutex are declared by lcd_writer.h. */
extern system_state_t sys_state; /* the single system-state instance */
extern TaskHandle_t lcd_task_handle;

static void lcd_request_refresh(void)
{
    if (lcd_task_handle != NULL) {
        xTaskNotifyGive(lcd_task_handle);
    }
}

static bool s_startup_released = false;

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
    s_startup_released = false;
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

void lcd_main_next_page(void)
{
    LCD_LOCK();
    if (sys_lcd.screen == LCD_SCREEN_MAIN) {
        sys_lcd.main.sub_page =
            (main_sub_page_t)((sys_lcd.main.sub_page + 1U) % MAIN_SUB_COUNT);
    }
    LCD_UNLOCK();
}

void lcd_update_wifi_status(bool connected, int8_t rssi)
{
    LCD_LOCK();
    sys_lcd.main.wifi_connected = connected;
    sys_lcd.main.wifi_rssi = rssi;
    LCD_UNLOCK();
}

void lcd_update_main_power(float pv_kw, float grid_kw, float load_kw,
                           float ac_voltage, uint16_t battery_remaining_minutes,
                           uint8_t voltage_system, uint8_t operating_mode)
{
    LCD_LOCK();
    sys_lcd.main.pv_power_kw = pv_kw;
    sys_lcd.main.grid_power_kw = grid_kw;
    sys_lcd.main.load_power_kw = load_kw;
    sys_lcd.main.ac_voltage = ac_voltage;
    sys_lcd.main.battery_remaining_minutes = battery_remaining_minutes;
    sys_lcd.main.voltage_system = voltage_system;
    sys_lcd.main.operating_mode = operating_mode;
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
    sys_lcd.fault.system_error = false;
    LCD_UNLOCK();
}

void lcd_show_system_error(uint16_t code)
{
    char code_line[LCD_LINE_SIZE];
    if (lcd_geometry_is_20x4()) {
        snprintf(code_line, sizeof(code_line), "Error code: 0x%03X",
                 (unsigned)code & 0x0FFFU);
    } else {
        snprintf(code_line, sizeof(code_line), "ERR 0x%03X",
                 (unsigned)code & 0x0FFFU);
    }
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_FAULT;
    set_line(sys_lcd.fault.line0, "SYSTEM ERROR");
    set_line(sys_lcd.fault.line1, code_line);
    sys_lcd.fault.blink = false;
    sys_lcd.fault.system_error = true;
    LCD_UNLOCK();
    lcd_request_refresh();
}

void lcd_show_inverter_start_error(inverter_start_error_code_t code,
                                   const char *reason)
{
    char code_line[LCD_LINE_SIZE];
    snprintf(code_line, sizeof(code_line), "CODE:E%03X",
             (unsigned)code & 0x0FFFU);
    if (lcd_geometry_is_20x4()) {
        lcd_show_fault(reason != NULL ? reason : "Inverter start failed",
                       code_line);
    } else {
        lcd_show_fault("SYSTEM ERROR", code_line);
    }
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
void lcd_show_wifi_scan_start(void)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_WIFI_SCAN;
    memset(&sys_lcd.wifi_scan, 0, sizeof(sys_lcd.wifi_scan));
    sys_lcd.wifi_scan.stage = LCD_WIFI_SCAN_SCANNING;
    sys_lcd.wifi_scan.entered_ms = _lcd_get_time_ms();
    LCD_UNLOCK();
    lcd_request_refresh();
}

void lcd_update_wifi_scan_results(uint8_t count,
                                  const char ssids[][LCD_WIFI_SSID_MAX_LEN + 1U],
                                  const int8_t rssi[],
                                  const uint8_t channel[],
                                  const uint8_t authmode[],
                                  uint8_t spinner_frame)
{
    LCD_LOCK();
    lcd_wifi_scan_data_t *w = &sys_lcd.wifi_scan;
    const uint8_t n = count < LCD_WIFI_MAX_AP ? count : LCD_WIFI_MAX_AP;
    w->count = n;
    w->spinner_frame = spinner_frame;
    w->stage = LCD_WIFI_SCAN_SCANNING;
    if (w->selected_index >= n) {
        w->selected_index = n > 0U ? n - 1U : 0U;
    }
    for (uint8_t i = 0U; i < n; ++i) {
        strncpy(w->ssid[i], ssids[i], LCD_WIFI_SSID_MAX_LEN);
        w->ssid[i][LCD_WIFI_SSID_MAX_LEN] = '\0';
        w->rssi[i] = rssi[i];
        w->channel[i] = channel ? channel[i] : 0U;
        w->authmode[i] = authmode ? authmode[i] : 0U;
    }
    if (n == 0U) {
        w->top_index = 0U;
    } else {
        const uint8_t visible = lcd_geometry_is_20x4() ? 3U : 2U;
        if (w->selected_index < w->top_index) {
            w->top_index = w->selected_index;
        } else if (w->selected_index >= w->top_index + visible) {
            w->top_index = w->selected_index - visible + 1U;
        }
    }
    LCD_UNLOCK();
    lcd_request_refresh();
}

void lcd_update_wifi_scan_spinner(uint8_t spinner_frame)
{
    LCD_LOCK();
    if (sys_lcd.screen == LCD_SCREEN_WIFI_SCAN &&
        sys_lcd.wifi_scan.stage == LCD_WIFI_SCAN_SCANNING) {
        sys_lcd.wifi_scan.spinner_frame = spinner_frame;
    }
    LCD_UNLOCK();
    lcd_request_refresh();
}

void lcd_show_wifi_scan(uint8_t count,
                        const char ssids[][LCD_WIFI_SSID_MAX_LEN + 1U],
                        const int8_t rssi[], const uint8_t channel[],
                        const uint8_t authmode[], uint8_t selected, uint8_t top)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_WIFI_SCAN;
    lcd_wifi_scan_data_t *w = &sys_lcd.wifi_scan;
    uint8_t n = count < LCD_WIFI_MAX_AP ? count : LCD_WIFI_MAX_AP;
    w->count = n;
    w->selected_index = selected < n ? selected : 0U;
    w->top_index = top;
    w->spinner_frame = 0U;
    w->stage = LCD_WIFI_SCAN_COMPLETE;
    w->entered_ms = _lcd_get_time_ms();
    for (uint8_t i = 0; i < n; i++)
    {
        strncpy(w->ssid[i], ssids[i], LCD_WIFI_SSID_MAX_LEN);
        w->ssid[i][LCD_WIFI_SSID_MAX_LEN] = '\0';
        w->rssi[i] = rssi[i];
        w->channel[i] = channel ? channel[i] : 0U;
        w->authmode[i] = authmode ? authmode[i] : 0U;
    }
    LCD_UNLOCK();
}

void lcd_update_wifi_selection(uint8_t selected, uint8_t top)
{
    LCD_LOCK();
    if (selected < sys_lcd.wifi_scan.count) {
        sys_lcd.wifi_scan.selected_index = selected;
    }
    sys_lcd.wifi_scan.top_index = top;
    sys_lcd.wifi_scan.entered_ms = _lcd_get_time_ms();
    LCD_UNLOCK();
}

void lcd_show_wifi_network_details(const char *ssid, int8_t rssi,
                                   uint8_t channel, uint8_t authmode)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_WIFI_NETWORK_DETAILS;
    memset(&sys_lcd.wifi_network_detail, 0, sizeof(sys_lcd.wifi_network_detail));
    snprintf(sys_lcd.wifi_network_detail.ssid,
             sizeof(sys_lcd.wifi_network_detail.ssid), "%s", ssid ? ssid : "");
    sys_lcd.wifi_network_detail.rssi = rssi;
    sys_lcd.wifi_network_detail.channel = channel;
    sys_lcd.wifi_network_detail.authmode = authmode;
    sys_lcd.wifi_network_detail.entered_ms = _lcd_get_time_ms();
    LCD_UNLOCK();
    lcd_request_refresh();
}

void lcd_update_wifi_network_detail_page(uint8_t page)
{
    LCD_LOCK();
    if (sys_lcd.screen == LCD_SCREEN_WIFI_NETWORK_DETAILS) {
        sys_lcd.wifi_network_detail.page = page % 3U;
        sys_lcd.wifi_network_detail.entered_ms = _lcd_get_time_ms();
    }
    LCD_UNLOCK();
    lcd_request_refresh();
}

void lcd_show_wifi_password(const char *ssid)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_WIFI_PASSWORD;
    memset(&sys_lcd.wifi_password, 0, sizeof(sys_lcd.wifi_password));
    strncpy(sys_lcd.wifi_password.ssid, ssid ? ssid : "",
            LCD_WIFI_SSID_MAX_LEN);
    sys_lcd.wifi_password.ssid[LCD_WIFI_SSID_MAX_LEN] = '\0';
    sys_lcd.wifi_password.current_char = 'a';
    sys_lcd.wifi_password.entered_ms = _lcd_get_time_ms();
    LCD_UNLOCK();
}

void lcd_update_wifi_password(char current_char, const char *password,
                              uint8_t length)
{
    LCD_LOCK();
    sys_lcd.wifi_password.current_char = current_char;
    sys_lcd.wifi_password.length = length < LCD_WIFI_PASSWORD_MAX_LEN
                                        ? length : LCD_WIFI_PASSWORD_MAX_LEN;
    if (password) {
        strncpy(sys_lcd.wifi_password.password, password,
                LCD_WIFI_PASSWORD_MAX_LEN);
        sys_lcd.wifi_password.password[LCD_WIFI_PASSWORD_MAX_LEN] = '\0';
    }
    sys_lcd.wifi_password.entered_ms = _lcd_get_time_ms();
    LCD_UNLOCK();
}

void lcd_show_wifi_status(const char *state, const char *ssid, const char *ip,
                          const char *gateway, int8_t rssi, bool connected,
                          bool got_ip, bool internet_available)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_WIFI_STATUS;
    lcd_wifi_status_data_t *w = &sys_lcd.wifi_status;
    memset(w, 0, sizeof(*w));
    snprintf(w->state, sizeof(w->state), "%s", state ? state : "Unknown");
    snprintf(w->ssid, sizeof(w->ssid), "%s", ssid ? ssid : "Not configured");
    snprintf(w->ip, sizeof(w->ip), "%s", ip ? ip : "0.0.0.0");
    snprintf(w->gateway, sizeof(w->gateway), "%s", gateway ? gateway : "0.0.0.0");
    w->rssi = rssi;
    w->connected = connected;
    w->got_ip = got_ip;
    w->internet_available = internet_available;
    w->entered_ms = _lcd_get_time_ms();
    LCD_UNLOCK();
}

void lcd_update_wifi_status_page(uint8_t page)
{
    LCD_LOCK();
    const uint8_t page_count = lcd_geometry_is_20x4() ? 3U : 4U;
    sys_lcd.wifi_status.page = page % page_count;
    sys_lcd.wifi_status.entered_ms = _lcd_get_time_ms();
    LCD_UNLOCK();
}

void lcd_show_wifi_connecting(const char *ssid)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_WIFI_CONNECTING;
    strncpy(sys_lcd.wifi_connect.ssid, ssid ? ssid : "",
            LCD_WIFI_SSID_MAX_LEN);
    sys_lcd.wifi_connect.ssid[LCD_WIFI_SSID_MAX_LEN] = '\0';
    sys_lcd.wifi_connect.connected = false;
    sys_lcd.wifi_connect.failed = false;
    sys_lcd.wifi_connect.timed_out = false;
    sys_lcd.wifi_connect.entered_ms = _lcd_get_time_ms();
    LCD_UNLOCK();
    lcd_request_refresh();
}

void lcd_show_wifi_result(bool connected, bool failed, bool timed_out,
                          const char *ssid)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_WIFI_CONNECTING;
    strncpy(sys_lcd.wifi_connect.ssid, ssid ? ssid : "",
            LCD_WIFI_SSID_MAX_LEN);
    sys_lcd.wifi_connect.ssid[LCD_WIFI_SSID_MAX_LEN] = '\0';
    sys_lcd.wifi_connect.connected = connected;
    sys_lcd.wifi_connect.failed = failed;
    sys_lcd.wifi_connect.timed_out = timed_out;
    sys_lcd.wifi_connect.entered_ms = _lcd_get_time_ms();
    LCD_UNLOCK();
    lcd_request_refresh();
}

void lcd_show_wifi_clients(uint8_t count,
                           const char macs[][18],
                           uint8_t selected)
{
    LCD_LOCK();
    sys_lcd.screen = LCD_SCREEN_WIFI_CLIENTS;
    lcd_wifi_clients_data_t *clients = &sys_lcd.wifi_clients;
    memset(clients, 0, sizeof(*clients));
    clients->count = count > LCD_WIFI_MAX_CLIENTS ? LCD_WIFI_MAX_CLIENTS : count;
    clients->selected = selected < clients->count ? selected : 0U;
    clients->entered_ms = _lcd_get_time_ms();
    for (uint8_t i = 0U; i < clients->count; ++i) {
        snprintf(clients->mac[i], sizeof(clients->mac[i]), "%s", macs[i]);
    }
    LCD_UNLOCK();
    lcd_request_refresh();
}

void lcd_update_wifi_client_selection(uint8_t selected)
{
    LCD_LOCK();
    if (selected < sys_lcd.wifi_clients.count) {
        sys_lcd.wifi_clients.selected = selected;
        sys_lcd.wifi_clients.entered_ms = _lcd_get_time_ms();
    }
    LCD_UNLOCK();
    lcd_request_refresh();
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
    if (sys_lcd.screen != LCD_SCREEN_STANDBY)
        sys_lcd.standby.page = LCD_STANDBY_PAGE_STATUS;
    sys_lcd.screen = LCD_SCREEN_STANDBY;
    sys_lcd.standby.battery_voltage = bat_v;
    sys_lcd.standby.low_voltage_threshold =
        sys_state.battery_profile.low_voltage_warning_12v;
    sys_lcd.standby.battery_pct = bat_pct;
    sys_lcd.standby.ac_connected = ac_connected;
    sys_lcd.standby.wifi_connected = sys_lcd.main.wifi_connected;
    sys_lcd.standby.wifi_rssi = sys_lcd.main.wifi_rssi;
    LCD_UNLOCK();
}

void lcd_standby_next_page(void)
{
    LCD_LOCK();
    if (sys_lcd.screen == LCD_SCREEN_STANDBY)
    {
        sys_lcd.standby.page =
            (uint8_t)((sys_lcd.standby.page + 1U) % LCD_STANDBY_PAGE_COUNT);
    }
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

bool lcd_is_startup_active(void)
{
    bool active;
    LCD_LOCK();
    active = !s_startup_released ||
             sys_lcd.screen == LCD_SCREEN_BOOT_BRAND ||
             sys_lcd.screen == LCD_SCREEN_BOOT_INIT ||
             sys_lcd.screen == LCD_SCREEN_LOADING ||
             sys_lcd.screen == LCD_SCREEN_STARTUP_SEQ;
    LCD_UNLOCK();
    return active;
}

void lcd_startup_release(void)
{
    s_startup_released = true;
}
