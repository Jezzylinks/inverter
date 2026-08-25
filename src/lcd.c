#include "lcd/lcd.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "stdint.h"

#define TAG "LCD"

// --------------------------------------------------
// STATIC INTERNALS
// --------------------------------------------------
static uint8_t lcd_address;
static lcd_state_t lcd;

// Commands
#define LCD_CMD_CLEAR 0x01
#define LCD_CMD_HOME 0x02
#define LCD_CMD_ENTRY_MODE 0x06
#define LCD_CMD_DISPLAY_ON 0x0C
#define LCD_CMD_FUNCTION_SET 0x28
#define LCD_CMD_SET_DDRAM 0x80

// Backlight control
#define LCD_BACKLIGHT 0x08
#define LCD_NOBACKLIGHT 0x00

// I2C
#define I2C_PORT I2C_NUM_0
#define I2C_FREQ_HZ 100000

esp_err_t lcd_i2c_probe(void)
{
    /* Zero-length write: the I2C driver still clocks out the address
     * byte and checks for ACK/NACK, but no command/data byte follows,
     * so this can't corrupt whatever the display currently shows. */
    return i2c_master_write_to_device(I2C_PORT, lcd_address, NULL, 0,
                                      pdMS_TO_TICKS(100));
}

// Pin latch bits
#define ENABLE_BIT 0x04
#define RS_BIT 0x01

// Special characters
#define CHAR_SELECTED 0x3E // >

/* ── CGRAM bitmaps (5×8, LSB = rightmost pixel) ──────────────── */

/* ── CGRAM slot assignments ───────────────────────────────────── */

static const uint8_t cgram_bar[6][8] = {
    /* BAR_0 — empty */
    {0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},
    /* BAR_1 */
    {0x1F, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1F},
    /* BAR_2 */
    {0x1F, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1F},
    /* BAR_3 */
    {0x1F, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1F},
    /* BAR_4 */
    {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F},
    /* BAR_5 — full (same as BAR_4 visually; differentiated if you prefer) */
    {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F},
};

static const uint8_t cgram_bat_l[8] = {
    /* left cap:  ┌─┐ style top, open body */
    0x07, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07};

static const uint8_t cgram_bat_r[8] = {
    /* right cap + nub on top */
    0x1C, 0x1C, 0x10, 0x10, 0x10, 0x10, 0x1C, 0x1C};


static const uint8_t cgram_wifi_tx[8] = {
    0x00, 0x04, 0x06, 0x1F, 0x06, 0x04, 0x00, 0x00};
static const uint8_t cgram_wifi_rx[8] = {
    0x00, 0x04, 0x0C, 0x1F, 0x0C, 0x04, 0x00, 0x00};
static const uint8_t cgram_wifi_link[8] = {
    0x00, 0x04, 0x0E, 0x1F, 0x0E, 0x04, 0x00, 0x00};
static const uint8_t cgram_wifi_device_local[8] = {
    0x1F, 0x11, 0x15, 0x11, 0x1F, 0x04, 0x0E, 0x04};
static const uint8_t cgram_wifi_device_remote[8] = {
    0x1F, 0x11, 0x1B, 0x11, 0x1F, 0x04, 0x0E, 0x04};

static uint8_t lcd_scan_and_find_address(void);
// --------------------------------------------------
// I2C LOW-LEVEL WRITE (FIXED)
// --------------------------------------------------
static esp_err_t lcd_write_byte(uint8_t data)
{
    uint8_t data_with_backlight =
        data | (lcd.backlight ? LCD_BACKLIGHT : LCD_NOBACKLIGHT);

    return i2c_master_write_to_device(
        I2C_PORT, lcd_address, &data_with_backlight, 1, 100 / portTICK_PERIOD_MS); // Increased timeout
}

static void lcd_pulse_enable(uint8_t data)
{
    lcd_write_byte(data | ENABLE_BIT);
    esp_rom_delay_us(1);
    lcd_write_byte(data & ~ENABLE_BIT);
    esp_rom_delay_us(50);
}

