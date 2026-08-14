#ifndef LCD_CONFIG_H
#define LCD_CONFIG_H

#include <stdint.h>
#include "menu_config.h"

/* menu_config.h is the single source of truth for the physical LCD. */
#if MENU_CONFIG_LCD_20X4
#define LCD_GEOMETRY_20X4 1
#define LCD_ROWS 4
#define LCD_COLS 20
#else
#define LCD_GEOMETRY_20X4 0
#define LCD_ROWS 2
#define LCD_COLS 16
#endif

#define LCD_LINE_SIZE (LCD_COLS + 1)

#define lcd_geometry_rows() ((uint8_t)LCD_ROWS)
#define lcd_geometry_cols() ((uint8_t)LCD_COLS)
#define lcd_geometry_is_20x4() (LCD_GEOMETRY_20X4 != 0)

#endif /* LCD_CONFIG_H */
