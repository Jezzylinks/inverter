#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

#include "driver/gpio.h"

/* Buttons: active-low inputs with internal pull-ups. */
#define GPIO_BUTTON_POWER GPIO_NUM_16
#define GPIO_BUTTON_ENTER_MENU GPIO_NUM_19
#define GPIO_BUTTON_UP GPIO_NUM_17
#define GPIO_BUTTON_DOWN GPIO_NUM_5
#define GPIO_BUTTON_BACK GPIO_NUM_18

/* Backward-compatible aliases used by the deep-sleep cleanup code. */
#define GPIO_PWR_BTN GPIO_BUTTON_POWER
#define GPIO_BTN_UP GPIO_BUTTON_UP
#define GPIO_BTN_DOWN GPIO_BUTTON_DOWN
#define GPIO_BTN_ENTER GPIO_BUTTON_ENTER_MENU
#define GPIO_BTN_BACK GPIO_BUTTON_BACK

/* GPIO13 is reserved exclusively for the buzzer. */
#define GPIO_BUZZER GPIO_NUM_13
#define GPIO_STATUS_LED GPIO_NUM_14
#define GPIO_ERROR_LED GPIO_NUM_26

#define GPIO_POWER_RELAY GPIO_NUM_12
#define GPIO_I2C_SDA GPIO_NUM_21
#define GPIO_I2C_SCL GPIO_NUM_22
#define GPIO_NEPA_INPUT GPIO_I2C_SCL

/* LCD power is separate from the PWM backlight output. */
#define GPIO_LCD_POWER GPIO_NUM_27
#define GPIO_LCD_BACKLIGHT GPIO_NUM_25

#define GPIO_FAN GPIO_NUM_33      /* Green wire -- PWM speed control (LEDC output) */
#define GPIO_FAN_TACH GPIO_NUM_35 /* Yellow wire -- tachometer pulse input. Input-only \
                                   * pin (34-39 have no output driver on ESP32), which \
                                   * is exactly right for a pure input signal. */
#define CONFIG_USE_LED_PWM 1

#endif