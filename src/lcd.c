/**
 * @file lcd.c
 * @brief HD44780 LCD driver via PCF8574T I2C backpack (HW-061) — ESP-IDF
 *
 * Key fixes over previous revision
 * ─────────────────────────────────
 * 1. Timing — pulse width raised to 1 ms (conservative); all HD44780
 *    "busy" waits replaced with documented minimums per datasheet §6.
 * 2. Init sequence — follows the exact flow from HD44780 datasheet
 *    Figure 24 (4-bit interface initialisation by instruction).
 * 3. Nibble masking — upper nibble is now isolated before OR-ing with
 *    control bits so RS/RW/E lines can never be corrupted by data bits.
 * 4. I2C address scan — covers both PCF8574 (0x20-0x27) and
 *    PCF8574A (0x38-0x3F) ranges.
 * 5. CGRAM cursor restore — after every CGRAM write sequence the driver
 *    issues SET_DDRAM 0x00 so subsequent prints land on the screen.
 * 6. lcd_text_scroll — rewritten; uses a properly managed window buffer
 *    with no in-place mutation of the caller's string.
 */

#include "lcd/lcd.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

#define TAG "LCD"

/* ── HD44780 command set ──────────────────────────────────────────────── */
#define LCD_CMD_CLEAR 0x01
#define LCD_CMD_HOME 0x02
#define LCD_CMD_ENTRY_MODE 0x06 /* increment, no shift               */
#define LCD_CMD_DISPLAY_ON 0x0C /* display on, cursor off, blink off */
#define LCD_CMD_DISPLAY_OFF 0x08
#define LCD_CMD_FUNCTION_SET 0x28 /* 4-bit, 2-line, 5×8               */
#define LCD_CMD_SET_DDRAM 0x80
#define LCD_CMD_SET_CGRAM 0x40

/* ── PCF8574 I2C backpack bit layout ─────────────────────────────────── *
 *  Bit 7  6  5  4  3   2   1  0
 *       D7 D6 D5 D4 BL  EN  RW RS
 */
#define BIT_RS 0x01
#define BIT_RW 0x02
#define BIT_EN 0x04
#define BIT_BACKLIGHT 0x08

/* ── I2C transport ───────────────────────────────────────────────────── */
#define I2C_PORT I2C_NUM_0
#define I2C_FREQ_HZ 100000 /* 100 kHz — safe for long wires     */
#define I2C_TIMEOUT_MS 50

/* ── HD44780 timing constants (µs) ──────────────────────────────────── *
 *  These are the MINIMUM values from the datasheet.  We add margin.
 *  The enable pulse width (Pw) must be ≥ 450 ns; we use 500 ns.
 *  After falling edge of E the display needs ≥ 37 µs to execute most
 *  commands; we use 50 µs.  CLEAR and HOME need ≥ 1.52 ms; we use 2 ms.
 */
#define T_EN_PULSE_US 1   /* enable pulse high time, ≥ 450 ns  */
#define T_EN_SETTLE_US 50 /* post-enable execution time         */
#define T_SLOW_CMD_MS 2   /* CLEAR / HOME execution time        */

/* ── Module state ────────────────────────────────────────────────────── */
static uint8_t s_addr;
static lcd_state_t lcd;
static bool s_initialized = false;

/* ── Forward declarations ────────────────────────────────────────────── */
static esp_err_t i2c_write_raw(uint8_t byte);
static esp_err_t pulse_enable(uint8_t byte);
static esp_err_t send_nibble(uint8_t nibble_high, uint8_t mode);
static esp_err_t send_byte(uint8_t value, uint8_t mode);
static uint8_t scan_find_address(void);

/* ══════════════════════════════════════════════════════════════════════
 *  I2C LOW-LEVEL
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Write one raw byte to the PCF8574.  Backlight bit is always merged in
 * from the current backlight state so it is never accidentally cleared.
 */