static void lcd_write_nibble(uint8_t nibble, uint8_t mode)
{
    uint8_t data = nibble | mode;
    lcd_pulse_enable(data);
}

static void lcd_send(uint8_t value, uint8_t mode)
{
    lcd_write_nibble(value & 0xF0, mode);
    lcd_write_nibble((value << 4) & 0xF0, mode);
}

// --------------------------------------------------
// BASIC LCD FUNCTIONS
// --------------------------------------------------
void lcd_send_command(uint8_t cmd)
{
    lcd_send(cmd, 0x00);
}

void lcd_print_char(char c)
{
    lcd_send(c, RS_BIT);
}

void lcd_clear(void)
{
    lcd_send_command(LCD_CMD_CLEAR);
    vTaskDelay(pdMS_TO_TICKS(2));
}

void lcd_home(void)
{
    lcd_send_command(LCD_CMD_HOME);
    vTaskDelay(pdMS_TO_TICKS(2));
}

void lcd_set_cursor(uint8_t col, uint8_t row)
{
    static const uint8_t row_offsets[] = {0x00, 0x40, 0x14, 0x54};
    uint8_t rows = lcd_geometry_rows();
    uint8_t cols = lcd_geometry_cols();
    if (row >= rows)
        row = rows - 1U;
    if (col >= cols)
        col = cols - 1U;

    lcd_send_command(LCD_CMD_SET_DDRAM | (col + row_offsets[row]));
}

void lcd_print_str(const char *str)
{
    if (!str)
        return;
    while (*str)
        lcd_print_char(*str++);
}

void lcd_print_string(const char *str)
{
    lcd_print_str(str);
}

void lcd_print_raw(const char *s)
{
    lcd_print_str(s);
}

void lcd_print_int(int value)
{
    char buffer[12];
    snprintf(buffer, sizeof(buffer), "%d", value);
    lcd_print_str(buffer);
}

void lcd_print_float(float v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", v);
    lcd_print_str(buf);
}

void lcd_print_centered(uint8_t row, const char *str)
{
    if (row >= lcd_geometry_rows())
        return;

    int len = strlen(str);
    int pad = (lcd_geometry_cols() - len) / 2;
    if (pad < 0)
        pad = 0;

    lcd_set_cursor(row, pad);
    lcd_print_str(str);
}

void lcd_printf(uint8_t row, uint8_t col, const char *format, ...)
{
    char buffer[32];

    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    lcd_set_cursor(row, col);
    lcd_print_str(buffer);
}

void lcd_backlight(bool on)
{
    lcd.backlight = on ? 1 : 0;
    lcd_write_byte(0); // refresh
}

void lcd_create_custom_char(uint8_t location, const uint8_t charmap[])
{
    location &= 0x7;
    lcd_send_command(0x40 | (location << 3));

    for (int i = 0; i < 8; i++)
        lcd_print_char(charmap[i]);
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

// --------------------------------------------------
// I2C INITIALIZATION
// --------------------------------------------------
esp_err_t lcd_i2c_init(uint8_t sdaPin, uint8_t sclPin)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sdaPin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = sclPin,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ};

    esp_err_t err = i2c_param_config(I2C_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C parameter configuration failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver installation failed: %s",
                 esp_err_to_name(err));
    }
    return err;
}

