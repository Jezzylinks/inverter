#pragma once
/*==============================================================================
  lcd_watchdog.h
  Watchdog integration and heartbeat monitoring for lcd_task.

  The shared task_watchdog module owns ESP-IDF TWDT registration and feeding.
  This module owns only a heartbeat counter — any task can call
  lcd_watchdog_check() to verify
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

/* How long before the heartbeat checker declares lcd_task stalled.
   The global ESP-IDF TWDT policy is configured by task_watchdog.c. */
#define LCD_HEARTBEAT_TIMEOUT_MS 5000

/* ── Public API ──────────────────────────────────────────────────────── */

/*
 * Call from lcd_task immediately after shared task_watchdog registration.
 * This initializes only the LCD heartbeat state.
 */
void lcd_watchdog_init(TaskHandle_t lcd_handle);

/*
 * Call from lcd_task at the TOP of every loop iteration (before drawing).
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
 * Clear LCD heartbeat state before deleting lcd_task.
 */
void lcd_watchdog_deinit(void);
