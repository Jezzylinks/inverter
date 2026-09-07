#ifndef LCD_STARTUP_CONFIG_H
#define LCD_STARTUP_CONFIG_H

/*
 * User-visible startup timing. These values control presentation time only;
 * hardware initialization and ADC sampling remain event-driven and unchanged.
 */
#define LCD_STARTUP_MIN_VISIBLE_DURATION_MS 5000U
#define LCD_STARTUP_IDENTITY_DURATION_MS 1200U
#define LCD_STARTUP_LOADING_DURATION_MS 2400U
#define LCD_STARTUP_STAGE_DURATION_MS 850U
#define LCD_STARTUP_READY_DURATION_MS 1100U

#endif /* LCD_STARTUP_CONFIG_H */
