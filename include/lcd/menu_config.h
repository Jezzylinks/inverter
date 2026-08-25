#ifndef MENU_CONFIG_H
#define MENU_CONFIG_H

#include "sdkconfig.h"

/* The LCD choice is defined in src/Kconfig.projbuild and selected with
 * `pio run -t menuconfig` or `idf.py menuconfig`. */
#if defined(CONFIG_INVERTER_LCD_16X2) && defined(CONFIG_INVERTER_LCD_20X4)
#error "LCD menuconfig selected both geometries"
#elif defined(CONFIG_INVERTER_LCD_20X4)
#define MENU_CONFIG_LCD_20X4 1
#elif defined(CONFIG_INVERTER_LCD_16X2)
#define MENU_CONFIG_LCD_20X4 0
#else
#error "Select an LCD geometry with menuconfig"
#endif

#if MENU_CONFIG_LCD_20X4 != 0 && MENU_CONFIG_LCD_20X4 != 1
#error "MENU_CONFIG_LCD_20X4 must resolve to 0 (16x2) or 1 (20x4)"
#endif

#endif /* MENU_CONFIG_H */
