#pragma once
/*==============================================================================
  lcd_writer.h
  Public API for every task that needs to change what is on the LCD.
  These are the ONLY functions any task outside lcd_task.c should call.
  None of them touch lcd hardware — they only write to lcd_render_state_t
  and set sys_state_mutex where needed.
==============================================================================*/
#include "lcd_state.h"
#include <stdint.h>
#include <stdbool.h>

/* Helper: take mutex, guaranteed short hold */
#define LCD_LOCK() xSemaphoreTake(sys_state_mutex, portMAX_DELAY)
#define LCD_UNLOCK() xSemaphoreGive(sys_state_mutex)

/* Call once at startup before any task runs */
void lcd_writer_init(void);

/* ── Boot ────────────────────────────────────────────────────────────────── */
void lcd_show_boot_brand(void);
void lcd_show_boot_init(uint8_t progress_pct);
void lcd_boot_complete(void); /* switches to LCD_SCREEN_MAIN    */
void lcd_show_loading(const char *title,
                      uint32_t duration_ms,
                      lcd_screen_id_t next_screen);

/* ── Normal operating screen ─────────────────────────────────────────────── */
/* Call from adc_task every cycle — lcd_task reads and decides sub-page      */
void lcd_update_main_data(float bat_v, float out_v, float out_a,
                          float out_hz, float bat_temp,
                          uint8_t load_pct, uint8_t bat_pct,
                          bool inv_active, bool ac_connected,
                          bool bat_charging);

void lcd_show_main(void); /* switch to main screen          */

/* ── Menu ────────────────────────────────────────────────────────────────── */
/* Caller pre-formats both rows (16 chars each, space-padded)                */
void lcd_show_menu(const char *row0, const char *row1);

/* ── Value editing ───────────────────────────────────────────────────────── */
void lcd_show_value_edit(const char *label, const char *value_str,
                         bool pending_confirm);

/* ── Monitoring / diagnostic detail ─────────────────────────────────────── */
void lcd_show_monitor_detail(const char *label, const char *value_str);
void lcd_show_diagnostic_detail(const char *label, const char *value_str);

/* ── Inverter sequences ──────────────────────────────────────────────────── */
void lcd_show_startup_progress(uint8_t pct);
void lcd_show_shutdown_progress(uint8_t pct, bool load_warn, float load_a);

/* ── Faults ──────────────────────────────────────────────────────────────── */
void lcd_show_fault(const char *line0, const char *line1);
void lcd_clear_fault(void); /* returns to LCD_SCREEN_MAIN     */

/* ── Factory reset ───────────────────────────────────────────────────────── */
void lcd_show_factory_confirm(void);
void lcd_show_factory_progress();
void lcd_show_factory_done(void);

/* ── Wi-Fi ───────────────────────────────────────────────────────────────── */
void lcd_show_wifi_scan(uint8_t count,
                        const char ssids[][9], const int8_t rssi[],
                        uint8_t selected, uint8_t top);
void lcd_update_wifi_selection(uint8_t selected, uint8_t top);
void lcd_show_wifi_connecting(const char *ssid);
void lcd_show_wifi_result(bool connected, bool failed, bool timed_out,
                          const char *ssid);

/* ── Generic confirmation prompt ─────────────────────────────────────────── */
void lcd_show_confirm(const char *line0, const char *line1);

/* ── Timed flash message (auto-returns to previous screen) ───────────────── */
/* duration_ms: how long to show the message before restoring current screen */
void lcd_flash_message(const char *line0, const char *line1,
                       uint32_t duration_ms);

/* Convenience wrappers used in many places */
void lcd_flash_saved(const char *label, const char *value_str);
void lcd_flash_cancelled(void);

/* ── Standby ─────────────────────────────────────────────────────────────── */
void lcd_show_standby(float bat_v, uint8_t bat_pct, bool ac_connected);