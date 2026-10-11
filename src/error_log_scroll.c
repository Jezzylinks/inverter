/*==============================================================================
  error_log_scroll.c
  Scrollable diagnostic view backed by the persistent system fault log.
==============================================================================*/
#include "lcd/error_log_scroll.h"
#include "lcd/lcd_writer.h"
#include "events/fault_log.h"

#include <stdio.h>
#include <string.h>

static error_log_entry_t s_entries[MAX_ERROR_LOG_ENTRIES];
static uint8_t s_count;
static uint8_t s_cursor; /* 0 = newest entry */

static void refresh_entries(void)
{
    fault_log_entry_t latest[MAX_ERROR_LOG_ENTRIES] = {0};
    const uint32_t count = fault_log_get_entries(latest, MAX_ERROR_LOG_ENTRIES);

    s_count = count > MAX_ERROR_LOG_ENTRIES
                  ? MAX_ERROR_LOG_ENTRIES
                  : (uint8_t)count;
    memset(s_entries, 0, sizeof(s_entries));

    for (uint8_t i = 0U; i < s_count; ++i)
    {
        s_entries[i].error_code = (uint8_t)latest[i].source;
        s_entries[i].timestamp_ms = latest[i].timestamp_ms;
        if (latest[i].message[0] != '\0')
        {
            snprintf(s_entries[i].description,
                     sizeof(s_entries[i].description), "%s",
                     latest[i].message);
        }
        else
        {
            snprintf(s_entries[i].description,
                     sizeof(s_entries[i].description), "%s",
                     fault_log_severity_name(latest[i].severity));
        }
    }

    if (s_count == 0U)
    {
        s_cursor = 0U;
    }
    else if (s_cursor >= s_count)
    {
        s_cursor = (uint8_t)(s_count - 1U);
    }
}

void error_log_scroll_enter(void)
{
    s_cursor = 0U;
    refresh_entries();
    error_log_scroll_update_display();
}

void error_log_scroll_next(void)
{
    refresh_entries();
    if (s_count == 0U)
        return;
    s_cursor = (uint8_t)((s_cursor + 1U) % s_count);
    error_log_scroll_update_display();
}

void error_log_scroll_prev(void)
{
    refresh_entries();
    if (s_count == 0U)
        return;
    s_cursor = (s_cursor == 0U) ? (uint8_t)(s_count - 1U)
                                : (uint8_t)(s_cursor - 1U);
    error_log_scroll_update_display();
}

void error_log_scroll_update_display(void)
{
    char row0[LCD_LINE_SIZE] = {0};
    char row1[LCD_LINE_SIZE] = {0};

    refresh_entries();
    if (s_count == 0U)
    {
        snprintf(row0, sizeof(row0), "%-*.*s", LCD_COLS, LCD_COLS, "Error Logs");
        snprintf(row1, sizeof(row1), "%-*.*s", LCD_COLS, LCD_COLS, "No logs captured");
    }
    else
    {
        const error_log_entry_t *entry = &s_entries[s_cursor];
        char indicator[12];
        snprintf(indicator, sizeof(indicator), "[%u/%u]",
                 (unsigned)(s_cursor + 1U), (unsigned)s_count);
        const int indicator_len = (int)strlen(indicator);
        int label_width = LCD_COLS - indicator_len;
        if (label_width < 0)
            label_width = 0;
        snprintf(row0, sizeof(row0), "%-*.*s%s",
                 label_width, label_width, "Error Log", indicator);

        const uint32_t uptime_seconds = entry->timestamp_ms / 1000U;
        char time_text[8];
        if (uptime_seconds < 99999U)
            snprintf(time_text, sizeof(time_text), "%5lus",
                     (unsigned long)uptime_seconds);
        else
            snprintf(time_text, sizeof(time_text), ">100ks");

        int description_width = LCD_COLS - (int)strlen(time_text) - 1;
        if (description_width < 1)
            description_width = 1;
        snprintf(row1, sizeof(row1), "%-*.*s %s",
                 description_width, description_width,
                 entry->description[0] ? entry->description : "?",
                 time_text);
    }

    row0[LCD_COLS] = '\0';
    row1[LCD_COLS] = '\0';
    lcd_show_diagnostic_detail(row0, row1);
}

bool error_log_scroll_has_multiple(void)
{
    refresh_entries();
    return s_count > 1U;
}

uint8_t error_log_scroll_index(void)
{
    return s_cursor;
}
