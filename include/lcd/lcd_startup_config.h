#ifndef LCD_STARTUP_CONFIG_H
#define LCD_STARTUP_CONFIG_H

/*
 * User-visible startup timing. These values control presentation time only;
 * hardware initialization and ADC sampling remain event-driven and unchanged.
 *
 * LCD_STARTUP_IDENTITY_DURATION_MS  -- company logo/brand visibility window.
 *   Increased from 1200 ms so the logo is clearly legible on the physical
 *   ESP32 before the hardware-stage screen appears.
 *
 * LCD_STARTUP_STAGE_DURATION_MS     -- minimum time each startup stage is
 *   shown on the physical panel. Increased from 850 ms so the user can read
 *   each stage name before it advances to the next one.
 *
 * LCD_STARTUP_READY_DURATION_MS     -- time the READY screen is held before
 *   transitioning to the main operating screen.
 */
#define LCD_STARTUP_MIN_VISIBLE_DURATION_MS 8000U
#define LCD_STARTUP_IDENTITY_DURATION_MS    2000U
#define LCD_STARTUP_LOADING_DURATION_MS     2400U
#define LCD_STARTUP_STAGE_DURATION_MS       1600U
#define LCD_STARTUP_READY_DURATION_MS       1500U

#endif /* LCD_STARTUP_CONFIG_H */
