// events/protection_handler.c (or add to event_dispatcher.c)

#include "events/event_dispatcher.h"
#include "security/protection.h"
#include "system_state.h"
#include "utility/led.h"
#include "utility/buzzer.h"
#include "esp_log.h"

extern system_state_t sys_state;
extern void shutdown_inverter(void);
extern void lcd_flash_info(const char *line1, const char *line2, uint32_t duration_ms);
extern void inverter_set_current_limit(float amps);

static const char *TAG = "PROT_HANDLER";

/* ---- shared helpers ---- */

static void flash_warning(const char *line1, const char *line2)
{
    lcd_flash_info(line1, line2, 1500);
}

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

static void do_shutdown(const char *line1, const char *line2)
{
    flash_warning(line1, line2);
    beep_shutdown();
    alert_led_shutdown();
    shutdown_inverter();
}

/* ---- one handler per quantity ---- */

static void handle_battery_voltage(const system_event_t *evt)
{
    switch (evt->action)
    {
    case EVENT_ACTION_WARNING:
        flash_warning("Battery Low     ", "Please Recharge ");
        beep_warning();
        alert_led_warn();
        break;

    case EVENT_ACTION_DERATE:
        flash_warning("Battery Low     ", "Reducing Load   ");
        beep_derate();
        alert_led_derate();
        break;

    case EVENT_ACTION_SHUTDOWN:
        do_shutdown("BATTERY CRITICAL", "Shutting Down   ");
        break;

    case EVENT_ACTION_RECOVERED:
        flash_warning("Battery OK      ", "Normal Operation");
        beep_recovered();
        alert_led_recovered();
        break;

    default:
        break;
    }
}

static void handle_temperature(const system_event_t *evt)
{
    switch (evt->action)
    {
    case EVENT_ACTION_WARNING:
        flash_warning("High Temp       ", "Monitor Closely ");
        beep_warning();
        alert_led_warn();
        break;

    case EVENT_ACTION_DERATE:
        flash_warning("Over Temp       ", "Reducing Output ");
        beep_derate();
        alert_led_derate();
        /* Temperature derate is the one case where explicitly clamping
         * output makes physical sense -- give it a real effect, not just
         * a notification. */
        inverter_set_current_limit(sys_state.current_limit * 0.5f);
        break;

    case EVENT_ACTION_SHUTDOWN:
        do_shutdown("OVER TEMP FAULT ", "Shutting Down   ");
        break;

    case EVENT_ACTION_RECOVERED:
        flash_warning("Temp Normal     ", "Resuming Output ");
        beep_recovered();
        alert_led_recovered();
        inverter_set_current_limit(sys_state.current_limit);
        break;

    default:
        break;
    }
}

static void handle_output_current(const system_event_t *evt)
{
    switch (evt->action)
    {
    case EVENT_ACTION_WARNING:
        flash_warning("Load High       ", "Approaching Max ");
        beep_warning();
        alert_led_warn();
        break;

    case EVENT_ACTION_DERATE:
        flash_warning("Overload        ", "Reducing Output ");
        beep_derate();
        alert_led_derate();
        break;

    case EVENT_ACTION_SHUTDOWN:
        do_shutdown("OVERLOAD FAULT  ", "Shutting Down   ");
        break;

    case EVENT_ACTION_RECOVERED:
        flash_warning("Load Normal     ", "Normal Operation");
        beep_recovered();
        alert_led_recovered();
        break;

    default:
        break;
    }
}

static void handle_ac_voltage(const system_event_t *evt)
{
    switch (evt->action)
    {
    case EVENT_ACTION_WARNING:
        flash_warning("AC Voltage      ", "Out of Range    ");
        beep_warning();
        alert_led_warn();
        break;

    case EVENT_ACTION_DERATE:
        flash_warning("AC Voltage      ", "Derating Output ");
        beep_derate();
        alert_led_derate();
        break;

    case EVENT_ACTION_SHUTDOWN:
        do_shutdown("AC FAULT        ", "Shutting Down   ");
        break;

    case EVENT_ACTION_RECOVERED:
        flash_warning("AC Voltage OK   ", "Normal Operation");
        beep_recovered();
        alert_led_recovered();
        break;

    default:
        break;
    }
}

/* ---- dispatch table, indexed by protection_quantity_t ---- */

typedef void (*quantity_handler_t)(const system_event_t *evt);

static const quantity_handler_t quantity_handlers[PROT_QUANTITY_COUNT] = {
    [PROT_QUANTITY_AC_VOLTAGE] = handle_ac_voltage,
    [PROT_QUANTITY_OUTPUT_CURRENT] = handle_output_current,
    [PROT_QUANTITY_TEMPERATURE] = handle_temperature,
    [PROT_QUANTITY_BATTERY_VOLTAGE] = handle_battery_voltage,
};

/* ---- task ---- */

void protection_event_task(void *pv)
{
    system_event_t evt;

    while (1)
    {
        if (!event_dispatcher_receive(EVENT_SUB_ENFORCER, &evt, portMAX_DELAY))
        {
            continue;
        }

        if (evt.category != EVENT_CATEGORY_PROTECTION)
        {
            continue;
        }

        if (evt.quantity >= PROT_QUANTITY_COUNT || quantity_handlers[evt.quantity] == NULL)
        {
            ESP_LOGW(TAG, "No handler for quantity=%d action=%d", evt.quantity, evt.action);
            continue;
        }

        quantity_handlers[evt.quantity](&evt);
    }
}