#include "buzzer.h"
#include "events/event_dispatcher.h"
#include "events/system_events.h"
#include "utility/buzzer.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "utility/led.h"
#include "hardware_config.h"
#include "system_state.h"
#include "quiet_hours.h"

extern system_state_t sys_state;

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

static void buzzer_stop(void)
{
    ESP_ERROR_CHECK(ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0));
    ESP_ERROR_CHECK(ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL));
}

void buzzer_beep(uint32_t frequency, uint8_t duty_percent, uint32_t duration_ms)
{
    if (!sys_state.sound_enabled || quiet_hours_is_active())
    {
        /* Still wait out the duration -- callers chain several beeps
         * back-to-back assuming each one blocks for its length, and this
         * function only ever runs on its own dedicated task, so silently
         * skipping the delay would desync multi-tone sequences without
         * actually saving anything meaningful. */
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        return;
    }

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

    buzzer_stop();
}

/* ========================================================================
 *  Lower-level compatibility API -- still called directly from main.c
 *  in a few places outside the event pipeline (e.g. the Sound On/Off
 *  settings toggle immediately silencing any tone in progress).
 * ======================================================================== */

void update_buzzer(uint16_t freq_hz, uint8_t volume_percent)
{
    if (!sys_state.sound_enabled || quiet_hours_is_active() || freq_hz == 0 || volume_percent == 0)
    {
        buzzer_stop();
        return;
    }

    if (volume_percent > 100)
        volume_percent = 100;

    uint32_t max_duty = (1 << BUZZER_LEDC_RES) - 1;
    uint32_t duty = (max_duty * volume_percent) / 100;

    ESP_ERROR_CHECK(ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq_hz));
    ESP_ERROR_CHECK(ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL));
}

void buzzer_off(void)
{
    buzzer_stop();
}

void buzzer_alert(void)
{
    buzzer_beep(2200, 70, 200);
}

void buzzer_error(void)
{
    buzzer_beep(600, 90, 150);
    vTaskDelay(pdMS_TO_TICKS(60));
    buzzer_beep(600, 90, 150);
}

void buzzer_success(void)
{
    buzzer_beep(2000, 55, 100);
}

/* ========================================================================
 *  Pattern helpers (blocking, called only from this task context)
 * ======================================================================== */

static void beep_warning(void) { buzzer_beep(1800, 50, 150); }

static void beep_derate(void)
{
    buzzer_beep(1400, 60, 100);
    vTaskDelay(pdMS_TO_TICKS(80));
    buzzer_beep(1400, 60, 100);
}

static void beep_shutdown(void)
{
    for (int i = 0; i < 3; i++)
    {
        buzzer_beep(800, 85, 200);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

static void beep_recovered(void)
{
    buzzer_beep(1600, 50, 80);
    buzzer_beep(2200, 60, 120);
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

/* Distinct from beep_on/beep_off -- a PIN change, calibration, or
 * factory-reset outcome shouldn't sound identical to powering the
 * inverter on or off. */
static void beep_success(void) { buzzer_beep(2000, 55, 100); }

static void beep_error(void)
{
    buzzer_beep(600, 90, 150);
    vTaskDelay(pdMS_TO_TICKS(60));
    buzzer_beep(600, 90, 150);
}

/* Very short, quiet -- keypress feedback. Kept brief so rapid presses
 * never feel like they're waiting on the buzzer. */
static void beep_click(void) { buzzer_beep(2500, 25, 15); }

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

        switch (evt.category)
        {
        case EVENT_CATEGORY_PROTECTION:
            /* led.c's led_event_task already owns the LED side of these
             * (it's independently subscribed to the same protection
             * events via the routing table) -- this task's only job for
             * PROTECTION is to actually make sound, which previously
             * wasn't happening at all here (this case called LED
             * functions instead of beeping). */
            switch (evt.action)
            {
            case EVENT_ACTION_WARNING:
                beep_warning();
                break;
            case EVENT_ACTION_DERATE:
                beep_derate();
                break;
            case EVENT_ACTION_SHUTDOWN:
                beep_shutdown();
                break;
            case EVENT_ACTION_RECOVERED:
                beep_recovered();
                break;
            default:
                break;
            }
            break;

        case EVENT_CATEGORY_SYSTEM:
        case EVENT_CATEGORY_FACTORY_RESET:
        case EVENT_CATEGORY_WIFI:
            if (evt.action == EVENT_ACTION_ON)
            {
                beep_on();
            }
            else if (evt.action == EVENT_ACTION_OFF)
            {
                beep_off();
            }
            else if (evt.action == EVENT_ACTION_SUCCESS)
            {
                beep_success();
            }
            else if (evt.action == EVENT_ACTION_ERROR)
            {
                beep_error();
            }
            break;

        case EVENT_CATEGORY_BUTTON:
            if (evt.action == EVENT_ACTION_PRESSED)
            {
                beep_click();
            }
            break;

        default:
            break;
        }
    }
}