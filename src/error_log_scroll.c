/*==============================================================================
  error_log_scroll.c
  Scrollable error history — implementation.
==============================================================================*/
#include "error_log_scroll.h"
#include "lcd_writer.h"
#include <stdio.h>
#include <string.h>

/* These are defined in main.c */
extern uint8_t              error_log_head;
extern uint8_t              error_log_count;
extern error_log_entry_t    error_log_ring[];   /* [MAX_ERROR_LOG_ENTRIES]  */

#define MAX_ERROR_LOG_ENTRIES 10

/* ── Scroll state ────────────────────────────────────────────────────── */
static uint8_t s_cursor = 0;   /* 0 = newest entry, count-1 = oldest       */

/* ── Private helpers ─────────────────────────────────────────────────── */

/*
 * Convert a scroll cursor position (0=newest) to a ring buffer index.
 * head-1 is the newest write slot; we step backwards from there.
 */
static uint8_t cursor_to_ring_index(uint8_t cursor)
{
    /* newest slot = (head - 1 + MAX) % MAX */
    return (uint8_t)((error_log_head + MAX_ERROR_LOG_ENTRIES - 1 - cursor)
                     % MAX_ERROR_LOG_ENTRIES);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void error_log_scroll_enter(void)
{
    s_cursor = 0;   /* start at newest */
    error_log_scroll_update_display();
}

void error_log_scroll_next(void)
{
    if (error_log_count == 0) return;
    s_cursor = (s_cursor + 1) % error_log_count;
    error_log_scroll_update_display();
}

void error_log_scroll_prev(void)
{
    if (error_log_count == 0) return;
    s_cursor = (s_cursor == 0) ? error_log_count - 1 : s_cursor - 1;
    error_log_scroll_update_display();
}

void error_log_scroll_update_display(void)
{
    char row0[17], row1[17];

    if (error_log_count == 0) {
        snprintf(row0, 17, "%-16s", "Error Log       ");
        snprintf(row1, 17, "%-16s", "No logs         ");
    } else {
        uint8_t idx = cursor_to_ring_index(s_cursor);
        const error_log_entry_t *entry = &error_log_ring[idx];

        /* Row 0: label + position indicator  "ErrLog   [2/7]  " */
        char indicator[10];
        snprintf(indicator, sizeof(indicator), "[%d/%d]",
                 s_cursor + 1, error_log_count);
        int ind_len   = (int)strlen(indicator);
        int label_w   = 16 - ind_len;
        if (label_w < 0)
            label_w = 0;
        snprintf(row0, sizeof(row0), "%-6.*s%s", label_w,
                 "ErrLog", indicator);

        /* Row 1: description + timestamp hint  "OverTemp  1234s " */
        /* Show description left-justified, timestamp (seconds) right */
        uint32_t age_s = entry->timestamp_ms / 1000;
        char ts[7];
        if (age_s < 99999)
            snprintf(ts, sizeof(ts), "%5lus", (unsigned long)age_s);
        else
            snprintf(ts, sizeof(ts), ">100ks");

        int desc_w = 16 - (int)strlen(ts) - 1;   /* -1 for space separator */
        if (desc_w < 1) desc_w = 1;
        snprintf(row1, 17, "%-*.*s %s",
                 desc_w, desc_w,
                 entry->description[0] ? entry->description : "?",
                 ts);
    }

    row0[16] = '\0';
    row1[16] = '\0';
    lcd_show_diagnostic_detail(row0, row1);
}

bool error_log_scroll_has_multiple(void)
{
    return (error_log_count > 1);
}

uint8_t error_log_scroll_index(void)
{
    return s_cursor;
}