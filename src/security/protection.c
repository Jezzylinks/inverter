#include "protection.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "system_state.h"

static const char *TAG = "protection";

static protection_thresholds_t s_thresholds[PROT_QUANTITY_COUNT];
static protection_channel_state_t s_state[PROT_QUANTITY_COUNT];
static SemaphoreHandle_t s_mutex;
extern QueueHandle_t protection_event_queue;
extern system_state_t sys_state;

typedef void (*protection_error_handler_t)(
    protection_stage_t stage,
    float value);

typedef struct
{
    protection_quantity_t quantity;
    protection_error_handler_t handler;
} protection_error_map_t;

static const protection_error_map_t error_map[] =
    {
        {PROT_QUANTITY_AC_VOLTAGE, ac_voltage_error_handler},
        {PROT_QUANTITY_OUTPUT_CURRENT, current_error_handler},
        {PROT_QUANTITY_TEMPERATURE, temperature_error_handler},
        {PROT_QUANTITY_BATTERY_VOLTAGE, battery_error_handler},
};

/* Sensible factory defaults for a 24V-class inverter. Tune these to
 * your actual hardware in the settings menu / commissioning step -
 * these are starting points, not certified limits. */
static void load_defaults(void)
{
    s_thresholds[PROT_QUANTITY_AC_VOLTAGE] = (protection_thresholds_t){
        .warning_high = 250.0f,
        .derate_high = 260.0f,
        .fault_high = 270.0f,
        .hysteresis_high = 8.0f,
        .warning_low = 190.0f,
        .derate_low = 180.0f,
        .fault_low = 170.0f,
        .hysteresis_low = 8.0f,
        .has_low_bound = true,
    };
    s_thresholds[PROT_QUANTITY_OUTPUT_CURRENT] = (protection_thresholds_t){
        .warning_high = 18.0f,
        .derate_high = 20.0f,
        .fault_high = 24.0f,
        .hysteresis_high = 2.0f,
        .has_low_bound = false,
    };
    s_thresholds[PROT_QUANTITY_TEMPERATURE] = (protection_thresholds_t){
        .warning_high = 65.0f,
        .derate_high = 75.0f,
        .fault_high = 90.0f,
        .hysteresis_high = 5.0f,
        .has_low_bound = false,
    };
    s_thresholds[PROT_QUANTITY_BATTERY_VOLTAGE] = (protection_thresholds_t){
        /* deep-discharge protection lives on the low side */
        .warning_high = 1e9f,
        .derate_high = 1e9f,
        .fault_high = 1e9f,
        .hysteresis_high = 0.0f,
        .warning_low = 22.5f,
        .derate_low = 21.5f,
        .fault_low = 20.5f,
        .hysteresis_low = 0.6f,
        .has_low_bound = true,
    };
}

bool protection_init(void)
{
    protection_event_queue =
        xQueueCreate(10, sizeof(protection_event_msg_t));

    assert(protection_event_queue);

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex)
    {
        ESP_LOGE(TAG, "failed to create mutex");
        return false;
    }
    load_defaults();
    memset(s_state, 0, sizeof(s_state));
    return true;
}

bool protection_set_thresholds(protection_quantity_t q, const protection_thresholds_t *t)
{
    if (q >= PROT_QUANTITY_COUNT || !t)
        return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_thresholds[q] = *t;
    xSemaphoreGive(s_mutex);
    return true;
}

static void update_error_flag(protection_quantity_t quantity,
                              protection_stage_t stage)
{
    for (size_t i = 0;
         i < ARRAY_SIZE(error_map);
         i++)
    {
        if (error_map[i].quantity != quantity)
            continue;

        if (stage == PROT_STAGE_FAULT)
        {
            sys_state.error.error_flags |=
                error_map[i].error_flag;
        }
        else
        {
            sys_state.error.error_flags &=
                ~error_map[i].error_flag;
        }

        return;
    }
}

