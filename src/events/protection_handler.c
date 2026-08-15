#include "task_watchdog.h"
// events/protection_handler.c (or add to event_dispatcher.c)

#include "events/event_dispatcher.h"
#include "security/protection.h"
#include "system_state.h"
#include "utility/led.h"
#include "utility/buzzer.h"
#include "lcd_writer.h"
#include "esp_log.h"

extern system_state_t sys_state;
extern void shutdown_inverter(void);
extern void lcd_flash_info(const char *line1, const char *line2, uint32_t duration_ms);
extern void inverter_set_current_limit(float amps);

static const char *TAG = "PROT_HANDLER";

/* ---- shared helpers ---- */

static void flash_warning(const char *line1, const char *line2)
{
    /* Protection enforcement continues during boot, but voltage warnings do
     * not take over the startup presentation. */
    if (lcd_is_startup_active())
        return;

    lcd_flash_info(line1, line2, 1500);
}

/* ---- one handler per quantity ---- */

static void handle_battery_voltage(const system_event_t *evt)
{
    switch (evt->action)
    {
    case EVENT_ACTION_WARNING:
        flash_warning("Battery Low     ", "Please Recharge ");
        break;

    case EVENT_ACTION_DERATE:
        inverter_set_current_limit(100);
        break;

    case EVENT_ACTION_SHUTDOWN:
        shutdown_inverter();
        break;

    case EVENT_ACTION_RECOVERED:
        inverter_set_current_limit(sys_state.current_limit);
        break;

    default:
        break;
    }
}

static void handle_temperature(const system_event_t *evt)
{
    switch (evt->action)
    {
    case EVENT_ACTION_DERATE:
        inverter_set_current_limit(sys_state.current_limit * 0.5f);
        break;

    case EVENT_ACTION_SHUTDOWN:
        shutdown_inverter();
        break;

    case EVENT_ACTION_RECOVERED:
        inverter_set_current_limit(sys_state.current_limit);
        break;

    default:
        break;
    }
}

static void handle_output_current(const system_event_t *evt)
{
    if (evt->action == EVENT_ACTION_SHUTDOWN)
    {
        shutdown_inverter();
    }
}

static void handle_ac_voltage(const system_event_t *evt)
{
    if (evt->action == EVENT_ACTION_SHUTDOWN)
    {
        shutdown_inverter();
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

/* ---- enforcer task ---- */

void protection_event_task(void *pv)
{
    task_watchdog_register("protection_event_task");
    system_event_t evt;

    while (1)
    {

        task_watchdog_feed();
        if (!event_dispatcher_receive(EVENT_SUB_ENFORCER, &evt, pdMS_TO_TICKS(1000U)))
        {
            continue;
        }

        if (evt.category != EVENT_CATEGORY_PROTECTION)
        {
            continue;
        }

        if (evt.quantity >= PROT_QUANTITY_COUNT ||
            quantity_handlers[evt.quantity] == NULL)
        {
            ESP_LOGW(TAG, "No handler for quantity=%d action=%d",
                     evt.quantity, evt.action);
            continue;
        }

        quantity_handlers[evt.quantity](&evt);
    }
}