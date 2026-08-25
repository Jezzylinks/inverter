#include "utility/led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "utility/led.h"
#include "hardware/hardware_config.h"
#include "system/system_state.h"

#include "system/task_watchdog.h"
static volatile bool s_inverter_active = false;
#include "events/event_dispatcher.h"
#include "events/system_events.h"
#include "hardware/hardware_config.h"

#define LED_REPEAT_FOREVER UINT16_MAX

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

void led_set_inverter_active(bool active)
{
    s_inverter_active = active;
    if (active)
        led_on(LED_STATUS);
    else
        led_off(LED_STATUS);
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
    uint8_t repeat_count = (pattern->repeat > 255) ? 255 : (uint8_t)pattern->repeat;

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
                  repeat_count);
        break;

    case LED_PATTERN_PULSE:
        pulse_led(pattern->led,
                  pattern->period_ms,
                  repeat_count);
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

static bool led_wait_for_critical(uint32_t duration_ms,
                                   system_event_t *critical_event)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t duration = pdMS_TO_TICKS(duration_ms);

    while ((xTaskGetTickCount() - start) < duration) {
        task_watchdog_feed();
        system_event_t pending = {0};
        TickType_t elapsed = xTaskGetTickCount() - start;
        TickType_t remaining = duration - elapsed;
        TickType_t wait = remaining > pdMS_TO_TICKS(25)
                              ? pdMS_TO_TICKS(25)
                              : remaining;
        if (wait == 0) {
            wait = 1;
        }

        if (event_dispatcher_receive(EVENT_SUB_LED, &pending, wait)) {
            if (pending.priority == EVENT_PRIORITY_CRITICAL ||
                pending.action == EVENT_ACTION_RECOVERED) {
                if (critical_event != NULL) {
                    *critical_event = pending;
                }
                return true;
            }
            /* Lower-priority pattern requests are intentionally consumed while
             * an active pattern is running; critical events preempt them. */
        }
    }
    return false;
}

