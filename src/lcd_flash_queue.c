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
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <esp_log.h>

#define TAG "LCD_FLASH"
#define LCD_FLASH_QUEUE_DEPTH 16
#define LCD_LINE_LENGTH 17

static inline uint32_t _lcd_get_time_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static TaskHandle_t s_lcd_task = NULL;
extern active_flash_t s_active_flash;
extern lcd_render_state_t sys_lcd;
static flash_entry_t s_queue[LCD_FLASH_QUEUE_DEPTH];
static uint8_t s_queue_count = 0; /* number of valid entries    */

/* Mutex 1: Protects the active flash struct */
static SemaphoreHandle_t s_flash_mutex = NULL;
static SemaphoreHandle_t s_queue_mutex = NULL;

void lcd_flash_init(TaskHandle_t lcd_task_handle)
{
    if (!s_flash_mutex)
        s_flash_mutex = xSemaphoreCreateMutex();

    if (!s_queue_mutex)
        s_queue_mutex = xSemaphoreCreateMutex();

    s_lcd_task = lcd_task_handle;

    /* ✅ INITIALIZE ACTIVE FLASH AS INACTIVE */
    memset(&s_active_flash, 0, sizeof(s_active_flash));
    s_active_flash.active = false; // ← CRITICAL!

    memset(s_queue, 0, sizeof(s_queue));
    s_queue_count = 0;

    ESP_LOGI(TAG, "✓ Flash system initialized");
}

/* ============================================================================
 * ACTIVE FLASH MANAGEMENT
 * ============================================================================ */

/**
 * @brief Get the currently active flash message
 *
 * Called by lcd_task to render the flash message.
 *
 * @param flash Output: current active flash
 * @return true if flash is active and should be displayed
 */
bool lcd_flash_is_active(void)
{
    xSemaphoreTake(s_flash_mutex, portMAX_DELAY);
    bool active = s_active_flash.active;
    xSemaphoreGive(s_flash_mutex);

    return active;
}

/**
 * @brief Get copy of active flash for rendering
 *
 * @param flash Output: pointer to active flash struct
 * @return true if flash is active
 */
bool lcd_flash_get(active_flash_t *flash)
{
    if (!flash)
        return false;

    xSemaphoreTake(s_flash_mutex, portMAX_DELAY);

    bool is_active = s_active_flash.active;
    if (is_active)
    {
        memcpy(flash, &s_active_flash, sizeof(active_flash_t));
    }

    xSemaphoreGive(s_flash_mutex);

    return is_active;
}

/**
 * @brief Check if active flash has expired
 *
 * Called by lcd_task to determine if flash should timeout.
 *
 * @return true if flash is active AND expired
 */
bool lcd_flash_is_expired(void)
{
    xSemaphoreTake(s_flash_mutex, portMAX_DELAY);

    bool expired = false;
    if (s_active_flash.active && _lcd_get_time_ms() >= s_active_flash.expire_ms)
    {
        expired = true;
    }

    xSemaphoreGive(s_flash_mutex);

    return expired;
}

/**
 * @brief Clear the active flash (call when it expires)
 */
void lcd_flash_clear(void)
{
    /* ✅ CLEAR THE ACTIVE FLASH (set active = false) */
    xSemaphoreTake(s_flash_mutex, portMAX_DELAY);
    s_active_flash.active = false; // ← THIS IS CRITICAL!
    xSemaphoreGive(s_flash_mutex);

    ESP_LOGI(TAG, "✓ Active flash cleared");
}

static void copy_entry(flash_entry_t *dst, const flash_entry_t *src)
{
    memcpy(dst, src, sizeof(flash_entry_t));
}

/*
 * Insert entry at position `pos`, shifting existing entries right.
 * Assumes s_queue_count < LCD_FLASH_QUEUE_DEPTH (space already verified by caller).
 */
static void insert_at(uint8_t pos, const flash_entry_t *entry)
{
    /* Shift everything from pos..s_queue_count-1 one slot to the right */
    for (int i = (int)s_queue_count; i > (int)pos; i--)
        copy_entry(&s_queue[i], &s_queue[i - 1]);
    copy_entry(&s_queue[pos], entry);
    s_queue_count++;
}

/*
 * Remove the entry at index `pos`, shifting left to close the gap.
 */
static void remove_at(uint8_t pos)
{
    for (uint8_t i = pos; i < s_queue_count - 1; i++)
        copy_entry(&s_queue[i], &s_queue[i + 1]);
    s_queue[s_queue_count - 1].valid = false;
    s_queue_count--;
}

/*
 * Find the insertion position for a new entry with `priority`.
 * We want descending order: highest priority at index 0.
 * Returns the index where new entry should be placed.
 */
static uint8_t find_insert_pos(flash_priority_t priority)
{
    for (uint8_t i = 0; i < s_queue_count; i++)
    {
        if (priority > s_queue[i].priority)
            return i; /* new entry is higher → insert before  */
    }
    return s_queue_count; /* new entry is lowest → append         */
}

/**
 * @brief Enqueue flash message with priority
 *
 * ✅ IMMEDIATE DISPLAY:
 * If no flash is currently active, this updates s_active_flash directly
 * and switches the screen. Otherwise, it queues for later.
 *
 * This ensures the first flash displays within ~10ms, not 100ms.
 */

