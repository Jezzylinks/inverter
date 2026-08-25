#pragma once
/*==============================================================================
  lcd_watchdog.h
  Watchdog integration and heartbeat monitoring for lcd_task.

  Two layers of protection:
  1. ESP Task WDT  — lcd_task feeds it every tick. If lcd_task hangs for
                     more than LCD_WDT_TIMEOUT_MS, the WDT fires a panic
                     and the system restarts.

  2. Heartbeat counter — any task can call lcd_watchdog_check() to verify
                         lcd_task is still alive. If the heartbeat has not
                         incremented within LCD_HEARTBEAT_TIMEOUT_MS, the
                         checker logs the stall and triggers a soft restart.
                         Useful for detecting a task that is alive but stuck
                         drawing the same frame (e.g. I2C bus lock-up).
==============================================================================*/
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Configuration ───────────────────────────────────────────────────── */

/* How long the task WDT waits before firing a panic (milliseconds).
   Must be longer than the worst-case draw time (a full lcd_clear +
   two lcd_print calls over I2C at 100 kHz takes ~15 ms max).
   Set conservatively at 3 s. */
#define LCD_WDT_TIMEOUT_MS 3000

/* How long before the heartbeat checker declares lcd_task stalled.
   Set longer than LCD_WDT_TIMEOUT_MS so the WDT fires first on a real hang,
   and the heartbeat check catches slower degradation (e.g. I2C semi-lockup
   where the task still runs but draws take 500 ms each). */
#define LCD_HEARTBEAT_TIMEOUT_MS 5000

/* ── Public API ──────────────────────────────────────────────────────── */

/*
 * Call once from app_main BEFORE xTaskCreate(lcd_task).
 * Registers lcd_task with the task WDT and resets the heartbeat counter.
 */
void lcd_watchdog_init(TaskHandle_t lcd_handle);

/*
 * Call from lcd_task at the TOP of every loop iteration (before drawing).
 * - Feeds the ESP task WDT so it does not fire.
 * - Increments the heartbeat counter so external checkers know we are alive.
 */
void lcd_watchdog_feed(void);

/*
 * Call from any monitoring task (e.g. the main loop or diagnostic task).
 * Returns true  — lcd_task is alive and feeding on time.
 * Returns false — lcd_task appears stalled; caller should restart system.
 *
 * When it returns false it also logs the stall via ESP_LOGE.
 */
bool lcd_watchdog_check(void);

/*
 * Returns the raw heartbeat tick count.
 * Useful for diagnostics (e.g. display on the diagnostic screen).
 */
uint32_t lcd_watchdog_get_heartbeat(void);

/*
 * Returns the timestamp (ms since boot) of the last successful feed.
 */
uint32_t lcd_watchdog_last_feed_ms(void);

/*
 * Deregister from task WDT — call before deleting lcd_task (e.g. on
 * factory reset or controlled shutdown).
 */
void lcd_watchdog_deinit(void);