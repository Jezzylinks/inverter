/*==============================================================================
  lcd_task_complete.c
  lcd_task with ALL advanced features integrated:
    1. Task WDT + heartbeat           (lcd_watchdog.h)
    2. Priority flash message queue   (lcd_flash_queue.h)
    3. Screen corruption detection    (lcd_integrity.h)
==============================================================================*/
#include "lcd_state.h"
#include "lcd_watchdog.h"
#include "lcd_integrity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lcd.h"
#include <string.h>

#include <stdio.h>
#include <math.h>
#include "lcd_flash_queue.h"
#include "lcd_writer.h"
#include <stdatomic.h>
#include "system_state.h"
#include "security/change_pin_flow.h"

#define BOOT_TOTAL_STEPS 3

static uint32_t _lcd_get_time_ms(void);

/* Defined in main.c */
extern SemaphoreHandle_t sys_state_mutex;
extern lcd_render_state_t sys_lcd;
extern active_flash_t s_active_flash;
extern diagnostic_data_t diag_data;
extern change_pin_ctx_t change_pin_ctx;
extern SemaphoreHandle_t change_pin_mutex;

/* lcd.h hardware config — same as original */
#define LCD_ADDR 0x27
#define SDA_PIN 21
#define SCL_PIN 22
#define LCD_BLINK_INTERVAL_MS 500
#define LCD_FLASH_QUEUE_DEPTH 4

static uint8_t loading_progress(uint32_t elapsed,
                                uint32_t duration);

static const char *TAG = "LCD_TASK";

// At top of file, after includes
#define now_ms() (xTaskGetTickCount() * portTICK_PERIOD_MS)

/*==============================================================================
  Draw helpers
  These are the ONLY functions that call lcd_* hardware directly.
==============================================================================*/
static char s_last_row0[17]; /* snapshot for integrity checker */
static char s_last_row1[17];

static void draw_commit(const char *r0, const char *r1)
{
    char row0[17];
    char row1[17];

    snprintf(row0, sizeof(row0), "%-16.16s", r0 ? r0 : "");
    snprintf(row1, sizeof(row1), "%-16.16s", r1 ? r1 : "");

    if (strcmp(row0, s_last_row0) != 0)
    {
        lcd_set_cursor(0, 0);
        lcd_print(row0);
        strcpy(s_last_row0, row0);
    }

    if (strcmp(row1, s_last_row1) != 0)
    {
        lcd_set_cursor(0, 1);
        lcd_print(row1);
        strcpy(s_last_row1, row1);
    }

    lcd_integrity_snapshot(s_last_row0, s_last_row1);
}

