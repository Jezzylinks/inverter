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

#define FLASH_DURATION_SHORT 1000
#define FLASH_PRIORITY_NORMAL 1500
#define FLASH_PRIORITY_HIGH 2000
#define FLASH_DURATION_LONG 3000

/* ── Priority levels ─────────────────────────────────────────────────── */
typedef enum
{
  FLASH_PRI_INFO = 0,     /* general notices: "Value Saved", "Cancelled" */
  FLASH_PRI_WARNING = 1,  /* non-critical alerts: low battery warning    */
  FLASH_PRI_FAULT = 2,    /* faults: over-temp, overload, fan fail       */
  FLASH_PRI_CRITICAL = 3, /* emergency: relay fault, emergency shutdown  */
} flash_priority_t;

/*==============================================================================
  Active flash state
==============================================================================*/
typedef struct
{
  bool active;
  char line0[LCD_LINE_SIZE];
  char line1[LCD_LINE_SIZE];
  uint32_t expire_ms;
  flash_priority_t priority;
  lcd_screen_id_t return_to;
} active_flash_t;

/* ── Single queue entry ──────────────────────────────────────────────── */
typedef struct
{
  char line0[LCD_LINE_SIZE];
  char line1[LCD_LINE_SIZE];
  uint32_t duration_ms;
  flash_priority_t priority;
  lcd_screen_id_t return_to; /* screen to restore when timer expires   */
  bool valid;
} flash_entry_t;

/* ── Public API ──────────────────────────────────────────────────────── */

/* Call once before any task starts. */
void lcd_flash_init(TaskHandle_t lcd_task_handle);
bool lcd_flash_is_initialized(void);
void lcd_flash_enqueue_to(const char *line0,
                          const char *line1,
                          uint32_t duration_ms,
                          flash_priority_t priority,
                          lcd_screen_id_t return_to_override);

/*
 * Convenience wrappers — use these instead of calling lcd_flash_enqueue
 * directly throughout the codebase.
 */
void lcd_flash_info(const char *line0, const char *line1, uint32_t ms);
void lcd_flash_warning(const char *line0, const char *line1, uint32_t ms);
void lcd_flash_fault(const char *line0, const char *line1, uint32_t ms);
void lcd_flash_critical(const char *line0, const char *line1, uint32_t ms);

void lcd_flash_info_to(const char *line0, const char *line1, uint32_t ms, lcd_screen_id_t return_to);
void lcd_flash_warning_to(const char *line0, const char *line1, uint32_t ms, lcd_screen_id_t return_to);
void lcd_flash_fault_to(const char *line0, const char *line1, uint32_t ms, lcd_screen_id_t return_to);
void lcd_flash_critical_to(const char *line0, const char *line1, uint32_t ms, lcd_screen_id_t return_to);

bool lcd_flash_is_active(void);
bool lcd_flash_get(active_flash_t *flash);
bool lcd_flash_is_expired(void);
void lcd_flash_clear(void);
lcd_screen_id_t lcd_flash_clear_and_get_return(void);

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