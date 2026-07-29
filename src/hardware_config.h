#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

#include "driver/gpio.h"

#define GPIO_BTN_UP GPIO_NUM_17
#define GPIO_BTN_DOWN GPIO_NUM_5
#define GPIO_BTN_ENTER GPIO_NUM_19
#define GPIO_BTN_BACK GPIO_NUM_18

#define GPIO_PWR_BTN GPIO_NUM_0

#define GPIO_BUZZER GPIO_NUM_13
#define GPIO_STATUS_LED GPIO_NUM_14
#define GPIO_ERROR_LED GPIO_NUM_26

#define GPIO_POWER_RELAY GPIO_NUM_12
#define GPIO_NEPA_INPUT GPIO_NUM_22

#define GPIO_FAN GPIO_NUM_33 /* Green wire -- PWM speed control (LEDC output) */
#define GPIO_FAN_TACH GPIO_NUM_35 /* Yellow wire -- tachometer pulse input. Input-only
                                    * pin (34-39 have no output driver on ESP32), which
                                    * is exactly right for a pure input signal. */
#define GPIO_FAN_TEST GPIO_NUM_4 /* No longer used by post_fan.c now that the fan has a
                                   * real PWM+tach pair -- POST commands actual speed via
                                   * fan_set_speed_percent() instead. Left defined in case
                                   * anything else on the board still wants a spare test
                                   * line. */
#define CONFIG_USE_LED_PWM 1

/* Standard PC/server 4-wire fan PWM frequency (Intel spec). */
#define FAN_PWM_FREQ_HZ 25000

/* Pulses per revolution -- 2 is the standard for PC-style brushless DC
 * fan tachometers. Check your fan's datasheet if RPM readings come out
 * exactly double or half of the rated speed. */
#define FAN_TACH_PULSES_PER_REV 2

/* RPM threshold for fan POST / stall detection. This used to be a
 * voltage threshold (2.0f) for an analog fan-speed sensor -- this
 * hardware has a real tachometer now, so it's RPM. Tune to comfortably
 * below your fan's rated idle/PWM-floor speed; 800 RPM is a reasonable
 * default for a fan rated around 2000-3000 RPM. */
#define FAN_SPEED_THRESHOLD_RPM 800.0f

#endif