#ifndef LCD_H
#define LCD_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lcd/lcd_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

// --------------------------------------------------
// LCD CONFIGURATION
// --------------------------------------------------
#define CHAR_BAR_0 0 /* empty / legacy bar  */
#define CHAR_BAR_1 1
#define CHAR_BAR_2 2 /* legacy slot; also used for the filled progress block */
#define CHAR_PROGRESS_BLOCK 2
#define CHAR_BAR_3 3
#define CHAR_BAR_4 4
#define CHAR_BAR_5 5 /* full / legacy bar   */
#define CHAR_BAT_L 6 /* battery left bracket */
#define CHAR_BAT_R 7 /* battery + nub        */

/* 20×4 activity glyphs. The renderer may use these in place of text arrows. */
#define CHAR_WIFI_TX 3
#define CHAR_WIFI_RX 4
#define CHAR_WIFI_LINK 5
#define CHAR_WIFI_LOCK 6
#define CHAR_WIFI_ALERT 7
#define CHAR_WIFI_DEVICE_LOCAL 6
#define CHAR_WIFI_DEVICE_REMOTE 7

    // --------------------------------------------------
    // LCD STATE STRUCTURE
    // --------------------------------------------------
    typedef struct
    {
        uint8_t backlight;
    } lcd_state_t;

    // --------------------------------------------------
    // BASIC LCD FUNCTIONS
    // --------------------------------------------------

    /**
     * @brief Initialize the LCD with I2C
     * @param addr I2C address of LCD; pass 0 to scan automatically.
     *        A PCF8574T uses 0x20-0x27; PCF8574A uses 0x38-0x3F.
     * @param sdaPin GPIO pin for SDA
     * @param sclPin GPIO pin for SCL
     */
    esp_err_t lcd_init(uint8_t addr, uint8_t sdaPin, uint8_t sclPin);

    /**
     * @brief Return whether the LCD controller completed initialization.
     */
    bool lcd_is_initialized(void);

    /**
     * @brief Probe the LCD's I2C address for an ACK, without writing any
     * command/data bytes -- safe to call at any time, won't disturb
     * whatever is currently on the display.
     * @return ESP_OK if the LCD ACKs its address, an I2C error otherwise.
     */
    esp_err_t lcd_i2c_probe(void);

    /**
     * @brief Initialize I2C bus
     * @param sdaPin GPIO pin for SDA
     * @param sclPin GPIO pin for SCL
     * @return ESP_OK on success
     */
    esp_err_t lcd_i2c_init(uint8_t sdaPin, uint8_t sclPin);

    /**
     * @brief Send command to LCD
     * @param cmd Command byte
     */
    void lcd_send_command(uint8_t cmd);

    /**
     * @brief Clear LCD display
     */
    void lcd_clear(void);

    /**
     * @brief Move cursor to home position (0,0)
     */
    void lcd_home(void);

    /**
     * @brief Set cursor position
     * @param row Row number (0..LCD_ROWS-1)
     * @param col Column number (0..LCD_COLS-1)
     */
    void lcd_set_cursor(uint8_t row, uint8_t col);

    /**
     * @brief Print a single character
     * @param c Character to print
     */
    void lcd_print_char(char c);

    /**
     * @brief Print a string
     * @param str Null-terminated string
     */
    void lcd_print_str(const char *str);

    /**
     * @brief Print a string (alias for lcd_print_str)
     * @param str Null-terminated string
     */
    void lcd_print_string(const char *str);

    /**
     * @brief Print raw string
     * @param s Null-terminated string
     */
    void lcd_print_raw(const char *s);

    /**
     * @brief Print an integer
     * @param value Integer value
     */
    void lcd_print_int(int value);

    /**
     * @brief Print a float with 2 decimal places
     * @param v Float value
     */
    void lcd_print_float(float v);

    /**
     * @brief Print centered text on a row
     * @param row Row number
     * @param str String to center
     */
    void lcd_print_centered(uint8_t row, const char *str);

    /**
     * @brief Printf-style formatted printing
     * @param row Row number
     * @param col Column number
     * @param format Format string (printf style)
     * @param ... Variable arguments
     */
    void lcd_printf(uint8_t row, uint8_t col, const char *format, ...);

    /**
     * @brief Control backlight
     * @param on true = on, false = off
     */
    void lcd_backlight(bool on);

    /**
     * @brief Create custom character
     * @param location Character location (0-7)
     * @param charmap 8-byte array defining character pattern
     */
    void lcd_create_custom_char(uint8_t location, const uint8_t charmap[8]);
    void lcd_init_cgram(void);

    /**
     * @brief Show a two-line message
     * @param line1 First line text
     * @param line2 Second line text (can be NULL)
     */
    void lcd_show_message(const char *line1, const char *line2);

    /**
     * @brief Scroll text across the display
     * @param str String to scroll
     */
    void lcd_text_scroll(const char *str);

    /**
     * @brief Write an integer at specified position with width
     * @param value Integer value
     * @param row Row number
     * @param col Column number
     * @param width Minimum width (zero-padded)
     */
    void lcd_write_int(int value, int row, int col, int width);

// --------------------------------------------------
// GENERIC PRINT MACRO
// --------------------------------------------------

/**
 * @brief Generic print macro - automatically detects type
 * Usage: lcd_print('A');  lcd_print(123);  lcd_print(45.67);  lcd_print("Hello");
 */
#define lcd_print(x) _Generic((x), \
    char: lcd_print_char,          \
    signed char: lcd_print_int,    \
    unsigned char: lcd_print_int,  \
    short: lcd_print_int,          \
    unsigned short: lcd_print_int, \
    int: lcd_print_int,            \
    unsigned int: lcd_print_int,   \
    long: lcd_print_int,           \
    unsigned long: lcd_print_int,  \
    float: lcd_print_float,        \
    double: lcd_print_float,       \
    const char *: lcd_print_str,   \
    char *: lcd_print_str)(x)

#ifdef __cplusplus
}
#endif

#endif // LCD_H