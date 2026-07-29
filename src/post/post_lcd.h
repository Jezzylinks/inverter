#ifndef POST_LCD_H
#define POST_LCD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Power-On Self-Test for the LCD.
     *
     * Probes the LCD's I2C address for an ACK -- confirms the display is
     * physically present and wired correctly (SDA/SCL/power) before the
     * rest of boot relies on it to show anything, including the results
     * of the other POST checks. Does not write any command/data bytes,
     * so it can't corrupt whatever is currently on screen.
     *
     * @return true if the LCD ACKs its I2C address.
     * @return false otherwise (not present, wrong address, wiring fault).
     */
    bool post_lcd_test(void);

#ifdef __cplusplus
}
#endif

#endif /* POST_LCD_H */
