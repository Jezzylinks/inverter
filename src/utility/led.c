#include "led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "system_state.h"
#include "events/event_dispatcher.h"
#include "events/system_events.h"

extern led_pattern_t pattern;
#define LED_REPEAT_FOREVER UINT16_MAX

static const led_pattern_t led_patterns[] =
    {
        [EVENT_ACTION_INFO] =
            {
                .type = LED_PATTERN_BLINK,
                .led = LED_STATUS,
                .on_time_ms = 100,
                .off_time_ms = 100,
                .repeat = 1,
            },

        [EVENT_ACTION_WARNING] =
            {
                .type = LED_PATTERN_BLINK,
                .led = LED_STATUS,
                .on_time_ms = 250,
                .off_time_ms = 250,
                .repeat = 3,
            },

        [EVENT_ACTION_ERROR] =
            {
                .type = LED_PATTERN_BLINK,
                .led = LED_ERROR,
                .on_time_ms = 100,
                .off_time_ms = 100,
                .repeat = LED_REPEAT_FOREVER,
            },

        [EVENT_ACTION_SUCCESS] =
            {
                .type = LED_PATTERN_ON,
                .led = LED_STATUS,
                .brightness = 100,
                .repeat = 1,
            },
};

void led_init(void)
{
#if CONFIG_USE_LED_PWM

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK};

    ledc_timer_config(&timer);

    typedef struct
    {
        gpio_num_t gpio;
        ledc_channel_t channel;
    } led_cfg_t;

    static const led_cfg_t leds[] =
        {
            {GPIO_STATUS_LED, LED_STATUS},
            {GPIO_ERROR_LED, LED_ERROR},
        };

    for (int i = 0; i < sizeof(leds) / sizeof(leds[0]); i++)
    {
        ledc_channel_config_t ch = {
            .gpio_num = leds[i].gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = leds[i].channel,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };

        ledc_channel_config(&ch);
    }

    ledc_fade_func_install(0);

    all_leds_off();

#endif
}

void update_led(led_channel_t led, uint8_t brightness_percent)
{
    if (brightness_percent > 100)
        brightness_percent = 100;

    uint32_t duty =
        ((1 << 13) - 1) * brightness_percent / 100;

    ledc_set_duty(
        LEDC_LOW_SPEED_MODE,
        led,
        duty);

    ledc_update_duty(
        LEDC_LOW_SPEED_MODE,
        led);
}

void led_on(led_channel_t led)
{
    update_led(led, 100);
}

void led_off(led_channel_t led)
{
    update_led(led, 0);
}

void set_led_brightness(led_channel_t led,
                        uint8_t brightness)
{
    update_led(led, brightness);
}

void fade_led(led_channel_t led,
              uint8_t brightness,
              uint32_t fade_time)
{
    if (brightness > 100)
        brightness = 100;

    uint32_t duty =
        ((1 << 13) - 1) * brightness / 100;

    ledc_set_fade_with_time(
        LEDC_LOW_SPEED_MODE,
        led,
        duty,
        fade_time);

    ledc_fade_start(
        LEDC_LOW_SPEED_MODE,
        led,
        LEDC_FADE_NO_WAIT);
}

