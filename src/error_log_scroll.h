#pragma once
/*==============================================================================
  error_log_scroll.h
  Scrollable error history for the diagnostic screen Error Logs item.

  Adds UP/DOWN navigation through all MAX_ERROR_LOG_ENTRIES entries while
  inside the Error Logs diagnostic detail view.
  The rest of the diagnostic items are unaffected.
==============================================================================*/
#include <stdint.h>
#include <stdbool.h>
#include "lcd_state.h"

/* ── How many entries the ring buffer holds ──────────────────────────── */
#define MAX_ERROR_LOG_ENTRIES 10

/* ── One entry in the error log ──────────────────────────────────────── */
typedef struct
{
    uint8_t error_code;    /* system_errors_t cast to uint8_t           */
    uint32_t timestamp_ms; /* xTaskGetTickCount()*portTICK_PERIOD_MS     */
    char description[16];  /* NUL-terminated human-readable string       */
} error_log_entry_t;

/*
 * Call when the user enters the Error Logs detail view.
 * Resets the scroll cursor to the most recent entry.
 */
void error_log_scroll_enter(void);

/*
 * Move cursor to the next older entry (DOWN button).
 * Wraps around to the newest when past the oldest.
 */
void error_log_scroll_next(void);

/*
 * Move cursor to the next newer entry (UP button).
 * Wraps around to the oldest when past the newest.
 */
void error_log_scroll_prev(void);

/*
 * Build the two LCD rows for the current scroll position and
 * write them into the lcd render state diagnostic detail slot.
 *
 * Row 0: "ErrLog  [X/N]   "  — entry index / total
 * Row 1: "<error description>" or "No logs        "
 */
void error_log_scroll_update_display(void);

/*
 * Returns true if there is more than one entry to scroll through.
 * Used by button handlers to decide whether to consume UP/DOWN.
 */
bool error_log_scroll_has_multiple(void);

/* Current scroll index (0 = newest). */
uint8_t error_log_scroll_index(void);