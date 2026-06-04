/*==============================================================================
  lcd_integrity.c
  Screen corruption detection — implementation.
==============================================================================*/
#include "lcd_integrity.h"
#include "lcd.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <stdint.h>

static const char *TAG = "LCD_INTEGRITY";

/*==============================================================================
  CRC-8 (polynomial 0x07, init 0x00, no reflection)
  Simple, fast, good enough for 32-byte payloads.
==============================================================================*/
static uint8_t crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0x00;
    while (len--)
    {
        crc ^= *data++;
        for (uint8_t bit = 0; bit < 8; bit++)
        {
            if (crc & 0x80)
                crc = (uint8_t)((crc << 1) ^ 0x07);
            else
                crc <<= 1;
        }
    }
    return crc;
}

/*==============================================================================
  Internal state
==============================================================================*/
typedef struct
{
    char row0[16];
    char row1[16];
    uint8_t crc; /* CRC-8 of row0 ++ row1 (32 bytes)             */
} screen_snapshot_t;

static screen_snapshot_t s_expected; /* last written content        */
static bool s_has_snapshot = false;
static uint32_t s_last_check_ms = 0;
static uint8_t s_consecutive_mismatches = 0;
static uint32_t s_corruption_count = 0;
static uint32_t s_last_corruption_ms = 0;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/*==============================================================================
  Public API
==============================================================================*/

void lcd_integrity_init(void)
{
    memset(&s_expected, 0, sizeof(s_expected));
    s_has_snapshot = false;
    s_last_check_ms = now_ms();
    s_consecutive_mismatches = 0;
    /* Do NOT reset corruption_count or last_corruption_ms — these are
       lifetime counters that survive reinits. */
}

void lcd_integrity_snapshot(const char *row0, const char *row1)
{
    // Restructure the corruption condition
    /* Store exactly 16 chars per row — space-pad if source is shorter */
    for (int i = 0; i < 16; i++)
    {
        s_expected.row0[i] = (row0 && row0[i]) ? row0[i] : ' ';
        s_expected.row1[i] = (row1 && row1[i]) ? row1[i] : ' ';
    }

    /* Compute CRC over both rows as a single 32-byte block */
    uint8_t block[32];
    memcpy(block, s_expected.row0, 16);
    memcpy(block + 16, s_expected.row1, 16);
    s_expected.crc = crc8(block, 32);

    s_has_snapshot = true;
}

bool lcd_integrity_check(void)
{
    uint32_t ms = now_ms();

    /* Not time yet */
    if ((ms - s_last_check_ms) < LCD_INTEGRITY_CHECK_INTERVAL_MS)
        return true;

    s_last_check_ms = ms;

    /* Nothing written yet — nothing to check */
    if (!s_has_snapshot)
        return true;

#if LCD_INTEGRITY_READ_SUPPORTED
    /*--------------------------------------------------------------------------
      READ-BACK MODE
      Read the current DDRAM content back from the LCD controller and compare
      its CRC against the expected CRC.

      lcd_read_row(row, buf, len) is a driver function you must provide that:
        1. Sends a "set DDRAM address" command for row 0 (0x00) or row 1 (0x40)
        2. Reads `len` bytes of DDRAM data via the PCF8574 RW cycle
        3. Stores results in buf
      If your driver does not support this, set LCD_INTEGRITY_READ_SUPPORTED 0.
    --------------------------------------------------------------------------*/
    char actual_row0[16], actual_row1[16];

    /* lcd_read_row() is provided by your lcd driver — declare it here */
    extern bool lcd_read_row(uint8_t row, char *buf, uint8_t len);

    bool r0_ok = lcd_read_row(0, actual_row0, 16);
    bool r1_ok = lcd_read_row(1, actual_row1, 16);

    if (!r0_ok || !r1_ok)
    {
        /* I2C read failed — bus issue, treat as corruption */
        ESP_LOGW(TAG, "lcd_read_row() failed — I2C bus issue");
        goto corruption_detected;
    }

    uint8_t actual_block[32];
    memcpy(actual_block, actual_row0, 16);
    memcpy(actual_block + 16, actual_row1, 16);
    uint8_t actual_crc = crc8(actual_block, 32);

    if (actual_crc != s_expected.crc)
    {
        ESP_LOGW(TAG,
                 "CRC mismatch: expected 0x%02X, got 0x%02X",
                 s_expected.crc, actual_crc);

        /* Log which characters differ */
        for (int i = 0; i < 16; i++)
        {
            if (actual_row0[i] != s_expected.row0[i])
                ESP_LOGW(TAG, "Row0[%d]: expected 0x%02X got 0x%02X",
                         i, (uint8_t)s_expected.row0[i],
                         (uint8_t)actual_row0[i]);
        }
        for (int i = 0; i < 16; i++)
        {
            if (actual_row1[i] != s_expected.row1[i])
                ESP_LOGW(TAG, "Row1[%d]: expected 0x%02X got 0x%02X",
                         i, (uint8_t)s_expected.row1[i],
                         (uint8_t)actual_row1[i]);
        }
        goto corruption_detected;
    }

    /* CRC matches — all good */
    s_consecutive_mismatches = 0;
    return true;

corruption_detected:
    s_consecutive_mismatches++;
    ESP_LOGW(TAG, "Corruption detected (%d/%d)",
             s_consecutive_mismatches, LCD_INTEGRITY_MISMATCH_THRESHOLD);

    if (s_consecutive_mismatches >= LCD_INTEGRITY_MISMATCH_THRESHOLD)
    {
        s_corruption_count++;
        s_last_corruption_ms = ms;
        s_consecutive_mismatches = 0;

        ESP_LOGE(TAG,
                 "LCD CORRUPTION CONFIRMED — reinit triggered "
                 "(total events: %lu)",
                 (unsigned long)s_corruption_count);

        return false; /* caller must reinit + redraw */
    }

    return true; /* mismatch count below threshold — wait and see */

#else /* LCD_INTEGRITY_READ_SUPPORTED == 0 */
    /*--------------------------------------------------------------------------
      WRITE-ONLY MODE (most common PCF8574 adapters)
      Cannot read back.  Instead we use a periodic forced full redraw.
      We still track the interval and "corruption count" so diagnostics
      show how often reinits occurred.

      Strategy: every check interval, signal lcd_task to force a redraw.
      We do this by returning false, which causes lcd_task to clear and
      redraw the current screen — harmless if the display is fine,
      corrective if it was garbled.

      To avoid unnecessary flicker we only force a redraw every
      LCD_INTEGRITY_CHECK_INTERVAL_MS seconds, not every tick.
    --------------------------------------------------------------------------*/
    ESP_LOGD(TAG, "Periodic forced redraw (write-only mode)");
    /* Count this as a "check", not a "corruption" in write-only mode */
    s_corruption_count++; /* reused as "redraw count" in this mode */
    s_last_corruption_ms = ms;
    s_consecutive_mismatches = 0;
    /* Return false to trigger redraw — lcd_task will reinit + redraw */
    return false;
#endif
}

uint32_t lcd_integrity_corruption_count(void)
{
    return s_corruption_count;
}

uint32_t lcd_integrity_last_corruption_ms(void)
{
    return s_last_corruption_ms;
}