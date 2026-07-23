/*==============================================================================
  lcd_task_complete.c
  lcd_task with ALL advanced features integrated:
    1. Task WDT + heartbeat           (lcd_watchdog.h)
    2. Priority flash message queue   (lcd_flash_queue.h)
    3. Screen corruption detection    (lcd_integrity.h)
    4. Security screen + PIN flow     (security.h / change_pin_flow.h)
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
#include "security/security.h"
#include "security/change_pin_flow.h"
#include "security/pin_entry.h"

#define BOOT_TOTAL_STEPS 3

static uint32_t _lcd_get_time_ms(void);

/* Defined in main.c */
extern SemaphoreHandle_t sys_state_mutex;
extern lcd_render_state_t sys_lcd;
extern active_flash_t s_active_flash;
extern diagnostic_data_t diag_data;
extern change_pin_ctx_t change_pin_ctx; /* no static -- external linkage */
extern SemaphoreHandle_t change_pin_mutex;
extern system_state_t sys_state;

/* lcd.h hardware config */
#define LCD_ADDR 0x27
#define SDA_PIN 21
#define SCL_PIN 22
#define LCD_BLINK_INTERVAL_MS 500
#define LCD_FLASH_QUEUE_DEPTH 4

static uint8_t loading_progress(uint32_t elapsed, uint32_t duration);

static const char *TAG = "LCD_TASK";

#define now_ms() (xTaskGetTickCount() * portTICK_PERIOD_MS)

/*==============================================================================
  Draw helpers — ONLY functions that call lcd_* hardware directly.
==============================================================================*/
static char s_last_row0[17];
static char s_last_row1[17];

static void draw_commit(const char *r0, const char *r1)
{
    char row0[17], row1[17];
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
    snprintf(pct_str, sizeof(pct_str), "%3u%%", pct);
    int bar_slots = LCD_COLS - 2 - 4;
    uint8_t filled = (pct * bar_slots) / 100;
    out[0] = '[';
    for (int i = 0; i < bar_slots; i++)
        out[i + 1] = (i < filled) ? '#' : '-';
    out[bar_slots + 1] = ']';
    out[bar_slots + 2] = ' ';
    memcpy(&out[bar_slots + 3], pct_str, 4);
    out[LCD_COLS] = '\0';
}

static void draw_main(lcd_main_data_t *m)
{
    static uint8_t last_sub_page = 0xFF;
    if (m->sub_page != last_sub_page)
        last_sub_page = m->sub_page;

    char r0[17], r1[17];
    switch (m->sub_page)
    {
    case MAIN_SUB_OUTPUT:
        snprintf(r0, 17, "OUT:%3.0fV %2.0fHz   ", m->output_voltage, m->output_frequency);
        snprintf(r1, 17, "CUR:%4.1fA %4.0fW ", m->output_current,
                 m->output_voltage * m->output_current);
        break;
    case MAIN_SUB_BATTERY:
        snprintf(r0, 17, "BAT:%4.1fV %3d%%  ", m->battery_voltage, m->battery_pct);
        snprintf(r1, 17, "TMP:%2.0fC CHG:%s ",
                 m->battery_temperature, m->battery_charging ? "YES" : "NO ");
        break;
    case MAIN_SUB_SYSTEM:
        snprintf(r0, 17, "INV:%s AC:%s  ",
                 m->inverter_active ? "ON " : "OFF",
                 m->ac_connected ? "YES" : "NO ");
        snprintf(r1, 17, "LOAD: %3d%%      ", m->load_pct);
        break;
    default:
        snprintf(r0, 17, "%-16s", "Vonix Inverter  ");
        snprintf(r1, 17, "%-16s", "                ");
        break;
    }
    draw_row(0, r0);
    draw_row(1, r1);
}

static void draw_menu(const lcd_two_line_t *d) { draw_commit(d->row0, d->row1); }
static void draw_detail(const lcd_detail_data_t *d) { draw_commit(d->label, d->value_str); }
static void draw_confirm(const lcd_two_line_t *d) { draw_commit(d->row0, d->row1); }

