#ifndef LCD_CONFIG_H
#define LCD_CONFIG_H

/*
 * Select the physical character LCD at build time.
 *
 * Default:
 *   -D LCD_GEOMETRY_20X4=0  -> 16 columns × 2 rows
 *
 * 20×4 build:
 *   -D LCD_GEOMETRY_20X4=1  -> 20 columns × 4 rows
 *
 * PlatformIO example:
 *   build_flags = -D LCD_GEOMETRY_20X4=1
 */
#ifndef LCD_GEOMETRY_20X4
#define LCD_GEOMETRY_20X4 0
#endif

#if LCD_GEOMETRY_20X4
#define LCD_ROWS 4
#define LCD_COLS 20
#else
#define LCD_ROWS 2
#define LCD_COLS 16
#endif

#define LCD_LINE_SIZE (LCD_COLS + 1)

#endif /* LCD_CONFIG_H */