// --------------------------------------------------
// LCD INITIALIZATION (FIXED - WITH AUTO-SCAN)
// --------------------------------------------------
void lcd_init(uint8_t addr, uint8_t sdaPin, uint8_t sclPin)
{
    // Initialize I2C first
    esp_err_t err = lcd_i2c_init(sdaPin, sclPin);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "LCD I2C initialization failed: %s", esp_err_to_name(err));
        return;
    }

    // If address is 0, auto-scan for LCD
    if (addr == 0)
    {
        ESP_LOGI(TAG, "Auto-scanning for LCD address...");
        addr = lcd_scan_and_find_address();

        if (addr == 0)
        {
            ESP_LOGE(TAG, "Failed to find LCD on I2C bus!");
            return;
        }
    }

    lcd_address = addr;
    lcd.backlight = 1;

    ESP_LOGI(TAG, "Initializing LCD at address 0x%02X", lcd_address);

    // Wait for LCD to power up (IMPORTANT)
    vTaskDelay(pdMS_TO_TICKS(50));

    // FIX 1: Initialize to 8-bit mode first (standard HD44780 init sequence)
    lcd_write_nibble(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(5));

    lcd_write_nibble(0x30, 0);
    esp_rom_delay_us(150); // Short delay

    lcd_write_nibble(0x30, 0);
    esp_rom_delay_us(150);

    // FIX 2: Now switch to 4-bit mode
    lcd_write_nibble(0x20, 0);
    esp_rom_delay_us(150);

    // FIX 3: Configure LCD in 4-bit mode with proper sequence
    lcd_send_command(LCD_CMD_FUNCTION_SET); // 0x28: 4-bit, 2 lines, 5x8 font
    esp_rom_delay_us(50);

    lcd_send_command(0x08); // Display OFF
    esp_rom_delay_us(50);

    lcd_send_command(LCD_CMD_CLEAR); // Clear display
    vTaskDelay(pdMS_TO_TICKS(2));

    lcd_send_command(LCD_CMD_ENTRY_MODE); // 0x06: Entry mode - increment, no shift
    esp_rom_delay_us(50);

    lcd_send_command(LCD_CMD_DISPLAY_ON); // 0x0C: Display ON, cursor OFF, blink OFF
    esp_rom_delay_us(50);

    lcd_send_command(LCD_CMD_HOME); // Return home
    vTaskDelay(pdMS_TO_TICKS(2));

    ESP_LOGI(TAG, "✓ LCD initialized successfully at address 0x%02X", addr);
}

// --------------------------------------------------
// TEXT SCROLLING
// --------------------------------------------------
void lcd_text_scroll(char *str)
{
    size_t len = strlen(str);
    char buffer[LCD_COLS + 1];
    memset(buffer, ' ', LCD_COLS);
    buffer[LCD_COLS] = '\0';

    for (size_t i = 0; i < len + LCD_COLS; i++)
    {
        if (i < len)
        {
            buffer[i % LCD_COLS] = str[i];
        }
        else
        {
            buffer[i % LCD_COLS] = ' ';
        }
        lcd_set_cursor(0, 0);
        lcd_print_str(buffer);
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}

void lcd_write_int(int value, int row, int col, int width)
{
    char buffer[16];

    // Create formatted integer with zero-padding (width digits)
    snprintf(buffer, sizeof(buffer), "%0*d", width, value);

    lcd_set_cursor(row, col);
    lcd_print_raw(buffer);
}

// --------------------------------------------------
// I2C SCANNER (UTILITY)
// --------------------------------------------------
static uint8_t lcd_scan_and_find_address(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus for LCD...");

    // Common LCD I2C addresses to check first
    const uint8_t common_lcd_addresses[] = {0x27, 0x3F, 0x20, 0x38};

    // First, try common LCD addresses
    for (int i = 0; i < sizeof(common_lcd_addresses); i++)
    {
        uint8_t addr = common_lcd_addresses[i];
        uint8_t data = 0x00;
        esp_err_t ret = i2c_master_write_to_device(
            I2C_PORT, addr, &data, 1, 100 / portTICK_PERIOD_MS);

        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "✓ LCD found at address 0x%02X", addr);
            return addr;
        }
    }

    // If not found in common addresses, scan entire bus
    ESP_LOGI(TAG, "Not found at common addresses, scanning entire bus...");

    for (uint8_t addr = 1; addr < 127; addr++)
    {
        uint8_t data = 0x00;
        esp_err_t ret = i2c_master_write_to_device(
            I2C_PORT, addr, &data, 1, 100 / portTICK_PERIOD_MS);

        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "✓ I2C device found at address 0x%02X", addr);
            return addr;
        }

        vTaskDelay(pdMS_TO_TICKS(2)); // Small delay between probes
    }

    ESP_LOGE(TAG, "⚠ No I2C devices found!");
    ESP_LOGE(TAG, "Check:");
    ESP_LOGE(TAG, "  - Wiring (SDA/SCL connections)");
    ESP_LOGE(TAG, "  - Power supply (5V for LCD)");
    ESP_LOGE(TAG, "  - Pull-up resistors (try 4.7kΩ external)");

    return 0; // Return 0 if not found
}