static void draw_row(uint8_t row, const char *text)
{
    lcd_set_cursor(0, row);
    lcd_print(text);
    /* Update the relevant snapshot line */
    if (row == 0)
        snprintf(s_last_row0, sizeof(s_last_row0), "%-16.16s", text ? text : "");
    else
        snprintf(s_last_row1, sizeof(s_last_row1), "%-16.16s", text ? text : "");
    lcd_integrity_snapshot(s_last_row0, s_last_row1);
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
  Per-screen draw functions
==============================================================================*/
#define LCD_COLS 16

void lcd_display_startup_screen(uint8_t progress)
{
    draw_commit(" C-TECH SYSTEMS ", "  Version 1.0   ");
}

static void draw_boot_brand(void)
{
    lcd_display_startup_screen(20);
}

static void format_progress_line(char *out, uint8_t pct)
{
    if (pct > 100)
        pct = 100;

    char pct_str[5];
    snprintf(pct_str, sizeof(pct_str), "%3u%%", pct); // "  0%" .. "100%"

    int bar_slots = LCD_COLS - 2 /*brackets*/ - 4 /*" NN%"*/;
    uint8_t filled = (pct * bar_slots) / 100;

    out[0] = '[';
    for (int i = 0; i < bar_slots; i++)
        out[i + 1] = (i < filled) ? '#' : '-';
    out[bar_slots + 1] = ']';
    out[bar_slots + 2] = ' ';
    memcpy(&out[bar_slots + 3], pct_str, 4);
    out[LCD_COLS] = '\0';
}

/**
 * draw_main — Display main screen with sub-page content
 * INSTRUMENTED WITH DETAILED LOGGING
 */
static void draw_main(lcd_main_data_t *m)
{
    static uint8_t last_sub_page = 0xFF;

    /* Log when sub-page changes */
    if (m->sub_page != last_sub_page)
    {
        last_sub_page = m->sub_page;
    }

    char r0[17], r1[17];

    switch (m->sub_page)
    {
    case MAIN_SUB_OUTPUT:
        ESP_LOGD("DRAW_MAIN", "RENDERING: OUTPUT page");
        snprintf(r0, 17, "OUT:%3.0fV %2.0fHz   ", m->output_voltage, m->output_frequency);
        snprintf(r1, 17, "CUR:%4.1fA %4.0fW ", m->output_current,
                 m->output_voltage * m->output_current);
        break;

    case MAIN_SUB_BATTERY:
        ESP_LOGD("DRAW_MAIN", "RENDERING: BATTERY page");
        snprintf(r0, 17, "BAT:%4.1fV %3d%%  ", m->battery_voltage, m->battery_pct);
        snprintf(r1, 17, "TMP:%2.0fC CHG:%s ",
                 m->battery_temperature, m->battery_charging ? "YES" : "NO ");
        break;

    case MAIN_SUB_SYSTEM:
        ESP_LOGD("DRAW_MAIN", "RENDERING: SYSTEM page");
        snprintf(r0, 17, "INV:%s AC:%s  ",
                 m->inverter_active ? "ON " : "OFF",
                 m->ac_connected ? "YES" : "NO ");
        snprintf(r1, 17, "LOAD: %3d%%      ", m->load_pct);
        break;

    default:
        ESP_LOGW("DRAW_MAIN", "⚠️ UNKNOWN_SUB_PAGE: %d", m->sub_page);
        snprintf(r0, 17, "%-16s", "Vonix Inverter  ");
        snprintf(r1, 17, "%-16s", "                ");
        break;
    }
    draw_row(0, r0);
    draw_row(1, r1);

    ESP_LOGD("DRAW_MAIN", "✓ RENDERED: r0='%s' r1='%s'", r0, r1);
}

static void draw_menu(const lcd_two_line_t *d)
{
    draw_commit(d->row0, d->row1);
}

static void draw_value_edit(const lcd_value_edit_data_t *d)
{
    static bool blink_state = false;
    static int64_t last_blink_time_ms = 0;

    draw_commit(d->label, d->value_str);

    /* ── Blinking cursor after the value text ── */
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - last_blink_time_ms > LCD_BLINK_INTERVAL_MS)
    {
        blink_state = !blink_state;
        last_blink_time_ms = now_ms;
    }

    /* Trim trailing padding spaces to find where the cursor goes */
    int len = strlen(d->value_str);
    while (len > 0 && d->value_str[len - 1] == ' ')
        len--;

    if (len < 16)
    {
        lcd_set_cursor(len, 1);
        lcd_print_string(blink_state ? "_" : " ");
    }
}

static void draw_detail(const lcd_detail_data_t *d)
{
    draw_commit(d->label, d->value_str);
}

static void draw_startup(const lcd_startup_data_t *d)
{
    char r1[17];
    snprintf(r1, 17, "Progress: %3d%%  ", d->progress_pct);
    draw_commit("STARTING...     ", r1);
}

static void draw_shutdown(const lcd_shutdown_data_t *d)
{
    if (d->load_warning)
    {
        char r1[17];
        snprintf(r1, 17, "Load:%-6.1fA    ", d->load_current);
        draw_commit("** WARNING! **  ", r1);
    }
    else
    {
        char r1[17];
        snprintf(r1, 17, "Power: %3d%%     ", d->progress_pct);
        draw_commit("RAMP DOWN       ", r1);
    }
}

static void draw_fault(const lcd_fault_data_t *d)
{
    static bool blink = false;
    static uint32_t last_blink = 0;
    if (now_ms() - last_blink > 500)
    {
        blink = !blink;
        last_blink = now_ms();
    }
    if (!d->blink || blink)
        draw_commit(d->line0, d->line1);
    else
        draw_commit("                ", "                ");
}

