/*==============================================================================
  error_log_scroll.c
  Scrollable fault history and four-row error-definition reader.
==============================================================================*/
#include "lcd/error_log_scroll.h"
#include "lcd/lcd_writer.h"
#include "events/fault_log.h"

#include <stdio.h>
#include <string.h>

static error_log_entry_t s_entries[MAX_ERROR_LOG_ENTRIES];
static uint8_t s_count;
static uint8_t s_cursor;
static bool s_detail_mode;
static uint8_t s_detail_page;

static const char *source_definition(fault_source_t source)
{
    switch (source) {
    case FAULT_SRC_PROTECTION_AC_VOLTAGE: return "AC voltage left its safe range. Check source and output wiring.";
    case FAULT_SRC_PROTECTION_CURRENT: return "Output current exceeded its limit. Reduce the connected load.";
    case FAULT_SRC_PROTECTION_TEMPERATURE: return "Internal temperature is too high. Clear vents and allow cooling.";
    case FAULT_SRC_PROTECTION_BATTERY: return "Battery voltage is outside limits. Check battery, wiring and charge.";
    case FAULT_SRC_WATCHDOG: return "A monitored task stopped responding. Review logs and task health.";
    case FAULT_SRC_BROWNOUT: return "Supply voltage dropped unexpectedly. Check battery and connections.";
    case FAULT_SRC_NVS: return "Settings or log storage failed. Check NVS and flash health.";
    case FAULT_SRC_LCD: return "LCD communication failed. Check I2C wiring and the 25 kHz bus.";
    case FAULT_SRC_BUTTON: return "Button input fault detected. Check switches and button wiring.";
    case FAULT_SRC_RESTART_POLICY: return "Repeated restart detected. Inspect the cause before continued use.";
    case FAULT_SRC_FAN: return "Cooling fan did not respond as expected. Check fan and tachometer.";
    case FAULT_SRC_POWER_METER: return "Measurement or telemetry event. Verify sensors and ADC readings.";
    case FAULT_SRC_OTHER:
    default: return "General system event. Check the event text and system status.";
    }
}

static void refresh_entries(void)
{
    fault_log_entry_t latest[MAX_ERROR_LOG_ENTRIES] = {0};
    const uint32_t count = fault_log_get_entries(latest, MAX_ERROR_LOG_ENTRIES);
    s_count = count > MAX_ERROR_LOG_ENTRIES ? MAX_ERROR_LOG_ENTRIES : (uint8_t)count;
    memset(s_entries, 0, sizeof(s_entries));
    for (uint8_t i = 0U; i < s_count; ++i) {
        s_entries[i].error_code = (uint8_t)latest[i].source;
        s_entries[i].timestamp_ms = latest[i].timestamp_ms;
        if (latest[i].message[0] != '\0')
            snprintf(s_entries[i].description, sizeof(s_entries[i].description), "%s", latest[i].message);
        else
            snprintf(s_entries[i].description, sizeof(s_entries[i].description), "%s",
                     fault_log_severity_name(latest[i].severity));
    }
    if (s_count == 0U) s_cursor = 0U;
    else if (s_cursor >= s_count) s_cursor = (uint8_t)(s_count - 1U);
}

void error_log_scroll_enter(void)
{
    s_cursor = 0U; s_detail_mode = false; s_detail_page = 0U;
    refresh_entries();
    error_log_scroll_update_display();
}

void error_log_scroll_select_detail(void)
{
    refresh_entries();
    if (s_count == 0U) return;
    s_detail_mode = true; s_detail_page = 0U;
    error_log_scroll_update_display();
}

void error_log_scroll_return_to_list(void)
{
    if (!s_detail_mode) return;
    s_detail_mode = false; s_detail_page = 0U;
    error_log_scroll_update_display();
}

bool error_log_scroll_is_detail(void) { return s_detail_mode; }

static void scroll_definition_page(int direction)
{
    const uint8_t max_page = (LCD_COLS >= 20U) ? 2U : 5U;
    if (direction > 0) s_detail_page = (uint8_t)((s_detail_page + 1U) % (max_page + 1U));
    else s_detail_page = (s_detail_page == 0U) ? max_page : (uint8_t)(s_detail_page - 1U);
}

void error_log_scroll_next(void)
{
    refresh_entries();
    if (s_count == 0U) return;
    if (s_detail_mode) scroll_definition_page(1);
    else s_cursor = (uint8_t)((s_cursor + 1U) % s_count);
    error_log_scroll_update_display();
}

void error_log_scroll_prev(void)
{
    refresh_entries();
    if (s_count == 0U) return;
    if (s_detail_mode) scroll_definition_page(-1);
    else s_cursor = (s_cursor == 0U) ? (uint8_t)(s_count - 1U) : (uint8_t)(s_cursor - 1U);
    error_log_scroll_update_display();
}