void lcd_scan_i2c_bus(uint8_t sdaPin, uint8_t sclPin)
{
    ESP_LOGI(TAG, "Starting I2C bus scan...");
    ESP_LOGI(TAG, "SDA: GPIO%d, SCL: GPIO%d", sdaPin, sclPin);

    // Initialize I2C if not already done
    lcd_i2c_init(sdaPin, sclPin);

    int devices_found = 0;

    for (uint8_t addr = 1; addr < 127; addr++)
    {
        // Try to write 0x00 to the address
        uint8_t data = 0x00;
        esp_err_t ret = i2c_master_write_to_device(
            I2C_PORT, addr, &data, 1, 100 / portTICK_PERIOD_MS);

        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "✓ I2C device found at address 0x%02X", addr);
            devices_found++;

            // Check if it's a common LCD address
            if (addr == 0x27 || addr == 0x3F || addr == 0x20 || addr == 0x38)
            {
                ESP_LOGI(TAG, "  → This is a common LCD I2C address!");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay between probes
    }

    if (devices_found == 0)
    {
        ESP_LOGW(TAG, "⚠ No I2C devices found!");
        ESP_LOGW(TAG, "Check:");
        ESP_LOGW(TAG, "  - Wiring (SDA/SCL connections)");
        ESP_LOGW(TAG, "  - Power supply (5V for LCD)");
        ESP_LOGW(TAG, "  - Pull-up resistors (try 4.7kΩ external)");
    }
    else
    {
        ESP_LOGI(TAG, "Scan complete. Found %d device(s)", devices_found);
    }
}

// --------------------------------------------------
// LCD TEST PATTERN (DIAGNOSTIC)
// --------------------------------------------------
void lcd_test_pattern(void)
{
    ESP_LOGI(TAG, "Running LCD test pattern...");

    // Test 1: Clear and display simple text
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print_str("LCD Test 1");
    lcd_set_cursor(1, 0);
    lcd_print_str("Hello World!");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Test 2: Display numbers
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print_str("Numbers:");
    lcd_set_cursor(1, 0);
    lcd_print_str("0123456789");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Test 3: Display alphabet
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print_str("ABCDEFGHIJKLMNOP");
    lcd_set_cursor(1, 0);
    lcd_print_str("QRSTUVWXYZ");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Test 4: Fill screen with character
    lcd_clear();
    for (int row = 0; row < LCD_ROWS; row++)
    {
        lcd_set_cursor(row, 0);
        for (int col = 0; col < LCD_COLS; col++)
        {
            lcd_print_char('*');
        }
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Test 5: Backlight toggle
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print_str("Backlight Test");
    for (int i = 0; i < 5; i++)
    {
        lcd_backlight(false);
        vTaskDelay(pdMS_TO_TICKS(300));
        lcd_backlight(true);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print_str("Test Complete!");

    ESP_LOGI(TAG, "LCD test pattern complete");
}

/**
 * @brief  Load all 8 CGRAM slots. Call ONCE at startup before any lcd_draw_*.
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
        for (uint8_t i = 0; i <= 5; i++)
            lcd_create_custom_char(i, cgram_bar[i]);
        lcd_create_custom_char(CHAR_BAT_L, cgram_bat_l);
        lcd_create_custom_char(CHAR_BAT_R, cgram_bat_r);
    }
}
