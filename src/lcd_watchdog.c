/*==============================================================================
  lcd_watchdog.c
  Watchdog and heartbeat implementation for lcd_task.
==============================================================================*/
#include "lcd_watchdog.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

static const char *TAG = "LCD_WDT";

/* ── Internal state ──────────────────────────────────────────────────── */
static TaskHandle_t s_lcd_handle = NULL;
static volatile uint32_t s_heartbeat = 0;    /* incremented by lcd_task  */
static volatile uint32_t s_last_feed_ms = 0; /* ms timestamp of last feed*/
static uint32_t s_last_seen_hb = 0;          /* last heartbeat seen by checker */

/* ── Public API ──────────────────────────────────────────────────────── */

void lcd_watchdog_init(TaskHandle_t lcd_handle)
{
    s_lcd_handle = lcd_handle;
    s_heartbeat = 0;
    s_last_seen_hb = 0;
    s_last_feed_ms = 0;

    if (lcd_handle == NULL)
    {
        ESP_LOGE(TAG, "lcd_watchdog_init: NULL handle — WDT not registered");
        return;
    }

    /* Register lcd_task with the task WDT.
     * ESP-IDF task WDT must have been initialised by the application first
     * (via esp_task_wdt_init()).  If it was not, we log a warning and
     * continue — the heartbeat checker still works independently. */
    esp_err_t err = esp_task_wdt_add(lcd_handle);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "lcd_task registered with task WDT "
                      "(timeout %d ms)",
                 LCD_WDT_TIMEOUT_MS);
    }
    else if (err == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "Task WDT not initialised — "
                      "only heartbeat monitor active");
    }
    else if (err == ESP_ERR_INVALID_ARG)
    {
        ESP_LOGI(TAG, "lcd_task already registered with task WDT");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to register with task WDT: %s",
                 esp_err_to_name(err));
    }
}

void lcd_watchdog_feed(void)
{
    /* Feed the ESP task WDT */
    esp_task_wdt_reset();

    /* Update heartbeat and timestamp */
    s_heartbeat++;
    s_last_feed_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

bool lcd_watchdog_check(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t last_ms = s_last_feed_ms;

    /* First call before lcd_task has started — not a stall */
    if (last_ms == 0)
        return true;

    uint32_t elapsed = now_ms - last_ms;

    if (elapsed > LCD_HEARTBEAT_TIMEOUT_MS)
    {
        ESP_LOGE(TAG,
                 "lcd_task STALLED — no heartbeat for %lu ms "
                 "(threshold %d ms). Last heartbeat: %lu",
                 (unsigned long)elapsed,
                 LCD_HEARTBEAT_TIMEOUT_MS,
                 (unsigned long)s_heartbeat);

        /* Log task stack high-water mark if handle is valid */
        if (s_lcd_handle != NULL)
        {
            UBaseType_t stack_left =
                uxTaskGetStackHighWaterMark(s_lcd_handle);
            ESP_LOGE(TAG, "lcd_task stack remaining: %u words", stack_left);

            eTaskState state = eTaskGetState(s_lcd_handle);
            ESP_LOGE(TAG, "lcd_task state: %d", (int)state);
        }

        return false; /* caller should trigger restart */
    }

    /* Task is alive — update last-seen heartbeat */
    s_last_seen_hb = s_heartbeat;
    return true;
}

uint32_t lcd_watchdog_get_heartbeat(void)
{
    return s_heartbeat;
}

uint32_t lcd_watchdog_last_feed_ms(void)
{
    return s_last_feed_ms;
}

void lcd_watchdog_deinit(void)
{
    if (s_lcd_handle != NULL)
    {
        esp_err_t err = esp_task_wdt_delete(s_lcd_handle);
        if (err == ESP_OK)
            ESP_LOGI(TAG, "lcd_task deregistered from task WDT");
        else
            ESP_LOGW(TAG, "WDT deregister failed: %s", esp_err_to_name(err));
        s_lcd_handle = NULL;
    }
}