void lcd_flash_enqueue(const char *line0,
                       const char *line1,
                       uint32_t duration_ms,
                       flash_priority_t priority)
{
    if (!s_flash_mutex)
        return;

    /* Build the entry before taking locks */
    flash_entry_t entry = {
        .valid = true,
        .priority = priority,
        .duration_ms = duration_ms,
        .return_to = LCD_SCREEN_MAIN};

    snprintf(entry.line0, sizeof(entry.line0), "%-16.16s", line0 ? line0 : "");
    snprintf(entry.line1, sizeof(entry.line1), "%-16.16s", line1 ? line1 : "");

    /* ✅ KEY: Check if flash is already active */
    xSemaphoreTake(s_flash_mutex, portMAX_DELAY);
    bool flash_active = s_active_flash.active;
    xSemaphoreGive(s_flash_mutex);

    if (!flash_active)
    {
        /* ✅ NO FLASH CURRENTLY SHOWING: Display this one immediately! */

        xSemaphoreTake(s_flash_mutex, portMAX_DELAY);

        /* Update the display flash struct */
        strcpy(s_active_flash.line0, entry.line0);
        strcpy(s_active_flash.line1, entry.line1);
        s_active_flash.priority = priority;
        s_active_flash.expire_ms = _lcd_get_time_ms() + duration_ms;
        s_active_flash.return_to = LCD_SCREEN_MAIN;
        s_active_flash.active = true;

        xSemaphoreGive(s_flash_mutex);

        /* Switch screen to flash display */
        xSemaphoreTake(s_flash_mutex, portMAX_DELAY);
        sys_lcd.screen = LCD_SCREEN_FLASH_MSG;
        xSemaphoreGive(s_flash_mutex);

        ESP_LOGI(TAG, "📢 FLASH_IMMEDIATE: '%s' / '%s' (%lu ms, pri=%d)",
                 entry.line0, entry.line1, duration_ms, priority);
    }
    else
    {
        /* ✅ FLASH ALREADY SHOWING: Queue this one for later */

        xSemaphoreTake(s_flash_mutex, portMAX_DELAY);

        if (s_queue_count < LCD_FLASH_QUEUE_DEPTH)
        {
            /* Space available — insert in priority order */
            uint8_t pos = find_insert_pos(priority);
            insert_at(pos, &entry);

            ESP_LOGI(TAG, "📋 FLASH_QUEUED: '%s' (pos=%d, queue_size=%d, pri=%d)",
                     entry.line0, pos, s_queue_count, priority);
        }
        else
        {
            /* Queue is full — drop lowest priority if new has higher */
            flash_priority_t tail_pri = s_queue[s_queue_count - 1].priority;
            if (priority > tail_pri)
            {
                ESP_LOGW(TAG, "⚠️ Queue full, dropping lower priority");
                s_queue_count--;

                uint8_t pos = find_insert_pos(priority);
                insert_at(pos, &entry);

                ESP_LOGI(TAG, "📋 FLASH_QUEUED (replaced): '%s' (queue_size=%d)",
                         entry.line0, s_queue_count);
            }
            else
            {
                ESP_LOGW(TAG, "⚠️ Queue full, rejecting lower priority flash");
            }
        }

        xSemaphoreGive(s_flash_mutex);
    }
}

/**
 * @brief Dequeue next pending message (highest priority first)
 *
 * ✅ KEY FUNCTION: Pulls from queue and returns entry to be activated.
 *
 * Called by lcd_task when current flash expires.
 * If queue has pending messages, this extracts the highest priority one.
 *
 * @param entry Output: dequeued entry
 * @return true if message dequeued, false if queue empty
 */

bool lcd_flash_dequeue(flash_entry_t *entry)
{
    if (!entry)
        return false;

    xSemaphoreTake(s_flash_mutex, portMAX_DELAY);

    if (s_queue_count == 0)
    {
        xSemaphoreGive(s_flash_mutex);
        return false;
    }

    /* Copy first (highest priority) message */
    memcpy(entry, &s_queue[0], sizeof(flash_entry_t));

    /* Shift remaining messages forward */
    for (uint8_t i = 0; i < s_queue_count - 1; i++)
    {
        memcpy(&s_queue[i], &s_queue[i + 1], sizeof(flash_entry_t));
    }

    s_queue_count--;

    xSemaphoreGive(s_flash_mutex);

    ESP_LOGI(TAG, "📋 Dequeued: '%s' (queue_size=%d, pri=%d)",
             entry->line0, s_queue_count, entry->priority);

    return true;
}

bool lcd_flash_queue_has_pending(void)
{
    if (!s_flash_mutex)
        return false;
    xSemaphoreTake(s_flash_mutex, portMAX_DELAY);
    bool result = (s_queue_count > 0);
    xSemaphoreGive(s_flash_mutex);
    return result;
}

/**
 * @brief Get queue size
 */
size_t lcd_flash_queue_size(void)
{
    xSemaphoreTake(s_flash_mutex, portMAX_DELAY);
    size_t size = s_queue_count;
    xSemaphoreGive(s_flash_mutex);

    return size;
}

uint8_t lcd_flash_queue_count(void)
{
    if (!s_flash_mutex)
        return 0;
    xSemaphoreTake(s_flash_mutex, portMAX_DELAY);
    uint8_t c = s_queue_count;
    xSemaphoreGive(s_flash_mutex);
    return c;
}

void lcd_flash_queue_purge_below(flash_priority_t min_priority)
{
    if (!s_flash_mutex)
        return;
    xSemaphoreTake(s_flash_mutex, portMAX_DELAY);

    /*
     * Walk from tail to head, removing entries below min_priority.
     * Walking backwards avoids index invalidation from remove_at().
     */
    for (int i = (int)s_queue_count - 1; i >= 0; i--)
    {
        if (s_queue[i].priority < min_priority)
            remove_at((uint8_t)i);
    }

    xSemaphoreGive(s_flash_mutex);
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