void blink_led(led_channel_t led,
               uint32_t on_ms,
               uint32_t off_ms,
               uint8_t count)
{
    for (uint8_t i = 0; i < count; i++)
    {
        led_on(led);
        vTaskDelay(pdMS_TO_TICKS(on_ms));

        led_off(led);

        if (i < count - 1)
            vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

void pulse_led(led_channel_t led,
               uint32_t period,
               uint8_t cycles)
{
    for (uint8_t i = 0; i < cycles; i++)
    {
        fade_led(led, 100, period / 2);
        vTaskDelay(pdMS_TO_TICKS(period / 2));

        fade_led(led, 0, period / 2);
        vTaskDelay(pdMS_TO_TICKS(period / 2));
    }
}

void set_all_leds(uint8_t brightness)
{
    update_led(LED_STATUS, brightness);
    update_led(LED_ERROR, brightness);
}

void all_leds_on(void)
{
    set_all_leds(100);
}

void all_leds_off(void)
{
    set_all_leds(0);
}

void led_execute_pattern(const led_pattern_t *pattern)
{
    if (pattern == NULL)
    {
        return;
    }

    switch (pattern->type)
    {
    case LED_PATTERN_OFF:
        led_off(pattern->led);
        break;

    case LED_PATTERN_ON:
        set_led_brightness(pattern->led, pattern->brightness);
        break;

    case LED_PATTERN_BLINK:
        blink_led(pattern->led,
                  pattern->on_time_ms,
                  pattern->off_time_ms,
                  pattern->repeat);
        break;

    case LED_PATTERN_PULSE:
        pulse_led(pattern->led,
                  pattern->period_ms,
                  pattern->repeat);
        break;

    case LED_PATTERN_FADE:
        fade_led(pattern->led,
                 pattern->brightness,
                 pattern->period_ms);
        break;

    default:
        break;
    }
}

void led_event_task(void *pv)
{
    system_event_t evt;

    while (1)
    {
        if (!event_dispatcher_receive(EVENT_SUB_LED,
                                      &evt,
                                      portMAX_DELAY))
        {
            continue;
        }

        switch (evt.action)
        {
        case EVENT_ACTION_INFO:
            pattern.type = LED_PATTERN_BLINK;
            pattern.on_time_ms = 100;
            pattern.off_time_ms = 100;
            pattern.repeat = 1;
            break;

        case EVENT_ACTION_SUCCESS:
            pattern.type = LED_PATTERN_BLINK;
            pattern.on_time_ms = 80;
            pattern.off_time_ms = 80;
            pattern.repeat = 2;
            break;

        case EVENT_ACTION_RECOVERED:
            pattern.type = LED_PATTERN_BLINK;
            pattern.on_time_ms = 100;
            pattern.off_time_ms = 100;
            pattern.repeat = 3;
            break;

        case EVENT_ACTION_WARNING:
            pattern.type = LED_PATTERN_BLINK;
            pattern.on_time_ms = 500;
            pattern.off_time_ms = 500;
            pattern.repeat = LED_REPEAT_FOREVER;
            break;

        case EVENT_ACTION_DERATE:
            pattern.type = LED_PATTERN_BLINK;
            pattern.on_time_ms = 100;
            pattern.off_time_ms = 100;
            pattern.repeat = LED_REPEAT_FOREVER;
            break;

        case EVENT_ACTION_STARTUP:
            pattern.type = LED_PATTERN_FADE;
            pattern.brightness = 100;
            pattern.period_ms = 1000;
            pattern.repeat = 5;
            break;

        case EVENT_ACTION_SHUTDOWN:
            pattern.type = LED_PATTERN_PULSE;
            pattern.brightness = 100;
            pattern.period_ms = 1000;
            pattern.repeat = LED_REPEAT_FOREVER;
            break;

        case EVENT_ACTION_FAULT:
            pattern.type = LED_PATTERN_BLINK;
            pattern.on_time_ms = 250;
            pattern.off_time_ms = 250;
            pattern.repeat = LED_REPEAT_FOREVER;
            break;

        case EVENT_ACTION_CRITICAL:
            pattern.type = LED_PATTERN_BLINK;
            pattern.on_time_ms = 50;
            pattern.off_time_ms = 50;
            pattern.repeat = LED_REPEAT_FOREVER;
            break;

        case EVENT_ACTION_USER_INPUT:
            pattern.type = LED_PATTERN_BLINK;
            pattern.on_time_ms = 50;
            pattern.off_time_ms = 50;
            pattern.repeat = 1;
            break;

        case EVENT_ACTION_COMMUNICATION:
            pattern.type = LED_PATTERN_BLINK;
            pattern.on_time_ms = 75;
            pattern.off_time_ms = 75;
            pattern.repeat = 2;
            break;

        case EVENT_ACTION_CHARGING:
            pattern.type = LED_PATTERN_PULSE;
            pattern.brightness = 100;
            pattern.period_ms = 1500;
            pattern.repeat = LED_REPEAT_FOREVER;
            break;

        default:
            pattern.type = LED_PATTERN_ON;
            pattern.brightness = 100;
            pattern.repeat = 1;
            break;
        }

        if (evt.action < EVENT_ACTION_COUNT)
        {
            led_execute_pattern(&led_patterns[evt.action]);
        }
    }
}
