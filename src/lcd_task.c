#include "lcd_state.h"
#include "lcd_flash_queue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "lcd.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

extern SemaphoreHandle_t sys_state_mutex;
extern lcd_render_state_t sys_lcd;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ── Local draw helpers (identical to lcd_task.c) ─────────────────────── */
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

/* ── Forward declarations for per-screen draw functions ──────────────── */
static void draw_boot_brand(void);
static void draw_boot_init(const lcd_boot_init_data_t *d);
static void draw_main(lcd_main_data_t *m);
static void draw_menu(const lcd_two_line_t *d);
static void draw_value_edit(const lcd_value_edit_data_t *d);
static void draw_detail(const lcd_detail_data_t *d);
static void draw_startup(const lcd_startup_data_t *d);
static void draw_shutdown(const lcd_shutdown_data_t *d);
static void draw_fault(const lcd_fault_data_t *d);
static void draw_factory_reset(const lcd_factory_reset_data_t *d);
static void draw_wifi_scan(const lcd_wifi_scan_data_t *d);
static void draw_wifi_connecting(const lcd_wifi_connect_data_t *d);
static void draw_confirm(const lcd_two_line_t *d);
static void draw_standby(const lcd_standby_data_t *d);

/*==============================================================================
  Flash message state — tracks the currently showing flash
==============================================================================*/
typedef struct
{
    bool active;
    char line0[17];
    char line1[17];
    uint32_t expire_ms;        /* absolute ms when flash should end   */
    flash_priority_t priority; /* priority of the flash being shown   */
    lcd_screen_id_t return_to; /* screen to restore when done         */
} active_flash_t;

static active_flash_t s_flash = {.active = false};

/*
 * Start showing a flash message.
 * Replaces the current flash only if new priority >= current priority.
 */
static void start_flash(const flash_entry_t *entry)
{
    if (s_flash.active && entry->priority < s_flash.priority)
        return; /* lower priority — don't interrupt current flash */

    s_flash.active = true;
    s_flash.priority = entry->priority;
    s_flash.expire_ms = now_ms() + entry->duration_ms;
    s_flash.return_to = entry->return_to;
    memcpy(s_flash.line0, entry->line0, sizeof(s_flash.line0));
    memcpy(s_flash.line1, entry->line1, sizeof(s_flash.line1));

    /* Draw immediately — no waiting for next tick */
    draw_two_lines(s_flash.line0, s_flash.line1);
}

/*==============================================================================
  Per-screen draw functions (identical to lcd_task.c)
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

static void draw_main(lcd_main_data_t *m)
{
    uint32_t ms = now_ms();
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
        snprintf(r1, 17, "CUR:%4.1fA %4.0fW  ", m->output_current,
                 m->output_voltage * m->output_current);
        break;
    case MAIN_SUB_BATTERY:
        snprintf(r0, 17, "BAT:%4.1fV %3d%%   ", m->battery_voltage, m->battery_pct);
        snprintf(r1, 17, "TMP:%2.0fC CHG:%s  ", m->battery_temperature,
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

static void draw_menu(const lcd_two_line_t *d) { draw_two_lines(d->row0, d->row1); }

static void draw_value_edit(const lcd_value_edit_data_t *d)
{
    if (d->pending_confirm)
        draw_two_lines(d->label, "Confirm? ENT/NO ");
    else
        draw_two_lines(d->label, d->value_str);
}

static void draw_detail(const lcd_detail_data_t *d) { draw_two_lines(d->label, d->value_str); }

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
    static bool blink = false;
    static uint32_t last_blink = 0;
    uint32_t ms = now_ms();
    if (ms - last_blink > 500)
    {
        blink = !blink;
        last_blink = ms;
    }
    if (!d->blink || blink)
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

static void draw_confirm(const lcd_two_line_t *d) { draw_two_lines(d->row0, d->row1); }

static void draw_standby(const lcd_standby_data_t *d)
{
    char r1[17];
    if (d->battery_voltage < 10.5f)
        snprintf(r1, 17, "LOW BAT:%4.1fV   ", d->battery_voltage);
    else
        snprintf(r1, 17, "STD_BY BAT:%3d%%  ", d->battery_pct);
    draw_two_lines("Vonix Inverter  ", r1);
}

/*==============================================================================
  lcd_task — the ONLY function that calls lcd_* hardware functions
==============================================================================*/
void lcd_task(void *arg)
{
    lcd_render_state_t snap;
    lcd_screen_id_t last_screen = LCD_SCREEN_COUNT;

    while (1)
    {
        /* ── 1. Snapshot render state ─────────────────────────────────── */
        xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
        memcpy(&snap, &sys_lcd, sizeof(snap));
        xSemaphoreGive(sys_state_mutex);

        /* ── 2. Flash queue: check for new messages ───────────────────── */
        /*
         * Check the queue every tick.
         * - If no flash is active: dequeue and start the next one.
         * - If a flash IS active and a higher-priority message is waiting:
         *   start_flash() will preempt the current one.
         * - If current flash has expired: clear and fall through to normal draw.
         */
        if (lcd_flash_queue_has_pending())
        {
            flash_entry_t next;
            if (lcd_flash_dequeue(&next))
            {
                /* start_flash() only replaces current if new >= current priority */
                start_flash(&next);
            }
        }

        /* ── 3. If a flash is showing, check expiry ───────────────────── */
        if (s_flash.active)
        {
            if (now_ms() >= s_flash.expire_ms)
            {
                /* Flash expired — restore previous screen */
                s_flash.active = false;

                /* Write return_to into render state so normal draw resumes */
                xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
                sys_lcd.screen = s_flash.return_to;
                xSemaphoreGive(sys_state_mutex);
                snap.screen = s_flash.return_to;
                last_screen = LCD_SCREEN_COUNT; /* force clear on next draw */
            }
            else
            {
                /* Flash still showing — skip normal draw entirely */
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }

        /* ── 4. Clear on screen change ────────────────────────────────── */
        if (snap.screen != last_screen)
        {
            lcd_clear();
            last_screen = snap.screen;
        }

        /* ── 5. Normal screen draw ────────────────────────────────────── */
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
        case LCD_SCREEN_STANDBY:
            draw_standby(&snap.standby);
            break;
        default:
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}