#ifndef LCD_CONFIG_H
#define LCD_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/*
 * One firmware supports both physical HD44780 geometries. Buffers are sized
 * for the larger panel; the active row/column count is selected at runtime
 * and persisted through the normal settings registry.
 */
#define LCD_ROWS 4
#define LCD_COLS 20
#define LCD_LINE_SIZE (LCD_COLS + 1)

typedef enum
{
    LCD_MODE_16X2 = 0,
    LCD_MODE_20X4 = 1,
    LCD_MODE_COUNT
} lcd_geometry_t;

void lcd_geometry_set(lcd_geometry_t geometry);
lcd_geometry_t lcd_geometry_get(void);
uint8_t lcd_geometry_rows(void);
uint8_t lcd_geometry_cols(void);
bool lcd_geometry_is_20x4(void);

#endif /* LCD_CONFIG_H */