static void draw_value_edit(const lcd_value_edit_data_t *d)
{
    static bool blink_state = false;
    static int64_t last_blink_time_ms = 0;
    draw_commit(d->label, d->value_str);

    int64_t now = esp_timer_get_time() / 1000;
    if (now - last_blink_time_ms > LCD_BLINK_INTERVAL_MS)
    {
        blink_state = !blink_state;
        last_blink_time_ms = now;
    }
    int len = strlen(d->value_str);
    while (len > 0 && d->value_str[len - 1] == ' ')
        len--;
    if (len < 16)
    {
        lcd_set_cursor(len, 1);
        lcd_print_string(blink_state ? "_" : " ");
    }
}

static void draw_startup(const lcd_startup_data_t *d)
{
    char r1[17];
    snprintf(r1, 17, "Progress: %3d%%  ", d->progress_pct);
    draw_commit("STARTING...     ", r1);
}

static void draw_shutdown(const lcd_shutdown_data_t *d)
{
    char r1[17];
    if (d->load_warning)
    {
        snprintf(r1, 17, "Load:%-6.1fA    ", d->load_current);
        draw_commit("** WARNING! **  ", r1);
    }
    else
    {
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

static void draw_factory_reset(const factory_reset_ctx_t *d)
{
    factory_reset_action_t action = atomic_load(&d->action);
    const char *r0, *r1;
    switch (d->phase)
    {
    case FACTORY_PHASE_CONFIRM:
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

    case FACTORY_PHASE_PROGRESS:
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
        lcd_show_loading(r0, 1500, LCD_SCREEN_FACTORY_RESET);
        break;

    case FACTORY_RESET_PIN_ENTRY:
    {
        /* Factory-reset PIN gate: reuses the security PIN-flow renderer.
         * The user is verifying the current PIN before the reset is
         * allowed -- always shown as "Enter PIN:" regardless of which
         * change_pin sub-phase is active internally. */
        if (security_is_locked_out())
        {
            int64_t remaining_ms = security_lockout_remaining_ms();
            uint32_t remaining_s = (remaining_ms > 0)
                                       ? (uint32_t)(remaining_ms / 1000) + 1
                                       : 0;
            char r1_buf[17];
            snprintf(r1_buf, sizeof(r1_buf), "Retry in %3lus   ",
                     (unsigned long)remaining_s);
            draw_commit("PIN Locked      ", r1_buf);
        }
        else
        {
            change_pin_ctx_t pin_snap;
            if (xSemaphoreTake(change_pin_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
            {
                memcpy(&pin_snap, &change_pin_ctx, sizeof(pin_snap));
                xSemaphoreGive(change_pin_mutex);
                char r1_buf[17];
                pin_entry_render_line(&pin_snap.pin_ctx, r1_buf, sizeof(r1_buf));
                draw_commit("Enter PIN:      ", r1_buf);
            }
            /* else: mutex busy this tick, skip frame silently */
        }
        break;
    }

    case FACTORY_PHASE_DONE:
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
        ESP_LOGI("FACTORY_RESET", "Drawing DONE screen");
        if (atomic_load(&sys_lcd.factory_reset.action) == FACTORY_ACTION_RESET_ALL)
            draw_commit(r0, r1);
        else
            lcd_flash_info_to(r0, r1, 2000, LCD_SCREEN_MAIN);
        break;

    case FACTORY_PHASE_IDLE:
    {
        static int last_selection = -1;
        if (atomic_load(&d->action) != FACTORY_ACTION_NONE || last_selection < 0)
        {
            draw_commit("Factory Reset   ", "Select an option");
            atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_CONFIRM);
        }
        break;
    }
    }
}

/*==============================================================================
  Security screen draw helpers
  All draw_security_* functions are self-contained: they build their own
  stack buffers and call draw_commit() themselves. Callers must NOT call
  draw_commit() again after any of these.
==============================================================================*/

/* Submenu item labels — order must match menu_selection indices 0/1/2. */
static const char *const SECURITY_MENU_ITEMS[] = {
    "Change PIN",
    "View Status",
    "Reset PIN",
};
#define SECURITY_MENU_COUNT \
    ((uint8_t)(sizeof(SECURITY_MENU_ITEMS) / sizeof(SECURITY_MENU_ITEMS[0])))

/*------------------------------------------------------------------------------
  draw_security_submenu()
  Scrolling 2-row cursor list for SECURITY_PHASE_IDLE.
  Reads sys_state.menu_selection under a brief sys_state_mutex take (10 ms
  timeout) so we never read a value torn mid-update by the button task.

  Scroll rule: selected item is always visible.
    selection=0 -> top=0  : row0=item[0]>, row1=item[1]
    selection=1 -> top=0  : row0=item[0],  row1=item[1]>
    selection=2 -> top=1  : row0=item[1],  row1=item[2]>

  Layout on the 16-char display:
    ">Change PIN     "
    " View Status    "
------------------------------------------------------------------------------*/
static void draw_security_submenu(void)
{
    uint8_t selection = 0;
    if (xSemaphoreTake(sys_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        selection = (uint8_t)sys_state.menu_selection;
        xSemaphoreGive(sys_state_mutex);
    }

    /* Defensive clamp. */
    if (selection >= SECURITY_MENU_COUNT)
        selection = SECURITY_MENU_COUNT - 1;

    /* Compute top of visible window. */
    uint8_t top = 0;
    if (selection >= 1)
        top = selection - 1;
    if (SECURITY_MENU_COUNT >= 2 && top + 1 >= SECURITY_MENU_COUNT)
        top = SECURITY_MENU_COUNT - 2;

    char r0[17], r1[17];

    snprintf(r0, sizeof(r0), "%c%-15s",
             (top == selection) ? '>' : ' ',
             SECURITY_MENU_ITEMS[top]);

    uint8_t bot = top + 1;
    if (bot < SECURITY_MENU_COUNT)
        snprintf(r1, sizeof(r1), "%c%-15s",
                 (bot == selection) ? '>' : ' ',
                 SECURITY_MENU_ITEMS[bot]);
    else
        snprintf(r1, sizeof(r1), "%-16s", "");

    draw_commit(r0, r1);
}

/*------------------------------------------------------------------------------
  draw_security_lockout()
  Shown whenever security_is_locked_out() is true during SECURITY_PHASE_PIN_FLOW.
  Displays a live countdown so the user isn't left staring at a frozen
  digit-entry screen with no explanation.
------------------------------------------------------------------------------*/
static void draw_security_lockout(void)
{
    int64_t remaining_ms = security_lockout_remaining_ms();
    uint32_t remaining_s = (remaining_ms > 0)
                               ? (uint32_t)(remaining_ms / 1000) + 1
                               : 0;
    char r1[17];
    snprintf(r1, sizeof(r1), "Retry in %3lus   ", (unsigned long)remaining_s);
    draw_commit("PIN Locked      ", r1);
}

/*------------------------------------------------------------------------------
  draw_security_status()
  Shown for SECURITY_PHASE_VIEW_STATUS.
  Three possible states, in priority order:
    1. Locked out   -> show countdown
    2. Default PIN  -> warn user to change it
    3. Custom PIN   -> confirm all good
------------------------------------------------------------------------------*/
static void draw_security_status()
{
    char line1[17], line2[17];
    if (security_pin_change_required())
    {
        snprintf(line1, 17, "PIN Status:");
        snprintf(line2, 17, "Default (0000)");
    }
    else if (security_is_locked_out())
    {
        uint32_t remaining_s = security_lockout_remaining_ms() / 1000;
        snprintf(line1, 17, "PIN Status:");
        snprintf(line2, 17, "Locked %luds", remaining_s);
    }
    else
    {
        snprintf(line1, 17, "PIN Status:");
        snprintf(line2, 17, "Custom, OK");
    }
    draw_commit(line1, line2);
}

/*------------------------------------------------------------------------------
  draw_security_pin_flow()
  Renders digit-entry for Change-PIN and Reset-PIN flows.

  Row 0: context label driven by change_pin_phase_t so the user always
         knows which sub-step they're on (Old / New / Confirm).
  Row 1: digit glyphs from pin_entry_render_line():
         - Active digit: live (0-9)
         - Confirmed digit: '*' (masked)
         - Pending digit: '_' (placeholder)

  Internally:
  1. If locked out, delegates to draw_security_lockout() immediately --
     no need to touch change_pin_ctx at all.
  2. Takes change_pin_mutex with a 20ms timeout to snapshot change_pin_ctx.
     If the mutex is busy (button task mid-update), skips this frame
     rather than blocking lcd_task or rendering a torn struct.

  Returns true if a frame was drawn, false if the frame was skipped.
------------------------------------------------------------------------------*/
static bool draw_security_pin_flow(change_pin_phase_t flow_phase)
{
    if (security_is_locked_out())
    {
        draw_security_lockout();
        return true;
    }

    change_pin_ctx_t pin_snap;
    if (xSemaphoreTake(change_pin_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    {
        /* Button task is holding the mutex this tick -- safe to skip. */
        ESP_LOGD(TAG, "Security: change_pin_mutex busy, skipping frame");
        return false;
    }
    memcpy(&pin_snap, &change_pin_ctx, sizeof(pin_snap));
    xSemaphoreGive(change_pin_mutex);

    char r0[17], r1[17];

    switch (flow_phase)
    {
    case CHANGE_PIN_VERIFY_OLD:
        snprintf(r0, sizeof(r0), "%-16s", "Enter Old PIN:  ");
        break;
    case CHANGE_PIN_ENTER_NEW:
        snprintf(r0, sizeof(r0), "%-16s", "Enter New PIN:  ");
        break;
    case CHANGE_PIN_CONFIRM_NEW:
        snprintf(r0, sizeof(r0), "%-16s", "Confirm PIN:    ");
        break;
    default:
        snprintf(r0, sizeof(r0), "%-16s", "Enter PIN:      ");
        break;
    }

    pin_entry_render_line(&pin_snap.pin_ctx, r1, sizeof(r1));
    draw_commit(r0, r1);
    return true;
}

/*------------------------------------------------------------------------------
  draw_security()
  Top-level dispatcher for LCD_SCREEN_SECURITY.
  Switches on security_phase_t, which is _Atomic and safe to read from
  snap (the mutex-protected copy taken at the top of each lcd_task tick).
------------------------------------------------------------------------------*/
static void draw_security(const lcd_security_data_t *d)
{
    security_phase_t sec_phase = atomic_load(&d->phase);
    security_action_t sec_action = atomic_load(&d->action);

    switch (sec_phase)
    {
    /* ── Idle: browsing the submenu list ─────────────────────────────── */
    case SECURITY_PHASE_IDLE:
        draw_security_submenu();
        break;

    /* ── PIN flow: Change PIN or Reset PIN ───────────────────────────── */
    case SECURITY_PHASE_PIN_FLOW:
    {
        /* Need change_pin_ctx.phase for the row-0 label.
         * Take the mutex briefly just to read one field. */
        change_pin_phase_t flow_phase = CHANGE_PIN_VERIFY_OLD;
        if (xSemaphoreTake(change_pin_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            flow_phase = change_pin_ctx.phase;
            xSemaphoreGive(change_pin_mutex);
        }
        /* draw_security_pin_flow() takes its own full snapshot internally. */
        draw_security_pin_flow(flow_phase);
        break;
    }

    /* ── View status: PIN state readout ─────────────────────────────── */
    case SECURITY_PHASE_VIEW_STATUS:
        draw_security_status();
        break;

    /* ── Unknown phase: defensive reset ─────────────────────────────── */
    default:
        ESP_LOGW(TAG, "Unknown security phase %d -- resetting to IDLE", sec_phase);
        xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
        atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_IDLE);
        atomic_store(&sys_lcd.security.action, SECURITY_ACTION_NONE);
        xSemaphoreGive(sys_state_mutex);
        break;
    }

    (void)sec_action; /* available for logging/future use */
}

/*==============================================================================
  LCD reinit helper — called when integrity check fails
==============================================================================*/
void lcd_task_reinit(lcd_screen_id_t *last_screen)
{
    /* Save important state before wipe. */
    uint8_t saved_sub_page = sys_lcd.main.sub_page;
    uint32_t saved_interval = sys_lcd.main.sub_page_interval_ms;
    security_phase_t saved_sec_phase = atomic_load(&sys_lcd.security.phase);
    security_action_t saved_sec_action = atomic_load(&sys_lcd.security.action);

    memset(&sys_lcd, 0, sizeof(sys_lcd));

    /* Restore main-screen cycling state. */
    sys_lcd.main.sub_page = saved_sub_page;
    sys_lcd.main.sub_page_interval_ms = saved_interval;

    /* Restore or cleanly abort security state.
     * If a PIN flow was in progress when corruption occurred, aborting it
     * cleanly is safer than trying to resume with potentially torn ctx. */
    if (saved_sec_phase == SECURITY_PHASE_PIN_FLOW)
    {
        atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_IDLE);
        atomic_store(&sys_lcd.security.action, SECURITY_ACTION_NONE);
        xSemaphoreTake(change_pin_mutex, portMAX_DELAY);
        memset(&change_pin_ctx, 0, sizeof(change_pin_ctx));
        xSemaphoreGive(change_pin_mutex);
        ESP_LOGW("LCD_REINIT",
                 "Security PIN flow aborted: LCD corruption recovery");
    }
    else
    {
        atomic_store(&sys_lcd.security.phase, saved_sec_phase);
        atomic_store(&sys_lcd.security.action, saved_sec_action);
    }

    /* Reinitialize LCD hardware and force full redraw. */
    lcd_init(LCD_ADDR, SDA_PIN, SCL_PIN);
    *last_screen = LCD_SCREEN_COUNT;

    /* Reset dirty-check buffers so draw_commit re-writes every character. */
    memset(s_last_row0, ' ', 16);
    s_last_row0[16] = '\0';
    memset(s_last_row1, ' ', 16);
    s_last_row1[16] = '\0';

    ESP_LOGI("LCD_REINIT", "LCD reinitialized, sub_page=%d sec_phase=%d",
             sys_lcd.main.sub_page, (int)atomic_load(&sys_lcd.security.phase));
}

/*==============================================================================
  HELPER FUNCTIONS
==============================================================================*/
static uint32_t _lcd_get_time_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

extern flash_entry_t s_queue[LCD_FLASH_QUEUE_DEPTH];

/*==============================================================================
  lcd_task — THE ONLY FUNCTION THAT CALLS lcd_* HARDWARE

  ARCHITECTURE:
  - Step 1:  Watchdog feed (safety first)
  - Step 2:  Snapshot LCD state under sys_state_mutex
  - Step 3:  Check LCD integrity (repair if corrupted)
  - Step 4:  Flash message expiry + dequeue
  - Step 5:  Sub-page cycling (main screen only)
  - Step 6:  Screen-change detection + lcd_clear()
  - Step 7:  Draw current screen
  - Step 8:  Task delay / yield
==============================================================================*/
void lcd_task(void *arg)
{
    lcd_render_state_t snap;
    static lcd_screen_id_t last_screen = LCD_SCREEN_COUNT;
    bool need_clear = true;

    static uint32_t main_page_last_change_ms = 0;
    const uint32_t MAIN_PAGE_INTERVAL_MS = 2000;

    sys_lcd.main.sub_page = MAIN_SUB_OUTPUT;
    sys_lcd.main.sub_page_interval_ms = 5000;

    lcd_integrity_init();

    memset(s_last_row0, ' ', 16);
    s_last_row0[16] = '\0';
    memset(s_last_row1, ' ', 16);
    s_last_row1[16] = '\0';

    ESP_LOGI(TAG, "lcd_task started");
    lcd_flash_init(xTaskGetCurrentTaskHandle());

    while (1)
    {
        /* ====== STEP 1: WATCHDOG ====== */
        lcd_watchdog_feed();

        /* ====== STEP 2: SNAPSHOT STATE ====== */
        xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
        memcpy(&snap, &sys_lcd, sizeof(snap));
        diag_data.uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        xSemaphoreGive(sys_state_mutex);

        /* ====== STEP 4: FLASH EXPIRY ====== */
        if (lcd_flash_is_expired())
        {
            ESP_LOGI(TAG, "Flash expired");
            lcd_screen_id_t fallback = lcd_flash_clear_and_get_return();
            if (lcd_flash_queue_has_pending())
            {
                flash_entry_t next;
                if (lcd_flash_dequeue(&next))
                    lcd_flash_enqueue_to(next.line0, next.line1,
                                         next.duration_ms, next.priority,
                                         next.return_to);
            }
            else
            {
                xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
                sys_lcd.screen = fallback;
                xSemaphoreGive(sys_state_mutex);
            }
        }
        if (lcd_flash_is_active())
            snap.screen = LCD_SCREEN_FLASH_MSG;

        /* ====== STEP 5: SUB-PAGE CYCLING ====== */
        if (snap.screen == LCD_SCREEN_MAIN && !lcd_flash_is_active())
        {
            uint32_t now = _lcd_get_time_ms();
            if (main_page_last_change_ms == 0)
            {
                main_page_last_change_ms = now;
            }
            else if (now - main_page_last_change_ms >= MAIN_PAGE_INTERVAL_MS)
            {
                xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
                snap.main.sub_page = (snap.main.sub_page + 1) % MAIN_SUB_COUNT;
                sys_lcd.main.sub_page = snap.main.sub_page;
                xSemaphoreGive(sys_state_mutex);
                main_page_last_change_ms = now;
            }
        }

        /* ====== STEP 6: SCREEN CHANGE DETECTION ====== */
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
        {
            static char prev_flash_line0[17] = {0};
            static char prev_flash_line1[17] = {0};
            active_flash_t flash;
            if (lcd_flash_get(&flash))
            {
                bool changed = (strcmp(prev_flash_line0, flash.line0) != 0) ||
                               (strcmp(prev_flash_line1, flash.line1) != 0);
                if (changed)
                {
                    strncpy(prev_flash_line0, flash.line0,
                            sizeof(prev_flash_line0) - 1);
                    prev_flash_line0[sizeof(prev_flash_line0) - 1] = '\0';
                    strncpy(prev_flash_line1, flash.line1,
                            sizeof(prev_flash_line1) - 1);
                    prev_flash_line1[sizeof(prev_flash_line1) - 1] = '\0';
                    ESP_LOGI(TAG, "Flash updated: '%s' / '%s'",
                             flash.line0, flash.line1);
                }
                draw_commit(flash.line0, flash.line1);
            }
            break;
        }

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

        /* ================================================================
           LCD_SCREEN_SECURITY
           Delegates entirely to draw_security(), which dispatches on
           snap.security.phase via a dedicated sub-switch.
           All three phases have their own draw helper:
             IDLE        -> draw_security_submenu()   scrolling cursor list
             PIN_FLOW    -> draw_security_pin_flow()  digit entry / lockout
             VIEW_STATUS -> draw_security_status()    PIN state readout
           ================================================================ */
        case LCD_SCREEN_SECURITY:
            draw_security(&snap.security);
            break;

        default:
            ESP_LOGW(TAG, "Unknown screen: %d", snap.screen);
            break;
        }

#ifdef LCD_DEBUG_TIMING
        uint32_t draw_end_ms = _lcd_get_time_ms();
        uint32_t draw_time = draw_end_ms - draw_start_ms;
        if (draw_time > 50)
            ESP_LOGW(TAG, "Slow draw: %lu ms for screen %d",
                     draw_time, snap.screen);
#endif

        /* ====== STEP 8: YIELD ====== */
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Notification-based wakeup path (unreachable in normal operation
     * but kept so the task can be signalled immediately on flash enqueue). */
    uint32_t notify_value = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    if (notify_value > 0)
        ESP_LOGD(TAG, "Woken by flash enqueue notification");
}