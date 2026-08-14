#ifndef MENU_CONFIG_H
#define MENU_CONFIG_H

/*
 * Select the physical LCD before compiling the firmware.
 *
 * 0 = 16 columns x 2 rows
 * 1 = 20 columns x 4 rows
 *
 * Edit only this value, then rebuild with the same PlatformIO environment:
 *     pio run
 */
#define MENU_CONFIG_LCD_20X4 0

#if MENU_CONFIG_LCD_20X4 != 0 && MENU_CONFIG_LCD_20X4 != 1
#error "MENU_CONFIG_LCD_20X4 must be 0 (16x2) or 1 (20x4)"
#endif

#endif /* MENU_CONFIG_H */