static void draw_factory_reset(const lcd_factory_reset_data_t *d)
{
    factory_reset_action_t action = atomic_load(&d->action);
    const char *r0, *r1;
    switch (d->phase)
    {
    case FACTORY_PHASE_CONFIRM:
    {
        switch (action)
        {
        case FACTORY_ACTION_CLEAR_SETTINGS:
            r0 = "CLEAR SETTINGS? ";
            r1 = "[OK]=Yes Back=No";
            break;
        case FACTORY_ACTION_ERASE_LOGS:
            r0 = "ERASE LOGS?     ";
            r1 = "[OK]=Yes Back=No";
            break;
        case FACTORY_ACTION_RESET_ALL:
        default:
            r0 = "FACTORY RESET?  ";
            r1 = "[OK]=Yes Back=No";
            break;
        }
        draw_commit(r0, r1);
        break;
    }

    case FACTORY_PHASE_PROGRESS:
    {
        switch (action)
        {
        case FACTORY_ACTION_CLEAR_SETTINGS:
            r0 = "CLEARING...     ";
            break;
        case FACTORY_ACTION_ERASE_LOGS:
            r0 = "ERASING LOGS... ";
            break;
        case FACTORY_ACTION_RESET_ALL:
        default:
            r0 = "RESETTING...    ";
            break;
        }
        // char r1[17];
        //  snprintf(r1, 17, "Progress: %3d%%  ", atomic_load(&d->progress_pct));
        //  draw_commit(r0, r1);
        lcd_show_loading(r0, 1500, LCD_SCREEN_FACTORY_RESET);
        break;
    }

    case FACTORY_RESET_PIN_ENTRY:
    {
        break;
    }

    case FACTORY_PHASE_DONE:
    {
        switch (action)
        {
        case FACTORY_ACTION_CLEAR_SETTINGS:
            r0 = "SETTINGS CLEARED";
            r1 = "DEFAULT LOADED  ";
            break;
        case FACTORY_ACTION_ERASE_LOGS:
            r0 = "LOGS ERASED     ";
            r1 = "System Clean    ";
            break;
        case FACTORY_ACTION_RESET_ALL:
        default:
            r0 = "RESET COMPLETE  ";
            r1 = "Restarting...   ";
            break;
        }
        ESP_LOGI("FLASH_AGAIN", "ReDRAWING AGAIN");
        if (atomic_load(&sys_lcd.factory_reset.action) == FACTORY_ACTION_RESET_ALL)
            draw_commit(r0, r1);
        else
            lcd_flash_info_to(r0, r1, 2000, LCD_SCREEN_MAIN);
        break;
    }

    case FACTORY_PHASE_IDLE:
    {
        static int last_selection = -1;
        if (atomic_load(&d->action) != FACTORY_ACTION_NONE || last_selection < 0)
        {
            /* fall back to a generic prompt if selection context isn't available */
            draw_commit("Factory Reset   ", "Select an option");
            atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_CONFIRM);
        }
        break;
    }
    }
}

static void draw_wifi_scan(const lcd_wifi_scan_data_t *d)
{
    if (d->count == 0)
    {
        draw_commit("No Networks     ", "Found           ");
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
                 d->ssid[idx], rssi_bars(d->rssi[idx]));
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
    draw_commit(r0, r1);
}

static void draw_confirm(const lcd_two_line_t *d) { draw_commit(d->row0, d->row1); }

static void draw_standby(const lcd_standby_data_t *d)
{
    char r1[17];
    if (d->battery_voltage < 10.5f)
        snprintf(r1, 17, "LOW BAT:%4.1fV   ", d->battery_voltage);
    else
        snprintf(r1, 17, "STD_BY BAT:%3d%% ", d->battery_pct);
    draw_commit("Vonix Inverter  ", r1);
}

static uint8_t loading_progress(uint32_t elapsed, uint32_t duration)
{
    if (elapsed >= duration)
        return 100;
    float t = (float)elapsed / duration;
    if (t < 0.80f)
        return (uint8_t)(t * 100.0f);
    float remain = (t - 0.80f) / 0.20f;
    return 80 + (uint8_t)(remain * 20.0f);
}