static void ac_voltage_error_handler(
    protection_stage_t stage,
    float value)
{
    (void)value;

    if (stage == PROT_STAGE_FAULT)
    {
        sys_state.error.error_flags |= ERR_AC_FAULT;
    }
    else
    {
        sys_state.error.error_flags &= ~ERR_AC_FAULT;
    }
}

static void output_current_error_handler(
    protection_stage_t stage,
    float value)
{
    (void)value;

    if (stage == PROT_STAGE_FAULT)
    {
        sys_state.error.error_flags |= ERR_OVERLOAD;
    }
    else
    {
        sys_state.error.error_flags &= ~ERR_OVERLOAD;
    }
}

static void temperature_error_handler(
    protection_stage_t stage,
    float value)
{
    (void)value;

    if (stage == PROT_STAGE_FAULT)
    {
        sys_state.error.error_flags |= ERR_OVER_TEMP;
    }
    else
    {
        sys_state.error.error_flags &= ~ERR_OVER_TEMP;
    }
}

static void battery_error_handler(
    protection_stage_t stage,
    float value)
{
    if (stage != PROT_STAGE_FAULT)
    {
        sys_state.error.error_flags &=
            ~(ERR_LOW_BAT | ERR_HIGH_BAT);

        return;
    }

    if (value <
        sys_state.battery_profile.cutoff_voltage_12v)
    {
        sys_state.error.error_flags |= ERR_LOW_BAT;

        sys_state.error.error_flags &= ~ERR_HIGH_BAT;
    }
    else
    {
        sys_state.error.error_flags |= ERR_HIGH_BAT;

        sys_state.error.error_flags &= ~ERR_LOW_BAT;
    }
}

static void clear_battery_error_flags(void)
{
    sys_state.error.error_flags &=
        ~(ERR_LOW_BAT | ERR_HIGH_BAT);
}

/* Figure out which stage `value` falls into given thresholds and the
 * PREVIOUS stage (needed for hysteresis: we only fall back down a
 * stage once the value has cleared the band by the hysteresis margin,
 * not the instant it crosses the raw threshold going the other way). */
static protection_stage_t classify(const protection_thresholds_t *t,
                                   protection_stage_t prev_stage,
                                   float value)
{
    /* High-side evaluation */
    bool fault_high_set = value >= t->fault_high;
    bool derate_high_set = value >= t->derate_high;
    bool warning_high_set = value >= t->warning_high;

    /* Low-side evaluation (only if this quantity has a low bound) */
    bool fault_low_set = t->has_low_bound && value <= t->fault_low;
    bool derate_low_set = t->has_low_bound && value <= t->derate_low;
    bool warning_low_set = t->has_low_bound && value <= t->warning_low;

    protection_stage_t raw_stage = PROT_STAGE_NORMAL;
    if (fault_high_set || fault_low_set)
        raw_stage = PROT_STAGE_FAULT;
    else if (derate_high_set || derate_low_set)
        raw_stage = PROT_STAGE_DERATED;
    else if (warning_high_set || warning_low_set)
        raw_stage = PROT_STAGE_WARNING;

    /* Apply hysteresis only when trying to step DOWN from a worse
     * stage than the raw thresholds alone would indicate. */
    if (prev_stage == PROT_STAGE_FAULT && raw_stage != PROT_STAGE_FAULT)
    {
        bool cleared_high = value < (t->fault_high - t->hysteresis_high);
        bool cleared_low = !t->has_low_bound || value > (t->fault_low + t->hysteresis_low);
        if (!(cleared_high && cleared_low))
        {
            raw_stage = PROT_STAGE_FAULT; // stay latched until clear of hysteresis band
        }
    }
    return raw_stage;
}

