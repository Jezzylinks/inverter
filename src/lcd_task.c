/*==============================================================================
  lcd_task.c
  THE ONLY FILE THAT CALLS lcd_clear() / lcd_print() / lcd_set_cursor().
  Runs at fixed 100 ms tick.  Reads lcd_render_state_t, draws to hardware.
==============================================================================*/
#include "lcd_state.h"
#include "lcd_writer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "lcd.h" /* your hardware driver */
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Defined in main.c */
extern SemaphoreHandle_t sys_state_mutex;
extern lcd_render_state_t sys_lcd;

/* ── Timing helpers ──────────────────────────────────────────────────────── */
static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ── Local draw helpers (call hardware directly — we own it) ─────────────── */
static void draw_row(uint8_t row, const char *text)
{
    lcd_set_cursor(0, row);
    lcd_print(text);
}

static void draw_two_lines(const char *r0, const char *r1)
{
    lcd_clear();
    draw_row(0, r0);
    draw_row(1, r1);
}

static void draw_progress_bar(uint8_t row, uint8_t pct)
{
    char bar[17];
    uint8_t filled = (pct * 16 + 50) / 100;
    for (uint8_t i = 0; i < 16; i++)
        bar[i] = (i < filled) ? 0xFF : ' ';
    bar[16] = '\0';
    draw_row(row, bar);
}

static const char *rssi_bars(int8_t rssi)
{
    if (rssi >= -50)
        return "||||";
    if (rssi >= -65)
        return "|||";
    if (rssi >= -75)
        return "||";
    if (rssi >= -85)
        return "|";
    return " ";
}

/*==============================================================================
  Per-screen draw functions — one per lcd_screen_id_t value
==============================================================================*/

static void draw_boot_brand(void)
{
    draw_two_lines("C-TECH SYSTEMS  ", " Starting...    ");
}

static void draw_boot_init(const lcd_boot_init_data_t *d)
{
    draw_row(0, "Initializing... ");
    draw_progress_bar(1, d->progress_pct);
}

/* ── Main rotating screen ────────────────────────────────────────────────── */
static void draw_main(lcd_main_data_t *m)
{
    uint32_t ms = now_ms();

    /* Advance sub-page on timer */
    if ((ms - m->sub_page_last_change_ms) >= m->sub_page_interval_ms)
    {
        m->sub_page = (m->sub_page + 1) % MAIN_SUB_COUNT;
        m->sub_page_last_change_ms = ms;
        lcd_clear();
    }

    char r0[17], r1[17];

    switch (m->sub_page)
    {

    case MAIN_SUB_OUTPUT:
        snprintf(r0, 17, "OUT:%3.0fV %2.0fHz   ", m->output_voltage, m->output_frequency);
        snprintf(r1, 17, "CUR:%4.1fA %4.0fW  ",
                 m->output_current,
                 m->output_voltage * m->output_current);
        break;

    case MAIN_SUB_BATTERY:
        snprintf(r0, 17, "BAT:%4.1fV %3d%%   ", m->battery_voltage, m->battery_pct);
        snprintf(r1, 17, "TMP:%2.0fC CHG:%s  ",
                 m->battery_temperature,
                 m->battery_charging ? "YES" : "NO ");
        break;

    case MAIN_SUB_SYSTEM:
        snprintf(r0, 17, "INV:%s AC:%s   ",
                 m->inverter_active ? "ON " : "OFF",
                 m->ac_connected ? "YES" : "NO ");
        snprintf(r1, 17, "LOAD: %3d%%       ", m->load_pct);
        break;

    default:
        snprintf(r0, 17, "%-16s", "Vonix Inverter  ");
        snprintf(r1, 17, "%-16s", "                ");
        break;
    }

    draw_row(0, r0);
    draw_row(1, r1);
}

static void draw_menu(const lcd_two_line_t *d)
{
    draw_two_lines(d->row0, d->row1);
}

static void draw_value_edit(const lcd_value_edit_data_t *d)
{
    if (d->pending_confirm)
        draw_two_lines(d->label, "Confirm? ENT/NO ");
    else
        draw_two_lines(d->label, d->value_str);
}

static void draw_detail(const lcd_detail_data_t *d)
{
    draw_two_lines(d->label, d->value_str);
}

static void draw_startup(const lcd_startup_data_t *d)
{
    char r1[17];
    snprintf(r1, 17, "Progress: %3d%%  ", d->progress_pct);
    draw_row(0, "STARTING...     ");
    draw_row(1, r1);
}

static void draw_shutdown(const lcd_shutdown_data_t *d)
{
    if (d->load_warning)
    {
        char r1[17];
        snprintf(r1, 17, "Load:%-6.1fA     ", d->load_current);
        draw_two_lines("** WARNING! **  ", r1);
    }
    else
    {
        char r1[17];
        snprintf(r1, 17, "Power: %3d%%     ", d->progress_pct);
        draw_two_lines("RAMP DOWN       ", r1);
    }
}

static void draw_fault(const lcd_fault_data_t *d)
{
    /* blink is driven by the task tick */
    static bool blink_state = false;
    static uint32_t last_blink = 0;
    uint32_t ms = now_ms();
    if (ms - last_blink > 500)
    {
        blink_state = !blink_state;
        last_blink = ms;
    }
    if (!d->blink || blink_state)
        draw_two_lines(d->line0, d->line1);
    else
        draw_two_lines("                ", "                ");
}

static void draw_factory_reset(const lcd_factory_reset_data_t *d)
{
    switch (d->phase)
    {
    case FACTORY_PHASE_CONFIRM:
        draw_two_lines("FACTORY RESET?  ", "Hold=Yes Back=No");
        break;
    case FACTORY_PHASE_PROGRESS:
    {
        char r1[17];
        snprintf(r1, 17, "Progress: %3d%%  ", d->progress_pct);
        draw_two_lines("RESETTING...    ", r1);
        break;
    }
    case FACTORY_PHASE_DONE:
        draw_two_lines("RESET COMPLETE  ", "Restarting...   ");
        break;
    }
}

