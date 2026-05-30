/*==============================================================================
  lcd_flash_queue.c
  Priority-aware flash message queue implementation.

  Data structure: fixed-size array kept in descending priority order.
  - Head (index 0) is always the highest-priority message.
  - On insert we find the correct position by priority and shift right.
  - On drop we remove the tail (lowest priority, last valid slot).

  Concurrency: protected by a single FreeRTOS mutex.
  All public functions are non-blocking from the caller's perspective;
  the mutex is held only for microseconds.
==============================================================================*/
#include "lcd_flash_queue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

/* ── Internal state ──────────────────────────────────────────────────── */
static flash_entry_t s_queue[LCD_FLASH_QUEUE_DEPTH];
static uint8_t s_count = 0; /* number of valid entries    */
static SemaphoreHandle_t s_mutex = NULL;

/* ── Private helpers ─────────────────────────────────────────────────── */

static void copy_entry(flash_entry_t *dst, const flash_entry_t *src)
{
    memcpy(dst, src, sizeof(flash_entry_t));
}

/*
 * Insert entry at position `pos`, shifting existing entries right.
 * Assumes s_count < LCD_FLASH_QUEUE_DEPTH (space already verified by caller).
 */
static void insert_at(uint8_t pos, const flash_entry_t *entry)
{
    /* Shift everything from pos..s_count-1 one slot to the right */
    for (int i = (int)s_count; i > (int)pos; i--)
        copy_entry(&s_queue[i], &s_queue[i - 1]);
    copy_entry(&s_queue[pos], entry);
    s_count++;
}

/*
 * Remove the entry at index `pos`, shifting left to close the gap.
 */
static void remove_at(uint8_t pos)
{
    for (uint8_t i = pos; i < s_count - 1; i++)
        copy_entry(&s_queue[i], &s_queue[i + 1]);
    s_queue[s_count - 1].valid = false;
    s_count--;
}

/*
 * Find the insertion position for a new entry with `priority`.
 * We want descending order: highest priority at index 0.
 * Returns the index where new entry should be placed.
 */
static uint8_t find_insert_pos(flash_priority_t priority)
{
    for (uint8_t i = 0; i < s_count; i++)
    {
        if (priority > s_queue[i].priority)
            return i; /* new entry is higher → insert before  */
    }
    return s_count; /* new entry is lowest → append         */
}

/* ── Public API ──────────────────────────────────────────────────────── */

void lcd_flash_queue_init(void)
{
    memset(s_queue, 0, sizeof(s_queue));
    s_count = 0;
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex != NULL);
}

void lcd_flash_enqueue(const char *line0,
                       const char *line1,
                       uint32_t duration_ms,
                       flash_priority_t priority)
{
    if (!s_mutex)
        return;

    /* Build the entry before taking the lock */
    flash_entry_t entry = {.valid = true, .priority = priority, .duration_ms = duration_ms, .return_to = LCD_SCREEN_MAIN};
    snprintf(entry.line0, sizeof(entry.line0), "%-16.16s", line0 ? line0 : "");
    snprintf(entry.line1, sizeof(entry.line1), "%-16.16s", line1 ? line1 : "");

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_count < LCD_FLASH_QUEUE_DEPTH)
    {
        /* Space available — insert in priority order */
        uint8_t pos = find_insert_pos(priority);
        insert_at(pos, &entry);
    }
    else
    {
        /*
         * Queue is full.
         * Tail (index s_count-1) is the lowest-priority waiting message.
         * Drop it only if the new message has strictly higher priority.
         */
        flash_priority_t tail_pri = s_queue[s_count - 1].priority;
        if (priority > tail_pri)
        {
            /* Drop the tail (lowest priority) */
            s_count--;
            s_queue[s_count].valid = false;

            /* Insert new entry in the correct position */
            uint8_t pos = find_insert_pos(priority);
            insert_at(pos, &entry);
        }
        /* else: incoming priority <= tail priority → silently drop */
    }

    xSemaphoreGive(s_mutex);
}

bool lcd_flash_dequeue(flash_entry_t *out)
{
    if (!s_mutex || !out)
        return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool found = (s_count > 0 && s_queue[0].valid);
    if (found)
    {
        copy_entry(out, &s_queue[0]);
        remove_at(0);
    }

    xSemaphoreGive(s_mutex);
    return found;
}

bool lcd_flash_queue_has_pending(void)
{
    if (!s_mutex)
        return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool result = (s_count > 0);
    xSemaphoreGive(s_mutex);
    return result;
}

uint8_t lcd_flash_queue_count(void)
{
    if (!s_mutex)
        return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t c = s_count;
    xSemaphoreGive(s_mutex);
    return c;
}

void lcd_flash_queue_purge_below(flash_priority_t min_priority)
{
    if (!s_mutex)
        return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /*
     * Walk from tail to head, removing entries below min_priority.
     * Walking backwards avoids index invalidation from remove_at().
     */
    for (int i = (int)s_count - 1; i >= 0; i--)
    {
        if (s_queue[i].priority < min_priority)
            remove_at((uint8_t)i);
    }

    xSemaphoreGive(s_mutex);
}

/* ── Convenience wrappers ────────────────────────────────────────────── */

void lcd_flash_info(const char *l0, const char *l1, uint32_t ms)
{
    lcd_flash_enqueue(l0, l1, ms, FLASH_PRI_INFO);
}

void lcd_flash_warning(const char *l0, const char *l1, uint32_t ms)
{
    lcd_flash_enqueue(l0, l1, ms, FLASH_PRI_WARNING);
}

void lcd_flash_fault(const char *l0, const char *l1, uint32_t ms)
{
    lcd_flash_enqueue(l0, l1, ms, FLASH_PRI_FAULT);
}

void lcd_flash_critical(const char *l0, const char *l1, uint32_t ms)
{
    lcd_flash_enqueue(l0, l1, ms, FLASH_PRI_CRITICAL);
}