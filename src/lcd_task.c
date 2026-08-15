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
#include "utility/led.h"
#include "events/event_dispatcher.h"

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
extern led_pattern_t pattern;

/* lcd.h hardware config */
#define LCD_ADDR 0x27
#define SDA_PIN 21
#define SCL_PIN 22
#define LCD_BLINK_INTERVAL_MS 500
#define LCD_FLASH_QUEUE_DEPTH 4
#define SYSTEM_STARTUP_DISPLAY_DURATION_MS 1500U

static uint8_t loading_progress(uint32_t elapsed, uint32_t duration);

static const char *TAG = "LCD_TASK";

#define now_ms() (xTaskGetTickCount() * portTICK_PERIOD_MS)

/*==============================================================================
  Draw helpers — ONLY functions that call lcd_* hardware directly.
==============================================================================*/
static char s_last_row0[LCD_LINE_SIZE];
static char s_last_row1[LCD_LINE_SIZE];
static char s_last_row2[LCD_LINE_SIZE];
static char s_last_row3[LCD_LINE_SIZE];

static void reset_row_cache(void)
{
    memset(s_last_row0, 0, sizeof(s_last_row0));
    memset(s_last_row1, 0, sizeof(s_last_row1));
    memset(s_last_row2, 0, sizeof(s_last_row2));
    memset(s_last_row3, 0, sizeof(s_last_row3));
}

static void draw_commit_rows(const char *const rows[])
{
    char *cached[] = {
        s_last_row0,
        s_last_row1,
        s_last_row2,
        s_last_row3,
    };

    uint8_t active_rows = lcd_geometry_rows();
    uint8_t active_cols = lcd_geometry_cols();
    for (uint8_t row = 0; row < active_rows; row++)
    {
        char line[LCD_LINE_SIZE];
        snprintf(line, sizeof(line), "%-*.*s", active_cols, active_cols,
                 rows[row] ? rows[row] : "");
        if (strcmp(line, cached[row]) != 0)
        {
            lcd_set_cursor(0, row);
            lcd_print(line);
            strcpy(cached[row], line);
        }
    }

    /* Integrity read-back currently covers the first two DDRAM rows. */
    lcd_integrity_snapshot(s_last_row0, s_last_row1);
}

static void draw_commit(const char *r0, const char *r1)
{
    const char *rows[] = {
        r0,
        r1,
        "",
        "",
    };
    draw_commit_rows(rows);
}

static void draw_row(uint8_t row, const char *text)
{
    if (row >= lcd_geometry_rows())
        return;
    const char *rows[] = {
        s_last_row0,
        s_last_row1,
        s_last_row2,
        s_last_row3,
    };
    lcd_set_cursor(0, row);
    char line[LCD_LINE_SIZE];
    uint8_t active_cols = lcd_geometry_cols();
    snprintf(line, sizeof(line), "%-*.*s", active_cols, active_cols,
             text ? text : "");
    lcd_print(line);
    strcpy((char *)rows[row], line);
    lcd_integrity_snapshot(s_last_row0, s_last_row1);
}

