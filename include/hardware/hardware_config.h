#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

#include "driver/gpio.h"

/* Buttons: active-low inputs with internal pull-ups. */
#define GPIO_BUTTON_POWER GPIO_NUM_16
#define GPIO_BUTTON_ENTER_MENU GPIO_NUM_19
#define GPIO_BUTTON_UP GPIO_NUM_17
#define GPIO_BUTTON_DOWN GPIO_NUM_5
#define GPIO_BUTTON_BACK GPIO_NUM_18

_Static_assert(GPIO_BUTTON_POWER != GPIO_BUTTON_ENTER_MENU &&
               GPIO_BUTTON_POWER != GPIO_BUTTON_UP &&
               GPIO_BUTTON_POWER != GPIO_BUTTON_DOWN &&
               GPIO_BUTTON_POWER != GPIO_BUTTON_BACK,
               "Power button GPIO must be unique");
_Static_assert(GPIO_BUTTON_ENTER_MENU != GPIO_BUTTON_UP &&
               GPIO_BUTTON_ENTER_MENU != GPIO_BUTTON_DOWN &&
               GPIO_BUTTON_ENTER_MENU != GPIO_BUTTON_BACK,
               "Enter/Menu button GPIO must be unique");
_Static_assert(GPIO_BUTTON_UP != GPIO_BUTTON_DOWN &&
               GPIO_BUTTON_UP != GPIO_BUTTON_BACK &&
               GPIO_BUTTON_DOWN != GPIO_BUTTON_BACK,
               "Navigation button GPIOs must be unique");

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
/* GPIO35 is reserved for Battery Voltage (ADC1 channel 7). The fan tach wire
 * must be rewired to GPIO23, the audited unused digital-input pin. GPIO23 is
 * not an ADC input, button, LCD, LED, buzzer, relay, or fan-PWM owner. */
#define GPIO_FAN_TACH GPIO_NUM_23 /* Yellow wire -- tachometer pulse input */

_Static_assert(GPIO_FAN_TACH != GPIO_BUTTON_POWER &&
               GPIO_FAN_TACH != GPIO_BUTTON_ENTER_MENU &&
               GPIO_FAN_TACH != GPIO_BUTTON_UP &&
               GPIO_FAN_TACH != GPIO_BUTTON_DOWN &&
               GPIO_FAN_TACH != GPIO_BUTTON_BACK,
               "Fan tach GPIO collides with a button GPIO");
_Static_assert(GPIO_FAN_TACH != GPIO_BUZZER &&
               GPIO_FAN_TACH != GPIO_STATUS_LED &&
               GPIO_FAN_TACH != GPIO_ERROR_LED &&
               GPIO_FAN_TACH != GPIO_POWER_RELAY &&
               GPIO_FAN_TACH != GPIO_I2C_SDA &&
               GPIO_FAN_TACH != GPIO_I2C_SCL &&
               GPIO_FAN_TACH != GPIO_LCD_POWER &&
               GPIO_FAN_TACH != GPIO_LCD_BACKLIGHT &&
               GPIO_FAN_TACH != GPIO_FAN,
               "Fan tach GPIO collides with another peripheral");
#define CONFIG_USE_LED_PWM 1

#endif