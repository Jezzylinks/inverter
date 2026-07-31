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

#define GPIO_FAN GPIO_NUM_33      /* Green wire -- PWM speed control (LEDC output) */
#define GPIO_FAN_TACH GPIO_NUM_35 /* Yellow wire -- tachometer pulse input. Input-only \
                                   * pin (34-39 have no output driver on ESP32), which \
                                   * is exactly right for a pure input signal. */
#define CONFIG_USE_LED_PWM 1

#endif