protection_action_t protection_update(protection_quantity_t q, float value, uint32_t now_ms)
{
    if (q >= PROT_QUANTITY_COUNT)
        return PROT_ACTION_NONE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    protection_thresholds_t t = s_thresholds[q];
    protection_stage_t prev = s_state[q].stage;
    protection_stage_t next = classify(&t, prev, value);

    s_state[q].last_value = value;
    protection_action_t action;

    if (next != prev)
    {
        s_state[q].stage = next;
        s_state[q].stage_entry_time_ms = now_ms;

        if (q == PROT_QUANTITY_BATTERY_VOLTAGE)
        {
            if (next == PROT_STAGE_FAULT)
            {
                update_battery_error_flag(value);
            }
            else
            {
                clear_battery_error_flags();
            }
        }
        else
        {
            update_error_flag(q, next);
        }

        if (next == PROT_STAGE_FAULT)
        {
            s_state[q].transition_count++;

            action = PROT_ACTION_SHUTDOWN;
        }
        else if (next == PROT_STAGE_DERATED)
        {
            action = PROT_ACTION_DERATE;
        }
        else if (next == PROT_STAGE_WARNING)
        {
            action = PROT_ACTION_WARN;
        }
        else
        {
            action = PROT_ACTION_RECOVERED;
        }

        ESP_LOGW(TAG,
                 "%s: %s -> %s (%.2f)",
                 protection_quantity_name(q),
                 protection_stage_name(prev),
                 protection_stage_name(next),
                 value);

        /* Automatically notify the event bus */
        if (action != PROT_ACTION_NONE)
        {
            system_event_post_protection(q,
                                         action,
                                         value);
        }
    }
    xSemaphoreGive(s_mutex);
    return action;
}

bool protection_get_state(protection_quantity_t q, protection_channel_state_t *out)
{
    if (q >= PROT_QUANTITY_COUNT || !out)
        return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_state[q];
    xSemaphoreGive(s_mutex);
    return true;
}

bool protection_any_fault_active(void)
{
    bool fault = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < PROT_QUANTITY_COUNT; i++)
    {
        if (s_state[i].stage == PROT_STAGE_FAULT)
        {
            fault = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return fault;
}

const char *protection_quantity_name(protection_quantity_t q)
{
    switch (q)
    {
    case PROT_QUANTITY_AC_VOLTAGE:
        return "AC Voltage";
    case PROT_QUANTITY_OUTPUT_CURRENT:
        return "Output Current";
    case PROT_QUANTITY_TEMPERATURE:
        return "Temperature";
    case PROT_QUANTITY_BATTERY_VOLTAGE:
        return "Battery Voltage";
    default:
        return "Unknown";
    }
}

const char *protection_stage_name(protection_stage_t stage)
{
    switch (stage)
    {
    case PROT_STAGE_NORMAL:
        return "NORMAL";
    case PROT_STAGE_WARNING:
        return "WARNING";
    case PROT_STAGE_DERATED:
        return "DERATED";
    case PROT_STAGE_FAULT:
        return "FAULT";
    default:
        return "?";
    }
}

static void protection_event_post(
    protection_quantity_t quantity,
    protection_action_t event)
{
    protection_event_msg_t msg;

    msg.quantity = quantity;
    msg.event = event;

    xQueueSend(protection_event_queue,
               &msg,
               0);
}

void handle_protection_action(
    protection_quantity_t quantity,
    protection_action_t action)
{
    switch (action)
    {
    case PROT_ACTION_WARN:

        protection_event_post(quantity,
                              PROT_ACTION_WARN);

        break;

    case PROT_ACTION_DERATE:

        protection_event_post(quantity,
                              PROT_ACTION_DERATE);

        break;

    case PROT_ACTION_SHUTDOWN:

        protection_event_post(quantity,
                              PROT_ACTION_SHUTDOWN);

        break;

    case PROT_ACTION_RECOVERED:

        protection_event_post(quantity,
                              PROT_ACTION_RECOVERED);

        break;

    default:

        break;
    }
}