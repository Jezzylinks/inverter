#pragma once
/*==============================================================================
  lcd_integrity.h
  Screen corruption detection for the 16x2 I2C LCD.

  Problem:
    I2C LCD controllers (PCF8574 + HD44780) can silently garble their
    display after:
      - Power glitches / brown-outs
      - I2C bus noise (long cable, inductive load switching nearby)
      - ESP32 deep sleep / wake cycles
      - Watchdog resets that leave I2C mid-transaction

  Solution:
    lcd_task maintains a CRC-8 of the last two 16-char rows it wrote.
    Every LCD_INTEGRITY_CHECK_INTERVAL_MS it re-reads those rows back
    via I2C and computes their CRC. A mismatch triggers a full
    lcd_init() + redraw of the current screen.

  How re-read works:
    The HD44780 supports a read cycle (RS=0, RW=1) that returns the
    DDRAM address counter and busy flag, and (RS=1, RW=1) that returns
    DDRAM data. The PCF8574 adapter wires RW to a GPIO so this is
    possible without extra hardware — we just set RW high during read.
    If your particular adapter hard-wires RW low (write-only), define
    LCD_INTEGRITY_READ_SUPPORTED 0 and the module falls back to a
    periodic full redraw instead.
==============================================================================*/
#include <stdint.h>
#include <stdbool.h>

/* ── Configuration ───────────────────────────────────────────────────── */

/* How often to run the integrity check (milliseconds). */
#define LCD_INTEGRITY_CHECK_INTERVAL_MS 30000 /* every 30 s               */

/* Set to 1 if your PCF8574 adapter supports RW reads, 0 if write-only.
   Write-only mode: integrity check triggers a forced full redraw instead
   of comparing read-back data against the expected CRC.               */
#ifndef LCD_INTEGRITY_READ_SUPPORTED
#define LCD_INTEGRITY_READ_SUPPORTED 0 /* safe default             */
#endif

/* Number of consecutive CRC mismatches before triggering a reinit.
   1 means reinit on the very first mismatch — safe but may cause a brief
   flicker on noisy busses. 2–3 is more tolerant. */
#define LCD_INTEGRITY_MISMATCH_THRESHOLD 2

/* ── Public API ──────────────────────────────────────────────────────── */

/*
 * Call once from lcd_task after lcd_init().
 * Resets all internal state.
 */
void lcd_integrity_init(void);

/*
 * Call from lcd_task AFTER every successful screen draw.
 * Snapshots the two rows that were just written so the checker
 * has a reference to compare against.
 *
 * @param row0  16-char string that was written to row 0 (need not be NUL-terminated)
 * @param row1  16-char string that was written to row 1
 */
void lcd_integrity_snapshot(const char *row0, const char *row1);

/*
 * Call from lcd_task on every tick (even when no draw happened).
 * Returns true  — display looks correct (or check not due yet).
 * Returns false — corruption detected; caller must reinit + redraw.
 *
 * When it returns false it has already logged the event via ESP_LOGW.
 * The caller (lcd_task) should:
 *   1. Call lcd_init(LCD_ADDR, SDA_PIN, SCL_PIN)
 *   2. Set last_screen = LCD_SCREEN_COUNT  (force full redraw next tick)
 *   3. Call lcd_integrity_init()            (reset counters)
 */
bool lcd_integrity_check(void);

/*
 * Returns the total number of corruption events detected since boot.
 * Shown on the diagnostics screen under "System Health".
 */
uint32_t lcd_integrity_corruption_count(void);

/*
 * Returns the timestamp (ms since boot) of the last detected corruption.
 * 0 if no corruption has occurred.
 */
uint32_t lcd_integrity_last_corruption_ms(void);