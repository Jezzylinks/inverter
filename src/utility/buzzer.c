#include "buzzer.h"
#include "events/event_dispatcher.h"
#include "events/system_events.h"
#include "utility/buzzer.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "utility/led.h"
#include "hardware_config.h"

#define BUZZER_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_0
#define BUZZER_LEDC_RES LEDC_TIMER_10_BIT

void buzzer_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = BUZZER_LEDC_MODE,
        .timer_num = BUZZER_LEDC_TIMER,
        .duty_resolution = BUZZER_LEDC_RES,
        .freq_hz = 1000, // Initial frequency
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .gpio_num = GPIO_BUZZER,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 0,
    };

    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

void buzzer_beep(uint32_t frequency, uint8_t duty_percent, uint32_t duration_ms)
{
    if (duty_percent > 100)
        duty_percent = 100;

    uint32_t max_duty = (1 << BUZZER_LEDC_RES) - 1;
    uint32_t duty = (max_duty * duty_percent) / 100;

    ESP_ERROR_CHECK(ledc_set_freq(
        BUZZER_LEDC_MODE,
        BUZZER_LEDC_TIMER,
        frequency));

    ESP_ERROR_CHECK(ledc_set_duty(
        BUZZER_LEDC_MODE,
        BUZZER_LEDC_CHANNEL,
        duty));

    ESP_ERROR_CHECK(ledc_update_duty(
        BUZZER_LEDC_MODE,
        BUZZER_LEDC_CHANNEL));

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    // Stop buzzer
    ESP_ERROR_CHECK(ledc_set_duty(
        BUZZER_LEDC_MODE,
        BUZZER_LEDC_CHANNEL,
        0));

    ESP_ERROR_CHECK(ledc_update_duty(
        BUZZER_LEDC_MODE,
        BUZZER_LEDC_CHANNEL));
}

/* ========================================================================
 *  Pattern helpers (blocking, called only from this task context)
 * ======================================================================== */

static void beep_warning(void)
{
    buzzer_beep(1800, 50, 150);
}

static void beep_on(void)
{
    buzzer_beep(1200, 60, 70);
    buzzer_beep(1700, 65, 70);
    buzzer_beep(2300, 70, 120);
}

static void beep_off(void)
{
    buzzer_beep(2000, 60, 90);
    buzzer_beep(1400, 60, 90);
    buzzer_beep(800, 60, 150);
}

// Buzzer starts here
static void alert_led_warn(void)
{
    led_execute_pattern(&(led_pattern_t){
        .led = LED_ERROR,
        .type = LED_PATTERN_BLINK,
        .on_time_ms = 500,
        .off_time_ms = 500,
        .repeat = 3});
}

static void alert_led_derate(void)
{
    led_execute_pattern(&(led_pattern_t){
        .led = LED_ERROR,
        .type = LED_PATTERN_BLINK,
        .on_time_ms = 150,
        .off_time_ms = 150,
        .repeat = 5});
}

static void alert_led_shutdown(void)
{
    led_execute_pattern(&(led_pattern_t){
        .led = LED_ERROR,
        .type = LED_PATTERN_BLINK,
        .on_time_ms = 100,
        .off_time_ms = 150,
        .repeat = 10});
}

static void alert_led_recovered(void)
{
    led_execute_pattern(&(led_pattern_t){.led = LED_ERROR, .type = LED_PATTERN_OFF});
}

/* ========================================================================
 *  Task
 * ======================================================================== */

void post_buzzer_event(bool success)
{
    system_event_t evt = {0};
    evt.category = EVENT_CATEGORY_SYSTEM;
    evt.action = success ? EVENT_ACTION_SUCCESS : EVENT_ACTION_ERROR;
    evt.source = EVENT_SOURCE_SYSTEM;
    evt.priority = EVENT_PRIORITY_NORMAL;
    evt.timestamp = xTaskGetTickCount();

    (void)event_dispatcher_send(EVENT_SUB_BUZZER, &evt);
}

void buzzer_event_task(void *pv)
{
    system_event_t evt;

    while (1)
    {
        if (!event_dispatcher_receive(EVENT_SUB_BUZZER,
                                      &evt,
                                      portMAX_DELAY))
        {
            continue;
        }
        ESP_LOGI("BUZZER_EVENT", "EVENT DISPATCHED TO BUZZER");
        switch (evt.category)
        {
        case EVENT_CATEGORY_PROTECTION:
            switch (evt.action)
            {
            case EVENT_ACTION_WARNING:
                alert_led_warn();
                break;
            case EVENT_ACTION_DERATE:
                alert_led_derate();
                break;
            case EVENT_ACTION_SHUTDOWN:
                ESP_LOGI("EVENT_PROTECTION", "EVENT PROTECTED BY SHUTDOWN");
                alert_led_shutdown();
                break;
            case EVENT_ACTION_RECOVERED:
                alert_led_recovered();
                break;
            default:
                break;
            }
            break;

        case EVENT_CATEGORY_SYSTEM:
            if (evt.action == EVENT_ACTION_ON)
            {
                beep_on();
            }
            else if (evt.action == EVENT_ACTION_OFF)
            {
                ESP_LOGI("EVENT_CATEGORY", "EVENT ACTION OFF BUZZER");
                beep_off();
            }
            else if (evt.action == EVENT_ACTION_SUCCESS)
            {
                ESP_LOGI("EVENT_CATEGORY", "EVENT ACTION OFF BUZZER");
                beep_on();
            }
            else if (evt.action == EVENT_ACTION_ERROR)
            {
                beep_off();
            }
            break;

        case EVENT_CATEGORY_BUTTON:
            if (evt.action == EVENT_ACTION_PRESSED)
            {
                beep_warning();
            }
            break;

        default:
            break;
        }
    }
}