static void append_two_digit_number(char *row, size_t *position,
                                    unsigned value)
{
    value %= 100U;
    if (value >= 10U)
        row[(*position)++] = (char)('0' + ((value / 10U) % 10U));
    row[(*position)++] = (char)('0' + (value % 10U));
}

static void format_log_header(char row[LCD_LINE_SIZE],
                              const error_log_entry_t *entry,
                              uint8_t cursor, uint8_t count, bool detail)
{
    memset(row, ' ', LCD_COLS);
    size_t pos = 0U;
    const unsigned code = (unsigned)entry->error_code + 1U;
    row[pos++] = 'E';
    append_two_digit_number(row, &pos, code);
    row[pos++] = ' ';
    if (detail) row[pos++] = 'D';
    row[pos++] = '[';
    append_two_digit_number(row, &pos, (unsigned)cursor + 1U);
    row[pos++] = '/';
    append_two_digit_number(row, &pos, count);
    row[pos++] = ']';
    row[LCD_COLS] = '\0';
}

static void wrap_definition(const char *text, uint8_t page,
                            char row2[LCD_LINE_SIZE], char row3[LCD_LINE_SIZE])
{
    const size_t width = LCD_COLS, len = strlen(text);
    size_t starts[8] = {0}, line_count = 0U, pos = 0U;
    while (pos < len && line_count < 7U) {
        starts[line_count++] = pos;
        size_t end = pos + width;
        if (end >= len) break;
        size_t split = end;
        while (split > pos && text[split] != ' ') --split;
        pos = split > pos ? split + 1U : end;
    }
    if (line_count == 0U) line_count = 1U;
    char *rows[2] = {row2, row3};
    const size_t lines_per_page = (LCD_COLS >= 20U) ? 2U : 1U;
    for (size_t r = 0U; r < 2U; ++r) {
        rows[r][0] = '\0';
        const size_t line = (size_t)page * lines_per_page + r;
        if (line >= line_count) continue;
        const size_t start = starts[line];
        size_t end = line + 1U < line_count ? starts[line + 1U] : len;
        while (end > start && text[end - 1U] == ' ') --end;
        size_t n = end - start;
        if (n > LCD_COLS) n = LCD_COLS;
        memcpy(rows[r], text + start, n);
        rows[r][n] = '\0';
    }
}

void error_log_scroll_update_display(void)
{
    char row0[LCD_LINE_SIZE] = {0}, row1[LCD_LINE_SIZE] = {0};
    char row2[LCD_LINE_SIZE] = {0}, row3[LCD_LINE_SIZE] = {0};
    refresh_entries();
    if (s_count == 0U) {
        snprintf(row0, sizeof(row0), "%-*.*s", LCD_COLS, LCD_COLS, "Error Logs");
        snprintf(row1, sizeof(row1), "%-*.*s", LCD_COLS, LCD_COLS, "No logs captured");
    } else {
        const error_log_entry_t *entry = &s_entries[s_cursor];
        if (s_detail_mode) {
            format_log_header(row0, entry, s_cursor, s_count, true);
            snprintf(row1, sizeof(row1), "%-*.*s", LCD_COLS, LCD_COLS,
                     fault_log_source_name((fault_source_t)entry->error_code));
            wrap_definition(source_definition((fault_source_t)entry->error_code),
                            s_detail_page, row2, row3);
        } else {
            format_log_header(row0, entry, s_cursor, s_count, false);
            const unsigned error_number = (unsigned)entry->error_code + 1U;
            const char *event_text = entry->description[0]
                                         ? entry->description
                                         : "Unknown event";
            const size_t event_width = LCD_COLS - 4U;
            memset(row1, ' ', LCD_COLS);
            row1[0] = 'E';
            row1[1] = (char)('0' + ((error_number / 10U) % 10U));
            row1[2] = (char)('0' + (error_number % 10U));
            row1[3] = ' ';
            size_t event_len = strlen(event_text);
            if (event_len > event_width) event_len = event_width;
            memcpy(row1 + 4U, event_text, event_len);
            row1[LCD_COLS] = '\0';
        }
    }
    row0[LCD_COLS] = '\0'; row1[LCD_COLS] = '\0';
    row2[LCD_COLS] = '\0'; row3[LCD_COLS] = '\0';
    lcd_show_diagnostic_detail_rows(row0, row1, row2, row3);
}

bool error_log_scroll_has_multiple(void) { refresh_entries(); return s_count > 1U; }
uint8_t error_log_scroll_index(void) { return s_cursor; }
