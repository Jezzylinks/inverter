#include "utility/buzzer.h"
#include "events/event_dispatcher.h"
#include "events/system_events.h"
#include "utility/buzzer.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "utility/led.h"
#include "hardware/hardware_config.h"
#include "system/system_state.h"
#include "utility/quiet_hours.h"

#include "system/task_watchdog.h"
extern system_state_t sys_state;

static const char *TAG = "BUZZER";
static volatile bool s_critical_preempt_pending = false;
static volatile bool s_buzzer_initialized = false;
static TaskHandle_t s_buzzer_task = NULL;
static volatile uint32_t s_pending_button_clicks = 0U;

#define BUZZER_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_0
#define BUZZER_LEDC_RES LEDC_TIMER_10_BIT

esp_err_t buzzer_init(void)
{
    if (s_buzzer_initialized) {
        return ESP_OK;
    }

    ledc_timer_config_t timer = {
        .speed_mode = BUZZER_LEDC_MODE,
        .timer_num = BUZZER_LEDC_TIMER,
        .duty_resolution = BUZZER_LEDC_RES,
        .freq_hz = 1000, // Initial frequency
        .clk_cfg = LEDC_AUTO_CLK,
    };

    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer initialization failed: %s", esp_err_to_name(err));
        return err;
    }

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

    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel initialization failed on GPIO %d: %s",
                 GPIO_BUZZER, esp_err_to_name(err));
        return err;
    }

    s_buzzer_initialized = true;
    ESP_LOGI(TAG, "Buzzer initialized on GPIO %d (timer %d/channel %d)",
             GPIO_BUZZER, BUZZER_LEDC_TIMER, BUZZER_LEDC_CHANNEL);
    return ESP_OK;
}

static void buzzer_stop(void)
{
    if (!s_buzzer_initialized) {
        return;
    }
    ESP_ERROR_CHECK(ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0));
    ESP_ERROR_CHECK(ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL));
}

void buzzer_beep(uint32_t frequency, uint8_t duty_percent, uint32_t duration_ms)
{
    if (!s_buzzer_initialized) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        return;
    }

    if (s_critical_preempt_pending) {
        buzzer_stop();
        return;
    }

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

    uint32_t remaining_ms = duration_ms;
    while (remaining_ms > 0U && !s_critical_preempt_pending) {
        const uint32_t slice_ms = remaining_ms > 25U ? 25U : remaining_ms;
        vTaskDelay(pdMS_TO_TICKS(slice_ms));
        remaining_ms -= slice_ms;
    }

    buzzer_stop();
}

void buzzer_request_critical_preemption(void)
{
    s_critical_preempt_pending = true;
    buzzer_stop();
}

/* ========================================================================
 *  Lower-level compatibility API -- still called directly from main.c
 *  in a few places outside the event pipeline (e.g. the Sound On/Off
 *  settings toggle immediately silencing any tone in progress).
 * ======================================================================== */

void update_buzzer(uint16_t freq_hz, uint8_t volume_percent)
{
    if (!s_buzzer_initialized || !sys_state.sound_enabled ||
        quiet_hours_is_active() || freq_hz == 0 || volume_percent == 0)
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

static void beep_critical(void)
{
    /* Fast, unmistakable alarm sequence for critical protection events. */
    for (int i = 0; i < 3; ++i) {
        buzzer_beep(900, 95, 120);
        if (i < 2) {
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    }
}

/* Very short, quiet -- keypress feedback. Kept brief so rapid presses
 * never feel like they're waiting on the buzzer. */
static void beep_click(void) { buzzer_beep(2000, 50, 60); }
static void beep_limit(void) { buzzer_beep(1100, 55, 70); }

/* ========================================================================
 *  Task
 * ======================================================================== */

void buzzer_button_click(void)
{
    if (s_buzzer_task != NULL) {
        (void)xTaskNotifyGive(s_buzzer_task);
    } else if (s_pending_button_clicks < UINT32_MAX) {
        s_pending_button_clicks++;
    }
}

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

void post_buzzer_limit_event(void)
{
    system_event_t evt = {0};
    evt.category = EVENT_CATEGORY_BUTTON;
    evt.action = EVENT_ACTION_ERROR;
    evt.source = EVENT_SOURCE_BUTTON;
    evt.priority = EVENT_PRIORITY_NORMAL;
    evt.timestamp = xTaskGetTickCount();
    (void)event_dispatcher_send(EVENT_SUB_BUZZER, &evt);
}

void buzzer_event_task(void *pv)
{
    task_watchdog_register("buzzer_event_task");
    s_buzzer_task = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "Buzzer event task started");
    if (s_pending_button_clicks > 0U) {
        s_pending_button_clicks = 0U;
        (void)xTaskNotifyGive(s_buzzer_task);
    }
    system_event_t evt;
    bool critical_active = false;

    while (1)
    {
        task_watchdog_feed();
        if (!critical_active && ulTaskNotifyTake(pdTRUE, 0) > 0U) {
            s_critical_preempt_pending = false;
            beep_click();
        }
        if (!event_dispatcher_receive(EVENT_SUB_BUZZER,
                                      &evt,
                                      pdMS_TO_TICKS(20U)))
        {
            continue;
        }

        if (evt.action == EVENT_ACTION_RECOVERED) {
            critical_active = false;
        } else if (evt.priority == EVENT_PRIORITY_CRITICAL) {
            s_critical_preempt_pending = false;
            critical_active = true;
            buzzer_off();
            beep_critical();
            continue;
        } else if (critical_active) {
            /* Critical alarm state suppresses lower-priority tones until the
             * corresponding recovery event arrives. */
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
                ESP_LOGD(TAG, "Button press event received; triggering click");
                beep_click();
            }
            else if (evt.action == EVENT_ACTION_ERROR)
            {
                beep_limit();
            }
            break;

        default:
            break;
        }
    }
}