static void draw_wifi_scan(const lcd_wifi_scan_data_t *d)
{
    if (d->count == 0)
    {
        draw_two_lines("No Networks     ", "Found           ");
        return;
    }
    lcd_clear();
    for (uint8_t line = 0; line < 2; line++)
    {
        uint8_t idx = d->top_index + line;
        if (idx >= d->count)
            break;
        char row[17];
        snprintf(row, 17, "%c%-8s %4s  ",
                 (idx == d->selected_index) ? '>' : ' ',
                 d->ssid[idx],
                 rssi_bars(d->rssi[idx]));
        draw_row(line, row);
    }
}

static void draw_wifi_connecting(const lcd_wifi_connect_data_t *d)
{
    char r0[17], r1[17];
    if (d->connected)
    {
        snprintf(r0, 17, "%-16s", "Wi-Fi Connected ");
        snprintf(r1, 17, "%-16.16s", d->ssid);
    }
    else if (d->failed)
    {
        snprintf(r0, 17, "%-16s", "Wi-Fi Failed    ");
        snprintf(r1, 17, "%-16s", "Check SSID/Pass ");
    }
    else if (d->timed_out)
    {
        snprintf(r0, 17, "%-16s", "Wi-Fi Timeout   ");
        snprintf(r1, 17, "%-16s", "No Connection   ");
    }
    else
    {
        snprintf(r0, 17, "%-16s", "Connecting Wi-Fi");
        snprintf(r1, 17, "%-16.16s", d->ssid);
    }
    draw_two_lines(r0, r1);
}

static void draw_confirm(const lcd_two_line_t *d)
{
    draw_two_lines(d->row0, d->row1);
}

static void draw_flash(const lcd_flash_data_t *d)
{
    draw_two_lines(d->line0, d->line1);
}

static void draw_standby(const lcd_standby_data_t *d)
{
    char r0[17], r1[17];
    snprintf(r0, 17, "Vonix Inverter  ");
    if (d->battery_voltage < 10.5f)
        snprintf(r1, 17, "LOW BAT:%4.1fV   ", d->battery_voltage);
    else
        snprintf(r1, 17, "STD_BY BAT:%3d%%  ", d->battery_pct);
    draw_two_lines(r0, r1);
}

/*==============================================================================
  lcd_task — the ONLY task that owns the display
==============================================================================*/
void lcd_task(void *arg)
{
    lcd_render_state_t snap;                        /* local snapshot — no lock while drawing */
    lcd_screen_id_t last_screen = LCD_SCREEN_COUNT; /* force first clear */

    while (1)
    {
        /* ── 1. Snapshot render state under mutex ───────────────────────── */
        xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
        memcpy(&snap, &sys_lcd, sizeof(snap));
        xSemaphoreGive(sys_state_mutex);

        /* ── 2. Handle timed flash expiry ───────────────────────────────── */
        if (snap.screen == LCD_SCREEN_FLASH_MSG)
        {
            uint32_t elapsed = now_ms();
            /* We compare against an absolute deadline stored in flash.duration_ms.
               The writer sets duration_ms to (now_ms() + desired_ms) at write time.
               We reinterpret it here as an absolute expiry. */
            if (elapsed >= snap.flash.duration_ms)
            {
                /* Flash expired — restore previous screen */
                xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
                sys_lcd.screen = sys_lcd.flash.return_to;
                xSemaphoreGive(sys_state_mutex);
                snap.screen = snap.flash.return_to;
            }
        }

        /* ── 3. Clear on screen change ──────────────────────────────────── */
        if (snap.screen != last_screen)
        {
            lcd_clear();
            last_screen = snap.screen;
        }

        /* ── 4. Draw ────────────────────────────────────────────────────── */
        switch (snap.screen)
        {
        case LCD_SCREEN_BOOT_BRAND:
            draw_boot_brand();
            break;
        case LCD_SCREEN_BOOT_INIT:
            draw_boot_init(&snap.boot_init);
            break;
        case LCD_SCREEN_MAIN:
            draw_main(&snap.main);
            break;
        case LCD_SCREEN_MENU:
            draw_menu(&snap.menu);
            break;
        case LCD_SCREEN_VALUE_EDIT:
            draw_value_edit(&snap.value_edit);
            break;
        case LCD_SCREEN_MONITORING_DETAIL:
            draw_detail(&snap.monitor_detail);
            break;
        case LCD_SCREEN_DIAGNOSTIC:
            draw_detail(&snap.diagnostic);
            break;
        case LCD_SCREEN_STARTUP_SEQ:
            draw_startup(&snap.startup);
            break;
        case LCD_SCREEN_SHUTDOWN_SEQ:
            draw_shutdown(&snap.shutdown);
            break;
        case LCD_SCREEN_FAULT:
            draw_fault(&snap.fault);
            break;
        case LCD_SCREEN_FACTORY_RESET:
            draw_factory_reset(&snap.factory_reset);
            break;
        case LCD_SCREEN_WIFI_SCAN:
            draw_wifi_scan(&snap.wifi_scan);
            break;
        case LCD_SCREEN_WIFI_CONNECTING:
            draw_wifi_connecting(&snap.wifi_connect);
            break;
        case LCD_SCREEN_CONFIRMATION:
            draw_confirm(&snap.confirm);
            break;
        case LCD_SCREEN_FLASH_MSG:
            draw_flash(&snap.flash);
            break;
        case LCD_SCREEN_STANDBY:
            draw_standby(&snap.standby);
            break;
        default:
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}