static void display_voltage_parts(float value, uint8_t *whole, uint8_t *tenths)
{
    if (!isfinite(value) || value < 0.0f)
        value = 0.0f;
    if (value > 99.9f)
        value = 99.9f;
    uint16_t scaled = (uint16_t)(value * 10.0f + 0.5f);
    *whole = (uint8_t)(scaled / 10U);
    *tenths = (uint8_t)(scaled % 10U);
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

void lcd_display_startup_screen(uint8_t progress)
{
    (void)progress;
    /* Startup branding is intentionally disabled. The startup screen is
     * reserved for progress/status information, not a splash logo. */
    if (lcd_geometry_is_20x4())
        draw_commit_rows((const char *[]){"", "", "", ""});
    else
        draw_commit("", "");
}

static void format_progress_line(char *out, uint8_t pct)
{
    if (pct > 100)
        pct = 100;
    char pct_str[5];
    snprintf(pct_str, sizeof(pct_str), "%3u%%", pct);
    int bar_slots = lcd_geometry_cols() - 2 - 4;
    uint8_t filled = (pct * bar_slots) / 100;
    out[0] = '[';
    for (int i = 0; i < bar_slots; i++)
        out[i + 1] = (i < filled) ? '#' : '-';
    out[bar_slots + 1] = ']';
    out[bar_slots + 2] = ' ';
    memcpy(&out[bar_slots + 3], pct_str, 4);
    out[lcd_geometry_cols()] = '\0';
}

static const char *home_mode_label(uint8_t mode)
{
    switch (mode)
    {
    case 0:
        return "SOLAR PRIORITY";
    case 1:
        return "AC PRIORITY";
    default:
        return "AUTO PRIORITY";
    }
}

static void format_home_wifi(char *out, size_t out_len,
                             bool connected, int8_t rssi)
{
    if (connected)
        snprintf(out, out_len, "W%s", rssi_bars(rssi));
    else
        snprintf(out, out_len, "W-");
}

static void format_battery_time(char *out, size_t out_len,
                                uint16_t remaining_minutes)
{
    if (remaining_minutes == 0U)
    {
        /* Runtime is load-dependent. Make the reason visible instead of
         * presenting an apparently missing value while the inverter is idle. */
        snprintf(out, out_len, "IDLE");
        return;
    }
    const unsigned hours = remaining_minutes / 60U;
    const unsigned minutes = remaining_minutes % 60U;
    snprintf(out, out_len, "%02u:%02u", hours > 99U ? 99U : hours, minutes);
}

static void draw_main(lcd_main_data_t *m)
{
    char wifi[8];
    format_home_wifi(wifi, sizeof(wifi), m->wifi_connected, m->wifi_rssi);

    if (lcd_geometry_is_20x4())
    {
        char rows[4][LCD_LINE_SIZE];
        const char *mode = home_mode_label(m->operating_mode);
        const uint8_t voltage = m->voltage_system ? m->voltage_system : 12U;
        const unsigned ac_voltage = (m->ac_voltage < 0.0f)
                                        ? 0U
                                        : (m->ac_voltage > 999.0f
                                               ? 999U
                                               : (unsigned)m->ac_voltage);
        const char *state = m->inverter_active ? "ON" : "OFF";

        /* Stable dashboard: values are updated in place; no timed rotation. */
        if (m->sub_page == MAIN_SUB_BATTERY)
        {
            char remaining[12];
            format_battery_time(remaining, sizeof(remaining),
                                m->battery_remaining_minutes);
            snprintf(rows[0], LCD_LINE_SIZE, "BAT:%4.1fV %3u%% %.5s",
                     m->battery_voltage, (unsigned)m->battery_pct, wifi);
            snprintf(rows[1], LCD_LINE_SIZE, "TIME REMAINING %.5s", remaining);
            snprintf(rows[2], LCD_LINE_SIZE, "AC:%3uV SYS:%s",
                     ac_voltage, state);
            snprintf(rows[3], LCD_LINE_SIZE, "%-14.14s %2uV",
                     mode, (unsigned)voltage);
        }
        else if (m->sub_page == MAIN_SUB_SYSTEM)
        {
            snprintf(rows[0], LCD_LINE_SIZE, "AC%3uV S%s %.5s",
                     ac_voltage, state, wifi);
            snprintf(rows[1], LCD_LINE_SIZE, "BAT:%4.1fV %3u%%",
                     m->battery_voltage, (unsigned)m->battery_pct);
            snprintf(rows[2], LCD_LINE_SIZE, "TIME REMAINING");
            char remaining[12];
            format_battery_time(remaining, sizeof(remaining),
                                m->battery_remaining_minutes);
            snprintf(rows[3], LCD_LINE_SIZE, "%-14.14s %.5s", mode, remaining);
        }
        else
        {
            snprintf(rows[0], LCD_LINE_SIZE, "INV 5.0kW %-3s %.5s", state, wifi);
            snprintf(rows[1], LCD_LINE_SIZE, "PV:%4.2fkW BAT:%3u%%",
                     m->pv_power_kw, (unsigned)m->battery_pct);
            snprintf(rows[2], LCD_LINE_SIZE, "LD:%4.2fkW GR:%4.2fkW",
                     m->load_power_kw, m->grid_power_kw);
            /* Keep battery telemetry on the default home page. The voltage
             * is already scaled by the selected 12/24/48 V system before it
             * reaches this renderer. */
            char remaining[12];
            format_battery_time(remaining, sizeof(remaining),
                                m->battery_remaining_minutes);
            snprintf(rows[3], LCD_LINE_SIZE, "BAT:%4.1fV T:%5.5s",
                     m->battery_voltage, remaining);
        }
        const char *row_ptrs[] = {rows[0], rows[1], rows[2], rows[3]};
        draw_commit_rows(row_ptrs);
    }
    else
    {
        /* Preserve the established compact 16×2 presentation. The shared
         * settings, menu selection, persistence, and Enter-only page control
         * remain identical to the 20×4 build; only the rendering is compact. */
        char r0[LCD_LINE_SIZE], r1[LCD_LINE_SIZE];
        switch (m->sub_page)
        {
        case MAIN_SUB_OUTPUT:
            snprintf(r0, LCD_LINE_SIZE, "OUT:%3.0fV %2.0fHz   ",
                     m->output_voltage, m->output_frequency);
            snprintf(r1, LCD_LINE_SIZE, "CUR:%4.1fA %4.0fW ",
                     m->output_current, m->output_voltage * m->output_current);
            break;
        case MAIN_SUB_BATTERY:
        {
            char remaining[12];
            format_battery_time(remaining, sizeof(remaining),
                                m->battery_remaining_minutes);
            snprintf(r0, LCD_LINE_SIZE, "BAT:%4.1fV %3d%%  ",
                     m->battery_voltage, m->battery_pct);
            snprintf(r1, LCD_LINE_SIZE, "REM:%5.5s TMP:%2.0f",
                     remaining, m->battery_temperature);
            break;
        }
        case MAIN_SUB_SYSTEM:
        default:
            snprintf(r0, LCD_LINE_SIZE, "INV:%s AC:%s  ",
                     m->inverter_active ? "ON " : "OFF",
                     m->ac_connected ? "YES" : "NO ");
            snprintf(r1, LCD_LINE_SIZE, "LOAD: %3d%% %-.5s",
                     m->load_pct, wifi);
            break;
        }
        draw_commit(r0, r1);
    }
}

static void draw_menu(const lcd_menu_data_t *d)
{
    if (lcd_geometry_is_20x4())
    {
        const char *rows[LCD_ROWS];
        for (uint8_t row = 0; row < LCD_ROWS; ++row)
            rows[row] = d->rows[row];
        draw_commit_rows(rows);
    }
    else
    {
        draw_commit(d->rows[0], d->rows[1]);
    }
}
static void draw_detail(const lcd_detail_data_t *d)
{
    if (lcd_geometry_is_20x4())
    {
        const char *rows[] = {d->label, d->value_str,
                              "UP/DN  More", "BACK    Return"};
        draw_commit_rows(rows);
    }
    else
    {
        draw_commit(d->label, d->value_str);
    }
}
static void draw_confirm(const lcd_two_line_t *d)
{
    if (lcd_geometry_is_20x4())
    {
        const char *rows[] = {d->row0, d->row1,
                              "ENTER   Confirm", "BACK    Cancel"};
        draw_commit_rows(rows);
    }
    else
    {
        draw_commit(d->row0, d->row1);
    }
}

static void draw_value_edit(const lcd_value_edit_data_t *d)
{
    static bool blink_state = false;
    static int64_t last_blink_time_ms = 0;
    if (lcd_geometry_is_20x4())
    {
        const char *rows[] = {d->label, d->value_str,
                              "UP/DN   Change", "ENTER   Save  BACK"};
        draw_commit_rows(rows);
    }
    else
    {
        draw_commit(d->label, d->value_str);
    }

    int64_t now = esp_timer_get_time() / 1000;
    if (now - last_blink_time_ms > LCD_BLINK_INTERVAL_MS)
    {
        blink_state = !blink_state;
        last_blink_time_ms = now;
    }
    int len = strlen(d->value_str);
    while (len > 0 && d->value_str[len - 1] == ' ')
        len--;
    if (len < lcd_geometry_cols())
    {
        lcd_set_cursor(len, 1);
        lcd_print_string(blink_state ? "_" : " ");
    }
}

static const char *startup_stage_label(uint8_t pct);

static void draw_startup(const lcd_startup_data_t *d)
{
    char bar[LCD_LINE_SIZE];
    format_progress_line(bar, d->progress_pct);
    if (lcd_geometry_is_20x4())
    {
        const char *rows[] = {"OUTPUT STARTING", "", bar, ""};
        draw_commit_rows(rows);
    }
    else
    {
        draw_commit("OUTPUT STARTING", bar);
    }
}

static void draw_shutdown(const lcd_shutdown_data_t *d)
{
    if (lcd_geometry_is_20x4())
    {
        char rows[4][LCD_LINE_SIZE];
        uint8_t load_a = 0, load_t = 0;
        display_voltage_parts(d->load_current, &load_a, &load_t);
        snprintf(rows[0], LCD_LINE_SIZE, "SYSTEM SHUTDOWN");
        snprintf(rows[1], LCD_LINE_SIZE, "%s", d->load_warning ? "LOAD WARNING" : "RAMPING DOWN");
        snprintf(rows[2], LCD_LINE_SIZE, "POWER LEVEL %3u%%", (unsigned)d->progress_pct);
        if (d->load_warning)
            snprintf(rows[3], LCD_LINE_SIZE, "LOAD %2u.%uA HOLD", (unsigned)load_a, (unsigned)load_t);
        else
            snprintf(rows[3], LCD_LINE_SIZE, "SAFE TO POWER OFF");
        const char *row_ptrs[] = {rows[0], rows[1], rows[2], rows[3]};
        draw_commit_rows(row_ptrs);
    }
    else
    {
        char r1[LCD_LINE_SIZE];
        if (d->load_warning)
        {
            snprintf(r1, LCD_LINE_SIZE, "Load:%-6.1fA    ", d->load_current);
            draw_commit("** WARNING! **  ", r1);
        }
        else
        {
            snprintf(r1, LCD_LINE_SIZE, "Power: %3d%%     ", d->progress_pct);
            draw_commit("RAMP DOWN       ", r1);
        }
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
    {
        if (lcd_geometry_is_20x4())
        {
            const char *rows[] = {"!!! SYSTEM FAULT !!!", d->line0,
                                  d->line1, "OUTPUT DISABLED"};
            draw_commit_rows(rows);
        }
        else
        {
            draw_commit(d->line0, d->line1);
        }
    }
    else
    {
        if (lcd_geometry_is_20x4())
        {
            const char *rows[] = {"                    ", "                    ",
                                  "                    ", "                    "};
            draw_commit_rows(rows);
        }
        else
        {
            draw_commit("                ", "                ");
        }
    }
}

static void draw_standby(const lcd_standby_data_t *d)
{
    uint8_t page = d->page < LCD_STANDBY_PAGE_COUNT
                       ? d->page
                       : LCD_STANDBY_PAGE_STATUS;

    char wifi[8];
    format_home_wifi(wifi, sizeof(wifi), d->wifi_connected, d->wifi_rssi);

    if (lcd_geometry_is_20x4())
    {
        char rows[4][LCD_LINE_SIZE];
        uint8_t bat_v = 0, bat_t = 0;
        display_voltage_parts(d->battery_voltage, &bat_v, &bat_t);

        if (page == LCD_STANDBY_PAGE_BATTERY)
        {
            snprintf(rows[0], LCD_LINE_SIZE, "BATTERY %3u%% %.5s",
                     (unsigned)d->battery_pct, wifi);
            snprintf(rows[1], LCD_LINE_SIZE, "VOLTAGE %2u.%uV", bat_v, bat_t);
            snprintf(rows[2], LCD_LINE_SIZE, "SOC     %3u%% %s",
                     d->battery_pct,
                     d->battery_voltage < d->low_voltage_threshold ? "LOW" : "OK");
            snprintf(rows[3], LCD_LINE_SIZE, "ENTER:NEXT PWR:START");
        }
        else
        {
            snprintf(rows[0], LCD_LINE_SIZE, "STBY %.5s AC:%s", wifi,
                     d->ac_connected ? "ON" : "OFF");
            snprintf(rows[1], LCD_LINE_SIZE, "BAT %2u.%uV %3u%%",
                     (unsigned)bat_v, (unsigned)bat_t,
                     (unsigned)d->battery_pct);
            snprintf(rows[2], LCD_LINE_SIZE, "%s",
                     d->battery_voltage < d->low_voltage_threshold
                         ? "LOW BATTERY WARNING"
                         : "BATTERY MONITOR OK");
            snprintf(rows[3], LCD_LINE_SIZE, "ENTER:NEXT PWR:START");
        }
        const char *row_ptrs[] = {rows[0], rows[1], rows[2], rows[3]};
        draw_commit_rows(row_ptrs);
    }
    else
    {
        char r0[LCD_LINE_SIZE], r1[LCD_LINE_SIZE];
        if (page == LCD_STANDBY_PAGE_BATTERY)
        {
            snprintf(r0, LCD_LINE_SIZE, "BAT%3u%% %.5s", d->battery_pct, wifi);
            snprintf(r1, LCD_LINE_SIZE, "ENTER>NEXT PWR");
        }
        else if (d->battery_voltage < d->low_voltage_threshold)
        {
            snprintf(r0, LCD_LINE_SIZE, "STBY %.5s A%s", wifi,
                     d->ac_connected ? "ON" : "OFF");
            snprintf(r1, LCD_LINE_SIZE, "LOW BAT:%4.1fV", d->battery_voltage);
        }
        else
        {
            snprintf(r0, LCD_LINE_SIZE, "STBY %.5s A%s", wifi,
                     d->ac_connected ? "ON" : "OFF");
            snprintf(r1, LCD_LINE_SIZE, "BATTERY %3u%%", d->battery_pct);
        }
        draw_commit(r0, r1);
    }
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

static void format_loading_bar(char *row, size_t row_len, uint8_t pct)
{
    const uint8_t slots = lcd_geometry_is_20x4() ? 14U : 10U;
    const uint8_t blocks = (uint8_t)((pct * slots) / 100U);
    memset(row, ' ', row_len);
    row[0] = '[';
    for (uint8_t i = 0; i < slots; i++)
        row[i + 1] = (i < blocks) ? '#' : '-';
    row[slots + 1] = ']';
    row[lcd_geometry_cols()] = '\0';
}

static const char *startup_stage_label(uint8_t pct)
{
    if (pct < 30U)
        return "WAKING SYSTEM";
    if (pct < 60U)
        return "CHECKING HARDWARE";
    if (pct < 85U)
        return "PREPARING OUTPUT";
    return "READY TO DELIVER";
}

static void draw_loading(const lcd_loading_data_t *d)
{
    uint32_t elapsed = _lcd_get_time_ms() - d->start_ms;
    uint8_t pct = loading_progress(elapsed, d->duration_ms);
    char bar[LCD_LINE_SIZE];
    format_loading_bar(bar, sizeof(bar), pct);

    if (lcd_geometry_is_20x4())
    {
        char status[LCD_LINE_SIZE];
        snprintf(status, sizeof(status), "  %s", startup_stage_label(pct));
        char progress[LCD_LINE_SIZE];
        snprintf(progress, sizeof(progress), "SYSTEM STARTING %3u%%", pct);
        const char *rows[] = {progress, bar, status, ""};
        draw_commit_rows(rows);
    }
    else
    {
        char row0[LCD_LINE_SIZE];
        snprintf(row0, sizeof(row0), "STARTING %3u%%", pct);
        draw_commit(row0, bar);
    }
}

static void draw_wifi_scan(const lcd_wifi_scan_data_t *d)
{
    if (d->count == 0)
    {
        if (lcd_geometry_is_20x4())
        {
            const char *rows[] = {"WI-FI SCAN", "NO NETWORKS FOUND",
                                  "CHECK ROUTER", "BACK TO RETURN"};
            draw_commit_rows(rows);
        }
        else
        {
            draw_commit("No Networks     ", "Found           ");
        }
        return;
    }
    if (lcd_geometry_is_20x4())
    {
        char rows[4][LCD_LINE_SIZE];
        for (uint8_t line = 0; line < 4; line++)
        {
            uint8_t idx = d->top_index + line;
            if (idx < d->count)
                snprintf(rows[line], LCD_LINE_SIZE, "%c%-12s %4s",
                         (idx == d->selected_index) ? '>' : ' ',
                         d->ssid[idx], rssi_bars(d->rssi[idx]));
            else
            {
                memset(rows[line], ' ', LCD_COLS);
                rows[line][LCD_COLS] = '\0';
            }
        }
        const char *row_ptrs[] = {rows[0], rows[1], rows[2], rows[3]};
        draw_commit_rows(row_ptrs);
    }
    else
    {
        for (uint8_t line = 0; line < 2; line++)
        {
            uint8_t idx = d->top_index + line;
            if (idx >= d->count)
                break;
            char row[LCD_LINE_SIZE];
            snprintf(row, LCD_LINE_SIZE, "%c%-8s %4s  ",
                     (idx == d->selected_index) ? '>' : ' ',
                     d->ssid[idx], rssi_bars(d->rssi[idx]));
            draw_row(line, row);
        }
    }
}

static void draw_wifi_connecting(const lcd_wifi_connect_data_t *d)
{
    if (lcd_geometry_is_20x4())
    {
        static uint8_t frame = 0;
        frame = (uint8_t)((frame + 1) % 4);
        if (d->connected)
        {
            const char *rows[] = {"WI-FI CONNECTED", d->ssid,
                                  "LINK ESTABLISHED", "READY FOR DATA"};
            draw_commit_rows(rows);
        }
        else if (d->failed)
        {
            const char *rows[] = {"WI-FI FAILED", d->ssid,
                                  "CHECK CREDENTIALS", "BACK TO RETURN"};
            draw_commit_rows(rows);
        }
        else if (d->timed_out)
        {
            const char *rows[] = {"WI-FI TIMEOUT", d->ssid,
                                  "NO RESPONSE", "BACK TO RETURN"};
            draw_commit_rows(rows);
        }
        else
        {
            const char *activity[] = {"CONNECTING  ", "CONNECTING .", "CONNECTING ..", "CONNECTING ..."};
            char rows[4][LCD_LINE_SIZE];
            snprintf(rows[0], LCD_LINE_SIZE, "%s", activity[frame]);
            snprintf(rows[1], LCD_LINE_SIZE, "%c %-18.18s", CHAR_WIFI_TX, d->ssid);
            snprintf(rows[2], LCD_LINE_SIZE, "%c WAITING FOR AP", CHAR_WIFI_RX);
            snprintf(rows[3], LCD_LINE_SIZE, "%c SIGNAL / AUTH", CHAR_WIFI_LINK);
            const char *row_ptrs[] = {rows[0], rows[1], rows[2], rows[3]};
            draw_commit_rows(row_ptrs);
        }
    }
    else
    {
        char r0[LCD_LINE_SIZE], r1[LCD_LINE_SIZE];
        if (d->connected)
        {
            snprintf(r0, LCD_LINE_SIZE, "%-16s", "Wi-Fi Connected ");
            snprintf(r1, LCD_LINE_SIZE, "%-16.16s", d->ssid);
        }
        else if (d->failed)
        {
            snprintf(r0, LCD_LINE_SIZE, "%-16s", "Wi-Fi Failed    ");
            snprintf(r1, LCD_LINE_SIZE, "%-16s", "Check SSID/Pass ");
        }
        else if (d->timed_out)
        {
            snprintf(r0, LCD_LINE_SIZE, "%-16s", "Wi-Fi Timeout   ");
            snprintf(r1, LCD_LINE_SIZE, "%-16s", "No Connection   ");
        }
        else
        {
            snprintf(r0, LCD_LINE_SIZE, "%-16s", "Connecting Wi-Fi");
            snprintf(r1, LCD_LINE_SIZE, "%-16.16s", d->ssid);
        }
        draw_commit(r0, r1);
    }
}

static void format_empty_pin_slots(char *line, size_t line_size)
{
    pin_entry_ctx_t empty_ctx = {0};
    empty_ctx.cursor = SECURITY_PIN_LEN;
    pin_entry_render_line(&empty_ctx, line, line_size);
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
        const bool locked = factory_reset_is_locked_out(&sys_lcd.factory_reset);
        const bool pin_reset_pending =
            factory_reset_pin_reset_pending(&sys_lcd.factory_reset);
        const uint8_t attempts_left =
            security_attempts_remaining_for_scope(SECURITY_LOCKOUT_FACTORY_RESET);

        if (lcd_geometry_is_20x4())
        {
            char pin_line[LCD_LINE_SIZE];
            char attempt_line[LCD_LINE_SIZE];
            if (pin_reset_pending)
                format_empty_pin_slots(pin_line, sizeof(pin_line));
            else
                pin_entry_render_line(&d->pin_ctx, pin_line, sizeof(pin_line));

            if (locked)
            {
                const int64_t remaining_ms =
                    security_lockout_remaining_ms_for_scope(
                        SECURITY_LOCKOUT_FACTORY_RESET);
                const uint32_t remaining_s = (remaining_ms > 0)
                                                 ? (uint32_t)(remaining_ms / 1000) + 1U
                                                 : 0U;
                char rows[4][LCD_LINE_SIZE];
                snprintf(rows[0], LCD_LINE_SIZE, "FACTORY RESET");
                snprintf(rows[1], LCD_LINE_SIZE, "PIN LOCKED");
                format_empty_pin_slots(rows[2], sizeof(rows[2]));
                snprintf(rows[3], LCD_LINE_SIZE, "Retry %2lus  BACK",
                         (unsigned long)remaining_s);
                const char *row_ptrs[] = {rows[0], rows[1], rows[2], rows[3]};
                draw_commit_rows(row_ptrs);
                return;
            }

            snprintf(attempt_line, sizeof(attempt_line),
                     "Attempts left: %u/5", (unsigned)attempts_left);
            const char *rows[] = {
                "FACTORY RESET PIN",
                pin_line,
                attempt_line,
                "ENT=SUBMIT BACK=EXIT"};
            draw_commit_rows(rows);
            return;
        }

        if (locked)
        {
            const int64_t remaining_ms =
                security_lockout_remaining_ms_for_scope(
                    SECURITY_LOCKOUT_FACTORY_RESET);
            const uint32_t remaining_s = (remaining_ms > 0)
                                             ? (uint32_t)(remaining_ms / 1000) + 1U
                                             : 0U;
            char r0_buf[LCD_LINE_SIZE];
            snprintf(r0_buf, sizeof(r0_buf), "LOCKED %2lus",

                     (unsigned long)remaining_s);
            char pin_slots[LCD_LINE_SIZE];
            format_empty_pin_slots(pin_slots, sizeof(pin_slots));
            draw_commit(r0_buf, pin_slots);
            return;
        }

        char r0[LCD_LINE_SIZE], r1[LCD_LINE_SIZE];
        snprintf(r0, sizeof(r0), "%-16s", "Enter PIN:      ");
        if (pin_reset_pending)
            format_empty_pin_slots(r1, sizeof(r1));
        else
            pin_entry_render_line(&d->pin_ctx, r1, sizeof(r1));
        draw_commit(r0, r1);
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
        draw_commit("Factory Reset   ", "Select an option");
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

    char r0[LCD_LINE_SIZE], r1[LCD_LINE_SIZE];

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
    char line1[LCD_LINE_SIZE], line2[LCD_LINE_SIZE];
    if (security_pin_change_required())
    {
        snprintf(line1, LCD_LINE_SIZE, "PIN Status:");
        snprintf(line2, LCD_LINE_SIZE, "Default (0000)");
    }
    else if (security_is_locked_out())
    {
        uint32_t remaining_s =
            (uint32_t)((security_lockout_remaining_ms() + 999) / 1000);
        snprintf(line1, LCD_LINE_SIZE, "PIN Status:");
        snprintf(line2, LCD_LINE_SIZE, "Locked %lus", (unsigned long)remaining_s);
    }
    else
    {
        snprintf(line1, LCD_LINE_SIZE, "PIN Status:");
        snprintf(line2, LCD_LINE_SIZE, "Custom, OK");
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
static bool draw_security_pin_flow(change_pin_phase_t flow_phase,
                                   security_action_t security_action)
{
    const security_lockout_scope_t scope =
        (security_action == SECURITY_ACTION_OTA_AUTH)
            ? SECURITY_LOCKOUT_OTA
            : SECURITY_LOCKOUT_GENERAL;
    if (security_is_locked_out_for_scope(scope))
    {
        const uint32_t remaining_s =
            (uint32_t)((security_lockout_remaining_ms_for_scope(scope) + 999) / 1000);
        const char *lock_title =
            (security_action == SECURITY_ACTION_OTA_AUTH) ? "OTA LOCKED" : "PIN LOCKED";

        if (lcd_geometry_is_20x4())
        {
            char rows[4][LCD_LINE_SIZE];
            snprintf(rows[0], sizeof(rows[0]), "%-*.*s", LCD_COLS, LCD_COLS,
                     lock_title);
            format_empty_pin_slots(rows[1], sizeof(rows[1]));
            snprintf(rows[2], sizeof(rows[2]), "Retry in %2lus",
                     (unsigned long)remaining_s);
            snprintf(rows[3], sizeof(rows[3]), "BACK=EXIT");
            const char *row_ptrs[] = {rows[0], rows[1], rows[2], rows[3]};
            draw_commit_rows(row_ptrs);
        }
        else
        {
            char row0[LCD_LINE_SIZE];
            snprintf(row0, sizeof(row0), "%s %2lus", lock_title,
                     (unsigned long)remaining_s);
            char pin_slots[LCD_LINE_SIZE];
            format_empty_pin_slots(pin_slots, sizeof(pin_slots));
            draw_commit(row0, pin_slots);
        }
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

    char r0[LCD_LINE_SIZE], r1[LCD_LINE_SIZE];
    const char *label = "Enter PIN:";

    if (security_action == SECURITY_ACTION_OTA_AUTH)
    {
        label = "OTA PIN:";
    }
    else
    {
        switch (flow_phase)
        {
        case CHANGE_PIN_VERIFY_OLD:
            label = "Enter Old PIN:";
            break;
        case CHANGE_PIN_ENTER_NEW:
            label = "Enter New PIN:";
            break;
        case CHANGE_PIN_CONFIRM_NEW:
            label = "Confirm PIN:";
            break;
        default:
            break;
        }
    }

    snprintf(r0, sizeof(r0), "%-*.*s", LCD_COLS, LCD_COLS, label);
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
        draw_security_pin_flow(flow_phase, sec_action);
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
    lcd_init_cgram();
    *last_screen = LCD_SCREEN_COUNT;

    /* Reset dirty-check buffers so draw_commit re-writes every character. */
    reset_row_cache();

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

    sys_lcd.main.sub_page = MAIN_SUB_OUTPUT;
    sys_lcd.main.sub_page_interval_ms = 5000;

    lcd_integrity_init();

    reset_row_cache();

    ESP_LOGI(TAG, "lcd_task started (%dx%d)", lcd_geometry_cols(), lcd_geometry_rows());
    lcd_init_cgram();
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

        /* ====== STEP 5: MAIN PAGE ROTATION ======
         * Deliberately disabled. The Enter button owns page changes so the
         * operator can read a stable dashboard without timed scrolling. */
        /* ====== STEP 6: SCREEN CHANGE DETECTION ====== */
        if (snap.screen != last_screen)
        {
            lcd_clear();
            reset_row_cache();
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
            /* No splash logo: move directly into functional startup status. */
            lcd_show_loading("System Starting",
                             SYSTEM_STARTUP_DISPLAY_DURATION_MS,
                             LCD_SCREEN_MAIN);
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

        case LCD_SCREEN_SETTINGS_VIEW:
            draw_detail(&snap.settings_view);
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

        case LCD_SCREEN_SYSTEM_EVENT:
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
            static char prev_flash_line0[LCD_LINE_SIZE] = {0};
            static char prev_flash_line1[LCD_LINE_SIZE] = {0};
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
