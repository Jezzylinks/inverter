#ifndef LED_H
#define LED_H

#include <stdint.h>
#include "driver/gpio.h"
#include "driver/ledc.h"

typedef enum
{
    LED_STATUS = LEDC_CHANNEL_1,
    LED_ERROR = LEDC_CHANNEL_2,
} led_channel_t;

typedef enum
{
    LED_PATTERN_OFF,
    LED_PATTERN_ON,
    LED_PATTERN_BLINK,
    LED_PATTERN_PULSE,
    LED_PATTERN_FADE,
} led_pattern_type_t;

typedef struct
{
    led_channel_t led;
    led_pattern_type_t type;

    uint8_t brightness;

    uint16_t on_time_ms;
    uint16_t off_time_ms;
    uint16_t period_ms;

    uint16_t repeat; // Repeat until stopped
} led_pattern_t;

void led_init(void);

void update_led(led_channel_t led, uint8_t brightness_percent);

void led_on(led_channel_t led);
void led_off(led_channel_t led);
void set_led_brightness(led_channel_t led, uint8_t brightness_percent);

void fade_led(led_channel_t led,
              uint8_t target_brightness_percent,
              uint32_t fade_time_ms);

void blink_led(led_channel_t led,
               uint32_t on_time_ms,
               uint32_t off_time_ms,
               uint8_t count);

void pulse_led(led_channel_t led,
               uint32_t period_ms,
               uint8_t cycles);

void set_all_leds(uint8_t brightness_percent);

void all_leds_on(void);
void all_leds_off(void);
void led_event_task(void *pv);
void led_execute_pattern(const led_pattern_t *pattern);
void post_led_event(bool success);
void led_set_inverter_active(bool active);

#endif