static void draw_loading_bar(uint8_t pct)
{
    char row[17];
    uint8_t blocks = (pct * 10) / 100;
    row[0] = '[';
    for (int i = 0; i < 10; i++)
        row[i + 1] = (i < blocks) ? 0xFF : '-';
    row[11] = ']';
    /* pad remaining columns so stale characters from a longer
     * previous line (e.g. progress text) don't linger on-screen */
    snprintf(row + 12, sizeof(row) - 12, "%-4s", "");
    row[16] = '\0';
    draw_row(1, row);
}

static void draw_loading(const lcd_loading_data_t *d)
{
    static uint8_t last_pct = 255;
    static uint32_t last_start_ms = 0;

    if (d->start_ms != last_start_ms)
    {
        last_pct = 255;
        last_start_ms = d->start_ms;
    }

    uint32_t elapsed = _lcd_get_time_ms() - d->start_ms;
    uint8_t pct = loading_progress(elapsed, d->duration_ms);

    char row0[17];
    snprintf(row0, sizeof(row0), "%-16.16s", d->title);
    draw_row(0, row0);

    if (pct != last_pct)
    {
        draw_loading_bar(pct);
        last_pct = pct;
    }
}

static void draw_security_status(char line1[17], char line2[17])
{
    if (security_pin_change_required())
    {
        snprintf(line1, 17, "PIN Status:");
        snprintf(line2, 17, "Default (0000)");
    }
    else if (security_is_locked_out())
    {
        uint32_t remaining_s = security_lockout_remaining_ms() / 1000;
        snprintf(line1, 17, "PIN Status:");
        snprintf(line2, 17, "Retry in %lus", remaining_s);
    }
    else
    {
        snprintf(line1, 17, "PIN Status:");
        snprintf(line2, 17, "Custom, OK");
    }
    draw_commit(line1, line2);
}

/*==============================================================================
  LCD reinit helper — called when integrity check fails
==============================================================================*/
void lcd_task_reinit(lcd_screen_id_t *last_screen)
{
    /* ✅ Save important state */
    uint8_t saved_sub_page = sys_lcd.main.sub_page;
    uint32_t saved_interval = sys_lcd.main.sub_page_interval_ms;

    /* Reset LCD state */
    memset(&sys_lcd, 0, sizeof(sys_lcd));

    /* ✅ Restore the preserved values */
    sys_lcd.main.sub_page = saved_sub_page;
    sys_lcd.main.sub_page_interval_ms = saved_interval;

    /* Reinitialize LCD hardware */
    lcd_init(LCD_ADDR, SDA_PIN, SCL_PIN);

    *last_screen = LCD_SCREEN_COUNT; // Force redraw

    ESP_LOGI("LCD_REINIT", "LCD reinitialized, sub_page=%d", sys_lcd.main.sub_page);
}

/*==============================================================================
  lcd_task — THE ONLY FUNCTION THAT CALLS lcd_* HARDWARE

  ARCHITECTURE:
  - Step 1: Watchdog (safety first)
  - Step 2: Snapshot LCD state (under mutex)
  - Step 3: Check LCD integrity (repair if corrupted)
  - Step 4: Handle flashing messages
  - Step 5: Process sub-page cycling (timing-based)
  - Step 6: Detect screen changes (clear if needed)
  - Step 7: Draw the current screen
  - Step 8: Brief task delay
==============================================================================*/

/*==============================================================================
  HELPER FUNCTIONS
==============================================================================*/

/**
 * Get current time in milliseconds (using FreeRTOS ticks)
 */
static uint32_t _lcd_get_time_ms(void)
{
    TickType_t ticks = xTaskGetTickCount();

    return (uint32_t)(ticks * portTICK_PERIOD_MS);
}

extern flash_entry_t s_queue[LCD_FLASH_QUEUE_DEPTH];