static bool led_execute_pattern_interruptible(const led_pattern_t *pattern,
                                              system_event_t *critical_event)
{
    if (pattern == NULL) {
        return false;
    }

    const uint32_t repeats = pattern->repeat;
    switch (pattern->type) {
    case LED_PATTERN_OFF:
        led_off(pattern->led);
        return false;
    case LED_PATTERN_ON:
        set_led_brightness(pattern->led, pattern->brightness);
        return false;
    case LED_PATTERN_FADE:
        fade_led(pattern->led, pattern->brightness, pattern->period_ms);
        if (led_wait_for_critical(pattern->period_ms, critical_event)) {
            all_leds_off();
            return true;
        }
        return false;
    case LED_PATTERN_BLINK:
        for (uint32_t i = 0; i < repeats; ++i) {
            led_on(pattern->led);
            if (led_wait_for_critical(pattern->on_time_ms, critical_event)) {
                all_leds_off();
                return true;
            }
            led_off(pattern->led);
            if (i + 1U < repeats &&
                led_wait_for_critical(pattern->off_time_ms, critical_event)) {
                all_leds_off();
                return true;
            }
        }
        return false;
    case LED_PATTERN_PULSE:
        for (uint32_t i = 0; i < repeats; ++i) {
            fade_led(pattern->led, 100, pattern->period_ms / 2U);
            if (led_wait_for_critical(pattern->period_ms / 2U, critical_event)) {
                all_leds_off();
                return true;
            }
            fade_led(pattern->led, 0, pattern->period_ms / 2U);
            if (i + 1U < repeats &&
                led_wait_for_critical(pattern->period_ms / 2U, critical_event)) {
                all_leds_off();
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

void post_led_event(bool success)
{
    system_event_t evt = {0};
    evt.category = EVENT_CATEGORY_SYSTEM;
    evt.action = success ? EVENT_ACTION_SUCCESS : EVENT_ACTION_ERROR;
    evt.source = EVENT_SOURCE_SYSTEM;
    evt.priority = EVENT_PRIORITY_NORMAL;
    evt.timestamp = xTaskGetTickCount();

    (void)event_dispatcher_send(EVENT_SUB_LED, &evt);
}

void led_event_task(void *pv)
{
    task_watchdog_register("led_event_task");
    system_event_t evt;

    while (1)
    {

        task_watchdog_feed();
        if (!event_dispatcher_receive(EVENT_SUB_LED, &evt, pdMS_TO_TICKS(1000U)))
        {
            continue;
        }

        for (;;) {
        led_pattern_t pattern = {0};
        bool have_pattern = false;
        bool direct = false;
        bool direct_state = false;
        led_channel_t direct_led = LED_STATUS;

        /* --------------------------------------------------------------
         *  Build the request — never execute inside the switch
         * -------------------------------------------------------------- */
        switch (evt.category)
        {
        case EVENT_CATEGORY_PROTECTION:
            pattern.led = LED_ERROR;

            switch (evt.action)
            {
            case EVENT_ACTION_RECOVERED:
                pattern.type = LED_PATTERN_OFF;
                have_pattern = true;
                break;

            case EVENT_ACTION_WARNING:
                pattern.type = LED_PATTERN_BLINK;
                pattern.on_time_ms = 500;
                pattern.off_time_ms = 500;
                pattern.repeat = LED_REPEAT_FOREVER;
                have_pattern = true;
                break;

            case EVENT_ACTION_DERATE:
                pattern.type = LED_PATTERN_BLINK;
                pattern.on_time_ms = 100;
                pattern.off_time_ms = 100;
                pattern.repeat = LED_REPEAT_FOREVER;
                have_pattern = true;
                break;

            case EVENT_ACTION_SHUTDOWN:
                pattern.type = LED_PATTERN_PULSE;
                pattern.brightness = 100;
                pattern.period_ms = 1000;
                pattern.repeat = LED_REPEAT_FOREVER;
                have_pattern = true;
                break;

            case EVENT_ACTION_FAULT:
                pattern.type = LED_PATTERN_BLINK;
                pattern.on_time_ms = 250;
                pattern.off_time_ms = 250;
                pattern.repeat = LED_REPEAT_FOREVER;
                have_pattern = true;
                break;

            case EVENT_ACTION_CRITICAL:
                pattern.type = LED_PATTERN_BLINK;
                pattern.on_time_ms = 50;
                pattern.off_time_ms = 50;
                pattern.repeat = LED_REPEAT_FOREVER;
                have_pattern = true;
                break;

            default:
                break;
            }
            break;

        case EVENT_CATEGORY_SYSTEM:
        case EVENT_CATEGORY_FACTORY_RESET:
        case EVENT_CATEGORY_WIFI:
            switch (evt.action)
            {
            case EVENT_ACTION_ON:
                direct = true;
                direct_state = true;
                direct_led = LED_STATUS;
                break;

            case EVENT_ACTION_OFF:
                direct = true;
                direct_state = false;
                direct_led = LED_STATUS;
                break;

            case EVENT_ACTION_STARTUP:
                pattern.led = LED_STATUS;
                pattern.type = LED_PATTERN_FADE;
                pattern.brightness = 100;
                pattern.period_ms = 1000;
                pattern.repeat = 5;
                have_pattern = true;
                break;

            case EVENT_ACTION_INFO:
                pattern.led = LED_STATUS;
                pattern.type = LED_PATTERN_BLINK;
                pattern.on_time_ms = 100;
                pattern.off_time_ms = 100;
                pattern.repeat = 1;
                have_pattern = true;
                break;

            case EVENT_ACTION_SUCCESS:
                pattern.led = LED_STATUS;
                pattern.type = LED_PATTERN_BLINK;
                pattern.on_time_ms = 80;
                pattern.off_time_ms = 80;
                pattern.repeat = 2;
                have_pattern = true;
                break;

            case EVENT_ACTION_ERROR:
                pattern.led = LED_ERROR;
                pattern.type = LED_PATTERN_BLINK;
                pattern.on_time_ms = 150;
                pattern.off_time_ms = 150;
                pattern.repeat = 2;
                have_pattern = true;
                break;

            case EVENT_ACTION_CHARGING:
                pattern.led = LED_STATUS;
                pattern.type = LED_PATTERN_PULSE;
                pattern.brightness = 100;
                pattern.period_ms = 1500;
                pattern.repeat = LED_REPEAT_FOREVER;
                have_pattern = true;
                break;

            default:
                break;
            }
            break;

        case EVENT_CATEGORY_BUTTON:
            if (evt.action == EVENT_ACTION_PRESSED)
            {
                pattern.led = LED_STATUS;
                pattern.type = LED_PATTERN_BLINK;
                pattern.on_time_ms = 50;
                pattern.off_time_ms = 50;
                pattern.repeat = 1;
                have_pattern = true;
            }
            break;

        default:
            /* Category-agnostic fallback */
            switch (evt.action)
            {
            case EVENT_ACTION_ON:
                direct = true;
                direct_state = true;
                direct_led = LED_STATUS;
                break;

            case EVENT_ACTION_OFF:
                direct = true;
                direct_state = false;
                direct_led = LED_STATUS;
                break;

            case EVENT_ACTION_COMMUNICATION:
                pattern.led = LED_STATUS;
                pattern.type = LED_PATTERN_BLINK;
                pattern.on_time_ms = 75;
                pattern.off_time_ms = 75;
                pattern.repeat = 2;
                have_pattern = true;
                break;

            case EVENT_ACTION_USER_INPUT:
                pattern.led = LED_STATUS;
                pattern.type = LED_PATTERN_BLINK;
                pattern.on_time_ms = 50;
                pattern.off_time_ms = 50;
                pattern.repeat = 1;
                have_pattern = true;
                break;

            default:
                break;
            }
            break;
        }

        /* Critical priority always overrides ordinary direct indications. */
        if (evt.priority == EVENT_PRIORITY_CRITICAL) {
            direct = false;
            have_pattern = true;
            pattern.led = LED_ERROR;
            pattern.type = LED_PATTERN_BLINK;
            pattern.on_time_ms = 50;
            pattern.off_time_ms = 50;
            pattern.repeat = LED_REPEAT_FOREVER;
        }

        /* --------------------------------------------------------------
         *  Single execution point
         * -------------------------------------------------------------- */
        if (direct)
        {
            if (direct_state)
                led_on(direct_led);
            else
                led_off(direct_led);
        }
        else if (have_pattern)
        {
            system_event_t critical_event = {0};
            if (led_execute_pattern_interruptible(&pattern, &critical_event)) {
                evt = critical_event;
                continue;
            }
        }
        if (s_inverter_active)
            led_on(LED_STATUS);
        break;
        }
    }
}
