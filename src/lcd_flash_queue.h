#pragma once
/*==============================================================================
  lcd_flash_queue.h
  Priority-aware flash message queue for lcd_task.

  Rules:
  - Queue holds up to LCD_FLASH_QUEUE_DEPTH messages.
  - Higher priority messages jump ahead of lower priority ones in the queue.
  - If the queue is full, the lowest-priority waiting message is dropped to
    make room for a higher-priority incoming message.
  - If the queue is full and the incoming message has equal or lower priority
    than everything waiting, it is silently dropped.
  - lcd_task drains one message per slot; while a flash is showing no new
    flash can overwrite it mid-display.
  - Caller never blocks — all functions return immediately.
==============================================================================*/
#include <stdint.h>
#include <stdbool.h>
#include "lcd_state.h" /* for lcd_screen_id_t */

/* ── Priority levels ─────────────────────────────────────────────────── */
typedef enum
{
    FLASH_PRI_INFO = 0,     /* general notices: "Value Saved", "Cancelled" */
    FLASH_PRI_WARNING = 1,  /* non-critical alerts: low battery warning    */
    FLASH_PRI_FAULT = 2,    /* faults: over-temp, overload, fan fail       */
    FLASH_PRI_CRITICAL = 3, /* emergency: relay fault, emergency shutdown  */
} flash_priority_t;

/* ── Single queue entry ──────────────────────────────────────────────── */
typedef struct
{
    char line0[17];
    char line1[17];
    uint32_t duration_ms;
    flash_priority_t priority;
    lcd_screen_id_t return_to; /* screen to restore when timer expires   */
    bool valid;
} flash_entry_t;

#define LCD_FLASH_QUEUE_DEPTH 4

/* ── Public API ──────────────────────────────────────────────────────── */

/* Call once before any task starts. */
void lcd_flash_queue_init(void);

/*
 * Enqueue a flash message.
 * - If queue has space: inserts in priority order (highest first).
 * - If queue is full and this priority > lowest waiting: drops lowest,
 *   inserts this one in priority order.
 * - If queue is full and this priority <= lowest waiting: drops silently.
 * Never blocks. Safe to call from any task or ISR context.
 */
void lcd_flash_enqueue(const char *line0,
                       const char *line1,
                       uint32_t duration_ms,
                       flash_priority_t priority);

/*
 * Convenience wrappers — use these instead of calling lcd_flash_enqueue
 * directly throughout the codebase.
 */
void lcd_flash_info(const char *line0, const char *line1, uint32_t ms);
void lcd_flash_warning(const char *line0, const char *line1, uint32_t ms);
void lcd_flash_fault(const char *line0, const char *line1, uint32_t ms);
void lcd_flash_critical(const char *line0, const char *line1, uint32_t ms);

/*
 * Called exclusively by lcd_task to dequeue the next message.
 * Returns true and fills *out if a message is waiting; false if empty.
 */
bool lcd_flash_dequeue(flash_entry_t *out);

/* Returns true if at least one message is waiting. */
bool lcd_flash_queue_has_pending(void);

/* Returns the count of messages currently in the queue. */
uint8_t lcd_flash_queue_count(void);

/* Discard all queued messages below the given priority. */
void lcd_flash_queue_purge_below(flash_priority_t min_priority);
cm\