void lcd_task(void *arg)
{
    lcd_render_state_t snap;
    static lcd_screen_id_t last_screen = LCD_SCREEN_COUNT;
    bool need_clear = true;

    /* Sub-page cycling state */
    static uint32_t main_page_last_change_ms = 0;
    const uint32_t MAIN_PAGE_INTERVAL_MS = 2000; // 5 seconds

    /* Initialize LCD subsystems */
    sys_lcd.main.sub_page = MAIN_SUB_OUTPUT;
    sys_lcd.main.sub_page_interval_ms = 5000;

    lcd_integrity_init();

    memset(s_last_row0, ' ', 16);
    s_last_row0[16] = '\0';
    memset(s_last_row1, ' ', 16);
    s_last_row1[16] = '\0';

    ESP_LOGI(TAG, "✅ lcd_task started");
    lcd_flash_init(xTaskGetCurrentTaskHandle());

    while (1)
    {

        /* ====== STEP 1: WATCHDOG (always first) ====== */
        lcd_watchdog_feed();
        /* ====== STEP 1b: UPDATE UPTIME ====== */
        /* ====== STEP 2: SNAPSHOT STATE (under mutex) ====== */
        xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
        memcpy(&snap, &sys_lcd, sizeof(snap));
        diag_data.uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        xSemaphoreGive(sys_state_mutex);

        /* ====== STEP 4: FLASH MESSAGE HANDLING ====== */
        /* ✅ KEY: Check if active flash has expired */
        /* In lcd_task loop, STEP 4: FLASH MESSAGE HANDLING */

        /* ✅ Check if active flash has expired */
        if (lcd_flash_is_expired())
        {
            ESP_LOGI(TAG, "✓ Flash expired");

            lcd_screen_id_t fallback_screen = lcd_flash_clear_and_get_return();

            if (lcd_flash_queue_has_pending())
            {
                flash_entry_t next;
                if (lcd_flash_dequeue(&next))
                {
                    lcd_flash_enqueue_to(next.line0, next.line1,
                                         next.duration_ms, next.priority, next.return_to);
                }
            }
            else
            {
                xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
                sys_lcd.screen = fallback_screen;
                xSemaphoreGive(sys_state_mutex);
            }
        }

        /* Check if flash is active and still showing */
        /* ✅ Check if flash is currently active */
        if (lcd_flash_is_active())
        {
            /* Keep showing flash screen */
            snap.screen = LCD_SCREEN_FLASH_MSG;
        }

        /* ====== STEP 5: SUB-PAGE CYCLING (main screen only) ====== */
        /* ✅ FIX: Check cycling but DON'T skip drawing if not cycling */
        /* ====== STEP 5: SUB-PAGE CYCLING (main screen only) ====== */
        if (snap.screen == LCD_SCREEN_MAIN && !lcd_flash_is_active())
        {
            uint32_t now_ms = _lcd_get_time_ms();

            if (main_page_last_change_ms == 0)
            {
                main_page_last_change_ms = now_ms;
            }
            else
            {
                uint32_t elapsed = now_ms - main_page_last_change_ms;

                if (elapsed >= MAIN_PAGE_INTERVAL_MS)
                {
                    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
                    snap.main.sub_page = (snap.main.sub_page + 1) % MAIN_SUB_COUNT;
                    sys_lcd.main.sub_page = snap.main.sub_page;
                    xSemaphoreGive(sys_state_mutex);

                    main_page_last_change_ms = now_ms;
                }
            }
        }

        /* ====== STEP 6: SCREEN CHANGE DETECTION & CLEAR ====== */
        /* Screen changed — always clear */
        if (snap.screen != last_screen)
        {
            lcd_clear();
            memset(s_last_row0, ' ', 16);
            s_last_row0[16] = '\0';
            memset(s_last_row1, ' ', 16);
            s_last_row1[16] = '\0';
            last_screen = snap.screen;
            need_clear = false;
        }
        else if (need_clear)
        {
            lcd_clear();
            need_clear = false;
        }

        /* ====== STEP 7: DRAW CURRENT SCREEN ====== */

        static lcd_screen_id_t last_draw_screen = LCD_SCREEN_COUNT;
        if (snap.screen != last_draw_screen)
        {
            last_draw_screen = snap.screen;
        }
        switch (snap.screen)
        {
        case LCD_SCREEN_BOOT_BRAND:
            draw_boot_brand();
            vTaskDelay(pdMS_TO_TICKS(2000));
            lcd_show_loading("System Loading  ", 2000, LCD_SCREEN_MAIN);
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
        case LCD_SCREEN_FLASH_MSG:

            static char prev_flash_line0[17] = {0};
            static char prev_flash_line1[17] = {0};

            active_flash_t flash;

            if (lcd_flash_get(&flash))
            {
                /* ✅ Compare flash content with previous frame */
                bool flash_line0_changed = (strcmp(prev_flash_line0, flash.line0) != 0);
                bool flash_line1_changed = (strcmp(prev_flash_line1, flash.line1) != 0);
                bool flash_content_changed = flash_line0_changed || flash_line1_changed;

                if (flash_content_changed)
                {
                    /* ✅ Only clear if flash MESSAGE changed (not every frame) */
                    // lcd_clear();

                    /* Save for next frame comparison */
                    strncpy(prev_flash_line0, flash.line0, sizeof(prev_flash_line0) - 1);
                    prev_flash_line0[sizeof(prev_flash_line0) - 1] = '\0';

                    strncpy(prev_flash_line1, flash.line1, sizeof(prev_flash_line1) - 1);
                    prev_flash_line1[sizeof(prev_flash_line1) - 1] = '\0';

                    ESP_LOGI(TAG, "📢 Flash content updated: '%s' / '%s'",
                             flash.line0, flash.line1);
                }
                else
                {
                    /* ✅ Flash content same - skip clear (eliminates flicker!) */
                    ESP_LOGD(TAG, "✓ Flash content unchanged, skipping clear");
                }

                /* Always render the flash (even if we skipped clear) */
                draw_commit(flash.line0, flash.line1);
            }
            break;

        case LCD_SCREEN_LOADING:
        {
            draw_loading(&snap.loading);
            uint32_t elapsed = _lcd_get_time_ms() - snap.loading.start_ms;
            if (elapsed >= snap.loading.duration_ms)
            {
                xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
                sys_lcd.loading.active = false;
                sys_lcd.screen = sys_lcd.loading.next_screen;
                xSemaphoreGive(sys_state_mutex);
            }
            break;
        }

        case LCD_SCREEN_SECURITY:
        {
            char line1[17], line2[17];
            security_phase_t sec_phase = atomic_load(&snap.security.phase);

            if (sec_phase == SECURITY_PHASE_PIN_FLOW)
            {
                change_pin_ctx_t pin_snap;

                if (xSemaphoreTake(change_pin_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
                {
                    memcpy(&pin_snap, &change_pin_ctx, sizeof(pin_snap));
                    xSemaphoreGive(change_pin_mutex);
                }
                else
                {
                    /* Mutex busy this tick -- skip this frame's redraw rather
                     * than block the LCD task or render torn/inconsistent state. */
                    break;
                }

                change_pin_render(&pin_snap, line1, line2);
                draw_commit(line1, line2);
            }
            else if (sec_phase == SECURITY_PHASE_VIEW_STATUS)
            {
                draw_security_status(line1, line2);
            }
            else
            {
                snprintf(line1, 17, "%-16s", "Change PIN      ");
                snprintf(line2, 17, "%-16s", "View  |  Reset  ");
                draw_commit(line1, line2);
            }
            break;
        }

        default:
            ESP_LOGW(TAG, "⚠️ Unknown screen: %d", snap.screen);
            break;
        }

#ifdef LCD_DEBUG_TIMING
        uint32_t draw_end_ms = _lcd_get_time_ms();
        uint32_t draw_time = draw_end_ms - draw_start_ms;
        if (draw_time > 50)
        {
            ESP_LOGW(TAG, "⚠️ Slow draw: %lu ms for screen %d", draw_time, snap.screen);
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(100)); // Small delay to yield CPU
    }

    /* ✅ Use notification-based wakeup:
     * - If no flash pending: Wait 100ms for next cycle (or notification)
     * - If flash enqueued: Notification wakes task immediately
     */
    uint32_t notify_value = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

    if (notify_value > 0)
    {
        /* Woken by notification (flash enqueued) */
        ESP_LOGD(TAG, "🔔 Woken by flash enqueue");
    }
}