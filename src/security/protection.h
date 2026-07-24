/**
 * protection.h
 *
 * Graduated protection state machine for the Vonix Inverter.
 *
 * Design goals:
 *  - No single hard cutoff. Each protected quantity (AC voltage, output
 *    current, internal temperature, battery voltage) moves through
 *    WARNING -> DERATED -> FAULT stages, each with its own threshold
 *    and its own hysteresis band, so the system doesn't chatter at
 *    the boundary.
 *  - Pure state machine: protection_update() takes fresh readings and
 *    returns an action. It does not itself touch GPIO, PWM, or the
 *    LCD - the caller (a dedicated protection_task, or your existing
 *    power/battery tasks) is responsible for acting on the result.
 *    This keeps the logic unit-testable and decoupled from hardware.
 *  - Every transition is reported so it can be pushed into fault_log.
 *
 * Thread-safety: protection_update() is intended to be called from a
 * single owning task (e.g. a 100ms protection_task). If multiple tasks
 * need the latest snapshot, use protection_get_state() which takes an
 * internal mutex.
 */

#ifndef PROTECTION_H
#define PROTECTION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROT_QUANTITY_AC_VOLTAGE = 0,
    PROT_QUANTITY_OUTPUT_CURRENT,
    PROT_QUANTITY_TEMPERATURE,
    PROT_QUANTITY_BATTERY_VOLTAGE,
    PROT_QUANTITY_COUNT
} protection_quantity_t;

typedef enum {
    PROT_STAGE_NORMAL = 0,
    PROT_STAGE_WARNING,   // approaching limit, log + LCD flash message
    PROT_STAGE_DERATED,   // reduce output (e.g. cap power/current target)
    PROT_STAGE_FAULT      // shut down output for this quantity
} protection_stage_t;

typedef enum {
    PROT_ACTION_NONE = 0,
    PROT_ACTION_WARN,          // no hardware change, just notify
    PROT_ACTION_DERATE,        // caller should reduce output setpoint
    PROT_ACTION_SHUTDOWN,      // caller should disable inverter output
    PROT_ACTION_RECOVERED      // stage dropped back to NORMAL
} protection_action_t;

/* Threshold set for one protected quantity. All values are in the
 * quantity's natural unit (Volts, Amps, degrees C). "Low" thresholds
 * are used for battery/voltage under-range; "high" thresholds for
 * over-range. A quantity that only has an upper bound (e.g. current,
 * temperature) can leave the low_* fields at a sentinel like -1e9f. */
typedef struct {
    float warning_high;
    float derate_high;
    float fault_high;
    float hysteresis_high;   // must drop below (fault_high - hysteresis) to clear FAULT

    float warning_low;
    float derate_low;
    float fault_low;
    float hysteresis_low;    // must rise above (fault_low + hysteresis) to clear FAULT

    bool  has_low_bound;     // set false for current/temperature
} protection_thresholds_t;

typedef struct {
    protection_stage_t stage;
    float              last_value;
    uint32_t           stage_entry_time_ms;
    uint32_t           transition_count;   // how many times this quantity has faulted, lifetime
} protection_channel_state_t;

/* Called once from app_main / init sequence, before any task uses
 * protection_update(). Loads defaults; caller may override individual
 * thresholds afterwards with protection_set_thresholds(). */
bool protection_init(void);

/* Override thresholds for a given quantity. Returns false if q is out
 * of range. Safe to call at runtime (e.g. from a settings screen that
 * lets an installer tune limits) - takes the internal mutex. */
bool protection_set_thresholds(protection_quantity_t q, const protection_thresholds_t *thresholds);

/* Feed a fresh reading for one quantity and get back the action the
 * caller should take. This both updates internal stage and returns
 * the delta-action (e.g. PROT_ACTION_SHUTDOWN only fires once, on the
 * transition into FAULT, not on every call while still in FAULT -
 * check protection_get_state() for the current persistent stage). */
protection_action_t protection_update(protection_quantity_t q, float value, uint32_t now_ms);

/* Thread-safe snapshot read, for LCD task / diagnostics. */
bool protection_get_state(protection_quantity_t q, protection_channel_state_t *out_state);

/* True if ANY quantity is currently in FAULT stage - the top-level
 * inverter control loop should treat this as "output must stay off". */
bool protection_any_fault_active(void);

/* Human-readable name, for LCD / fault log rendering. */
const char *protection_quantity_name(protection_quantity_t q);
const char *protection_stage_name(protection_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif // PROTECTION_H