static esp_err_t i2c_write_raw(uint8_t byte)
{
    byte |= lcd.backlight ? BIT_BACKLIGHT : 0x00;
    return i2c_master_write_to_device(
        I2C_PORT, s_addr, &byte, 1,
        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

/**
 * Generate a single enable pulse.
 *
 * The byte passed in already contains the data nibble (bits 7-4) and the
 * RS/RW flags.  We OR in BIT_EN to raise the clock, delay, then clear it.
 */
static esp_err_t pulse_enable(uint8_t byte)
{
    esp_err_t err;

    /* E HIGH */
    err = i2c_write_raw(byte | BIT_EN);
    if (err != ESP_OK)
        return err;

    esp_rom_delay_us(T_EN_PULSE_US);

    /* E LOW */
    err = i2c_write_raw(byte & (uint8_t)(~BIT_EN));
    if (err != ESP_OK)
        return err;

    esp_rom_delay_us(T_EN_SETTLE_US);
    return ESP_OK;
}

/**
 * Send one nibble.
 *
 * @param nibble_high  The nibble already shifted into bits 7-4 (e.g. 0xA0).
 * @param mode         BIT_RS for data, 0x00 for command.
 *
 * IMPORTANT: we mask out the lower 4 bits so that the data nibble can
 * never accidentally set RS/RW/EN.  Only the upper 4 bits carry data;
 * the lower 4 are reserved for control.
 */
static esp_err_t send_nibble(uint8_t nibble_high, uint8_t mode)
{
    /* Keep only bits 7-4 from the data, keep only bits 1-0 from mode */
    uint8_t byte = (nibble_high & 0xF0) | (mode & 0x03);
    return pulse_enable(byte);
}

/**
 * Send a full byte (two nibbles, high nibble first).
 *
 * @param mode  BIT_RS → character data; 0 → command.
 */
static esp_err_t send_byte(uint8_t value, uint8_t mode)
{
    esp_err_t err;
    err = send_nibble(value & 0xF0, mode);
    if (err != ESP_OK)
        return err;
    return send_nibble((value << 4) & 0xF0, mode);
}

/* ══════════════════════════════════════════════════════════════════════
 *  ADDRESS SCANNER
 *  Covers both PCF8574 (0x20-0x27) and PCF8574A (0x38-0x3F).
 * ══════════════════════════════════════════════════════════════════════ */

static uint8_t scan_find_address(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus for PCF8574T LCD backpack ...");

    /*
     * PCF8574T (your chip) lives at 0x20-0x27.
     * PCF8574AT variant lives at 0x38-0x3F.
     *
     * IMPORTANT: we send a real 1-byte payload (0x00 = all outputs LOW)
     * instead of a NULL/zero-length probe.  Older ESP-IDF I2C drivers
     * reject NULL data pointers with "i2c null address error" even though
     * the intent is just an ACK check.  Sending 0x00 is safe: it drives
     * all PCF8574 outputs LOW, which is the latch-clear we want anyway.
     */
    static const struct
    {
        uint8_t lo;
        uint8_t hi;
        const char *name;
    } ranges[] = {
        {0x20, 0x27, "PCF8574T"}, /* your chip */
        {0x38, 0x3F, "PCF8574AT"},
    };

    uint8_t probe_byte = 0x00;

    for (size_t r = 0; r < sizeof(ranges) / sizeof(ranges[0]); ++r)
    {
        ESP_LOGI(TAG, "Probing %s range 0x%02X-0x%02X ...",
                 ranges[r].name, ranges[r].lo, ranges[r].hi);

        for (uint8_t addr = ranges[r].lo; addr <= ranges[r].hi; ++addr)
        {
            esp_err_t ret = i2c_master_write_to_device(
                I2C_PORT, addr, &probe_byte, 1, pdMS_TO_TICKS(20));

            ESP_LOGI(TAG, "  0x%02X -> %s", addr,
                     ret == ESP_OK ? "ACK (found!)" : esp_err_to_name(ret));

            if (ret == ESP_OK)
            {
                ESP_LOGI(TAG, "Found %s at 0x%02X", ranges[r].name, addr);
                return addr;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    ESP_LOGE(TAG, "No PCF8574T found — all addresses returned NACK.");
    ESP_LOGE(TAG, "Hardware checklist:");
    ESP_LOGE(TAG, "  1. SDA and SCL wired to correct GPIO pins?");
    ESP_LOGE(TAG, "  2. Backpack VCC = 5 V (not 3.3 V)?");
    ESP_LOGE(TAG, "  3. Common GND between ESP32 and backpack?");
    ESP_LOGE(TAG, "  4. Pull-ups on SDA/SCL (4.7 kohm to 3.3 V)?");
    ESP_LOGE(TAG, "  5. A0/A1/A2: all open = 0x27, all shorted = 0x20");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  I2C BUS INITIALISATION
 * ══════════════════════════════════════════════════════════════════════ */

esp_err_t lcd_i2c_init(uint8_t sdaPin, uint8_t sclPin)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sdaPin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = sclPin,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };

    esp_err_t err = i2c_param_config(I2C_PORT, &cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2c_param_config: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        /* ESP_ERR_INVALID_STATE means the driver is already installed — OK */
        ESP_LOGE(TAG, "i2c_driver_install: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 *  LCD HARDWARE INITIALISATION
 *
 *  Follows HD44780 datasheet Figure 24:
 *    1. Power-on delay ≥ 40 ms
 *    2. Write 0x03 nibble (8-bit reset #1), wait ≥ 4.1 ms
 *    3. Write 0x03 nibble (8-bit reset #2), wait ≥ 100 µs
 *    4. Write 0x03 nibble (8-bit reset #3), wait ≥ 100 µs
 *    5. Write 0x02 nibble → switch to 4-bit mode
 *    6. Configure: function set, display off, clear, entry mode, display on
 * ══════════════════════════════════════════════════════════════════════ */

esp_err_t lcd_init(uint8_t addr, uint8_t sdaPin, uint8_t sclPin)
{
    s_initialized = false;
    lcd.backlight = 1;

    esp_err_t err = lcd_i2c_init(sdaPin, sclPin);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return err;

    if (addr == 0)
    {
        addr = scan_find_address();
        if (addr == 0)
            return ESP_ERR_NOT_FOUND;
    }

    s_addr = addr;
    ESP_LOGI(TAG, "LCD init: addr=0x%02X SDA=%u SCL=%u",
             s_addr, (unsigned)sdaPin, (unsigned)sclPin);

    /* ── Step 0: PCF8574T latch clear ──────────────────────────────── *
     *  The PCF8574T powers on with all outputs HIGH (0xFF).  If we go
     *  straight into the reset nibble sequence the HD44780 sees EN=1
     *  on the very first I2C transaction, which it interprets as a
     *  spurious strobe and enters a random state before we even begin.
     *  Drive every output LOW now so the display sees a clean baseline.
     */
    {
        uint8_t zero = 0x00;
        (void)i2c_master_write_to_device(
            I2C_PORT, s_addr, &zero, 1, pdMS_TO_TICKS(20));
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    /* ── Step 1: Power-on stabilisation ────────────────────────────── */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* ── Steps 2-4: 8-bit reset sequence ───────────────────────────── *
     *  The display is in an unknown state; these three identical nibble
     *  writes guarantee it re-enters a known 8-bit mode on the third one.
     *  We use send_nibble() rather than pulse_enable() directly so the
     *  backlight bit is merged correctly.
     *
     *  0x30 in 4-bit physical terms:
     *    Bits 7-4 = 0x3 → DB7-DB4 = 0,0,1,1   → function set 8-bit
     */
    err = send_nibble(0x30, 0); /* reset #1 */
    if (err != ESP_OK)
        goto init_fail;
    vTaskDelay(pdMS_TO_TICKS(5)); /* ≥ 4.1 ms */

    err = send_nibble(0x30, 0); /* reset #2 */
    if (err != ESP_OK)
        goto init_fail;
    esp_rom_delay_us(200); /* ≥ 100 µs */

    err = send_nibble(0x30, 0); /* reset #3 */
    if (err != ESP_OK)
        goto init_fail;
    esp_rom_delay_us(200);

    /* ── Step 5: Switch to 4-bit interface ─────────────────────────── */
    err = send_nibble(0x20, 0); /* 0010 → 4-bit mode select       */
    if (err != ESP_OK)
        goto init_fail;
    esp_rom_delay_us(200);

    /* ── Step 6: Configuration commands ────────────────────────────── *
     *  Every send_byte() call issues two nibbles; full 4-bit transfers.
     */

    /* Function set: 4-bit bus, 2 display lines, 5×8 font */
    err = send_byte(LCD_CMD_FUNCTION_SET, 0);
    if (err != ESP_OK)
        goto init_fail;
    esp_rom_delay_us(T_EN_SETTLE_US);

    /* Display off (required step in the datasheet flow) */
    err = send_byte(LCD_CMD_DISPLAY_OFF, 0);
    if (err != ESP_OK)
        goto init_fail;
    esp_rom_delay_us(T_EN_SETTLE_US);

    /* Display clear — slow command, needs full wait */
    err = send_byte(LCD_CMD_CLEAR, 0);
    if (err != ESP_OK)
        goto init_fail;
    vTaskDelay(pdMS_TO_TICKS(T_SLOW_CMD_MS));

    /* Entry mode: cursor moves right, display does not shift */
    err = send_byte(LCD_CMD_ENTRY_MODE, 0);
    if (err != ESP_OK)
        goto init_fail;
    esp_rom_delay_us(T_EN_SETTLE_US);

    /* Display on: display on, cursor off, blink off */
    err = send_byte(LCD_CMD_DISPLAY_ON, 0);
    if (err != ESP_OK)
        goto init_fail;
    esp_rom_delay_us(T_EN_SETTLE_US);

    s_initialized = true;
    ESP_LOGI(TAG, "LCD ready at 0x%02X", s_addr);
    return ESP_OK;

init_fail:
    ESP_LOGE(TAG, "LCD init failed: %s", esp_err_to_name(err));
    return err;
}

bool lcd_is_initialized(void) { return s_initialized; }

/* ══════════════════════════════════════════════════════════════════════
 *  PROBE (non-destructive ACK check)
 * ══════════════════════════════════════════════════════════════════════ */

esp_err_t lcd_i2c_probe(void)
{
    /* Send 0x00 rather than NULL — older ESP-IDF rejects NULL data ptr */
    uint8_t probe = 0x00;
    return i2c_master_write_to_device(
        I2C_PORT, s_addr, &probe, 1, pdMS_TO_TICKS(100));
}

/* ══════════════════════════════════════════════════════════════════════
 *  BASIC LCD API
 * ══════════════════════════════════════════════════════════════════════ */

void lcd_send_command(uint8_t cmd)
{
    (void)send_byte(cmd, 0x00);
}

void lcd_print_char(char c)
{
    (void)send_byte((uint8_t)c, BIT_RS);
}

void lcd_clear(void)
{
    lcd_send_command(LCD_CMD_CLEAR);
    vTaskDelay(pdMS_TO_TICKS(T_SLOW_CMD_MS));
}

void lcd_home(void)
{
    lcd_send_command(LCD_CMD_HOME);
    vTaskDelay(pdMS_TO_TICKS(T_SLOW_CMD_MS));
}

void lcd_set_cursor(uint8_t row, uint8_t col)
{
    /* DDRAM addresses for a 4-line display:
     *   Row 0: 0x00 … 0x27
     *   Row 1: 0x40 … 0x67
     *   Row 2: 0x14 … 0x3B  (20-column displays only)
     *   Row 3: 0x54 … 0x7B  (20-column displays only)
     */
    static const uint8_t row_offsets[] = {0x00, 0x40, 0x14, 0x54};

    uint8_t rows = lcd_geometry_rows();
    uint8_t cols = lcd_geometry_cols();
    if (row >= rows)
        row = rows - 1;
    if (col >= cols)
        col = cols - 1;

    lcd_send_command(LCD_CMD_SET_DDRAM | (col + row_offsets[row]));
}

void lcd_print_str(const char *str)
{
    if (!str)
        return;
    while (*str)
        lcd_print_char(*str++);
}

/* Aliases kept for API compatibility */
void lcd_print_string(const char *str) { lcd_print_str(str); }
void lcd_print_raw(const char *str) { lcd_print_str(str); }

void lcd_print_int(int value)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", value);
    lcd_print_str(buf);
}

void lcd_print_float(float v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", v);
    lcd_print_str(buf);
}

void lcd_print_centered(uint8_t row, const char *str)
{
    if (!str || row >= lcd_geometry_rows())
        return;

    int len = (int)strlen(str);
    int pad = ((int)lcd_geometry_cols() - len) / 2;
    if (pad < 0)
        pad = 0;

    lcd_set_cursor(row, (uint8_t)pad);
    lcd_print_str(str);
}

void lcd_printf(uint8_t row, uint8_t col, const char *fmt, ...)
{
    char buf[33]; /* max 20 chars for 20-col, pad to 32   */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    lcd_set_cursor(row, col);
    lcd_print_str(buf);
}

void lcd_write_int(int value, int row, int col, int width)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%0*d", width, value);
    lcd_set_cursor((uint8_t)row, (uint8_t)col);
    lcd_print_str(buf);
}

void lcd_backlight(bool on)
{
    lcd.backlight = on ? 1 : 0;
    /* A zero-data write refreshes the backlight bit on the PCF8574 */
    (void)i2c_write_raw(0x00);
}

void lcd_show_message(const char *line1, const char *line2)
{
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print_str(line1);
    if (line2)
    {
        lcd_set_cursor(1, 0);
        lcd_print_str(line2);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  CUSTOM CHARACTER (CGRAM)
 *
 *  FIX: after loading all CGRAM bitmaps we MUST return to DDRAM address 0
 *  (or wherever the next print should go).  Without this the HD44780
 *  cursor still points into CGRAM and the first lcd_print_* after
 *  lcd_init_cgram() silently overwrites the custom bitmaps instead of
 *  drawing on screen.
 * ══════════════════════════════════════════════════════════════════════ */

void lcd_create_custom_char(uint8_t slot, const uint8_t bitmap[8])
{
    slot &= 0x07; /* only 8 slots: 0-7         */
    lcd_send_command(LCD_CMD_SET_CGRAM | (slot << 3));
    for (int i = 0; i < 8; i++)
        lcd_print_char(bitmap[i]);
    /* Return to DDRAM so the caller can print immediately */
    lcd_send_command(LCD_CMD_SET_DDRAM | 0x00);
}

/* ── CGRAM bitmaps ───────────────────────────────────────────────────── */

static const uint8_t cgram_bar[6][8] = {
    /* BAR_0 — empty frame */
    {0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},
    /* BAR_1 — 20 % filled */
    {0x1F, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1F},
    /* BAR_2 — 40 % filled */
    {0x1F, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1F},
    /* BAR_3 — 60 % filled */
    {0x1F, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1F},
    /* BAR_4 — 80 % filled */
    {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F},
    /* BAR_5 — 100 % / solid */
    {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F},
};

static const uint8_t cgram_bat_l[8] = {
    0x07, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07};

static const uint8_t cgram_bat_r[8] = {
    0x1C, 0x1C, 0x10, 0x10, 0x10, 0x10, 0x1C, 0x1C};

static const uint8_t cgram_wifi_tx[8] = {0x00, 0x04, 0x06, 0x1F, 0x06, 0x04, 0x00, 0x00};
static const uint8_t cgram_wifi_rx[8] = {0x00, 0x04, 0x0C, 0x1F, 0x0C, 0x04, 0x00, 0x00};
static const uint8_t cgram_wifi_link[8] = {0x00, 0x04, 0x0E, 0x1F, 0x0E, 0x04, 0x00, 0x00};
static const uint8_t cgram_wifi_device_local[8] = {0x1F, 0x11, 0x15, 0x11, 0x1F, 0x04, 0x0E, 0x04};
static const uint8_t cgram_wifi_device_remote[8] = {0x1F, 0x11, 0x1B, 0x11, 0x1F, 0x04, 0x0E, 0x04};

/**
 * @brief Load all CGRAM slots.  Call once at startup, after lcd_init().
 *        Each individual lcd_create_custom_char() call already restores
 *        DDRAM, so there is no ordering issue between slots.
 */
void lcd_init_cgram(void)
{
    if (lcd_geometry_is_20x4())
    {
        lcd_create_custom_char(CHAR_WIFI_TX, cgram_wifi_tx);
        lcd_create_custom_char(CHAR_WIFI_RX, cgram_wifi_rx);
        lcd_create_custom_char(CHAR_WIFI_LINK, cgram_wifi_link);
        lcd_create_custom_char(CHAR_WIFI_DEVICE_LOCAL, cgram_wifi_device_local);
        lcd_create_custom_char(CHAR_WIFI_DEVICE_REMOTE, cgram_wifi_device_remote);
        lcd_create_custom_char(CHAR_BAR_0, cgram_bar[0]);
        lcd_create_custom_char(CHAR_BAR_1, cgram_bar[1]);
        lcd_create_custom_char(CHAR_BAR_2, cgram_bar[2]);
    }
    else
    {
        for (uint8_t i = 0; i < 6; i++)
            lcd_create_custom_char(i, cgram_bar[i]);
        lcd_create_custom_char(CHAR_BAT_L, cgram_bat_l);
        lcd_create_custom_char(CHAR_BAT_R, cgram_bat_r);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEXT SCROLLING
 *
 *  FIX: does not mutate the caller's string.  Uses a separate window
 *  buffer that slides a view across padding + content + padding.
 * ══════════════════════════════════════════════════════════════════════ */

void lcd_text_scroll(const char *str)
{
    if (!str)
        return;

    uint8_t cols = lcd_geometry_cols();
    size_t slen = strlen(str);
    /* Total virtual length: blank padding on each side so the text
     * slides fully onto and off the display.                        */
    size_t total = slen + (size_t)(cols * 2);
    char win[cols + 1];
    win[cols] = '\0';

    for (size_t offset = 0; offset < total - cols; offset++)
    {
        for (uint8_t c = 0; c < cols; c++)
        {
            size_t src = offset + c;
            if (src < (size_t)cols || src >= (size_t)cols + slen)
                win[c] = ' '; /* left or right padding region */
            else
                win[c] = str[src - cols];
        }

        lcd_set_cursor(0, 0);
        lcd_print_str(win);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  FULL I2C BUS SCAN (diagnostic utility)
 * ══════════════════════════════════════════════════════════════════════ */

void lcd_scan_i2c_bus(uint8_t sdaPin, uint8_t sclPin)
{
    ESP_LOGI(TAG, "I2C bus scan — SDA:GPIO%d SCL:GPIO%d", sdaPin, sclPin);
    lcd_i2c_init(sdaPin, sclPin);

    int found = 0;

    uint8_t probe = 0x00;
    for (uint8_t addr = 1; addr < 127; addr++)
    {
        esp_err_t ret = i2c_master_write_to_device(
            I2C_PORT, addr, &probe, 1, pdMS_TO_TICKS(20));

        if (ret == ESP_OK)
        {
            const char *hint = "";
            if (addr >= 0x20 && addr <= 0x27)
                hint = "  ← PCF8574 / PCF8574T LCD";
            if (addr >= 0x38 && addr <= 0x3F)
                hint = "  ← PCF8574A LCD";
            ESP_LOGI(TAG, "  0x%02X%s", addr, hint);
            found++;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (found == 0)
        ESP_LOGW(TAG, "No I2C devices found — check wiring and pull-ups.");
    else
        ESP_LOGI(TAG, "Scan complete: %d device(s) found.", found);
}

/* ══════════════════════════════════════════════════════════════════════
 *  DIAGNOSTIC TEST PATTERN
 * ══════════════════════════════════════════════════════════════════════ */

void lcd_test_pattern(void)
{
    ESP_LOGI(TAG, "LCD test pattern start");

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print_str("LCD Test OK");
    lcd_set_cursor(1, 0);
    lcd_print_str("Hello World!");
    vTaskDelay(pdMS_TO_TICKS(2000));

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print_str("0123456789");
    lcd_set_cursor(1, 0);
    lcd_print_str("ABCDEFGHIJ");
    vTaskDelay(pdMS_TO_TICKS(2000));

    lcd_clear();
    uint8_t rows = lcd_geometry_rows();
    uint8_t cols = lcd_geometry_cols();
    for (uint8_t r = 0; r < rows; r++)
    {
        lcd_set_cursor(r, 0);
        for (uint8_t c = 0; c < cols; c++)
            lcd_print_char('*');
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print_str("Backlight test");
    for (int i = 0; i < 4; i++)
    {
        lcd_backlight(false);
        vTaskDelay(pdMS_TO_TICKS(400));
        lcd_backlight(true);
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print_str("Test complete");
    ESP_LOGI(TAG, "LCD test pattern done");
}