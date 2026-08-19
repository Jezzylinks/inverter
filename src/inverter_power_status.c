#include "inverter_power_status.h"

#include <string.h>

void inverter_power_status_from_snapshot(const system_state_t *state,
                                         inverter_power_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    status->interlock_reason = "System state is unavailable";
    if (state == NULL) return;

    /* GPIO_POWER_RELAY is an output command, not a physical readback. No
     * dedicated inverter-output feedback input is configured in this board
     * profile, so clients must not label this value as measured feedback. */
    status->relay_commanded = state->output_enabled;
    status->physical_feedback_supported = false;
    status->physical_feedback_active = false;

    if (!state->system_ready) {
        status->interlock_reason = "System initialization is not complete";
    } else if (!state->adc_ready || !state->adc_data_valid) {
        status->interlock_reason = "Required telemetry is unavailable";
    } else if (state->emergency_stop_active) {
        status->interlock_reason = "Emergency stop is active";
    } else if (state->error.error_flags != 0U || state->fault_flags != 0U) {
        status->interlock_reason = "Protection or fault status is active";
    } else {
        status->interlocks_ready = true;
        status->interlock_reason = "Preflight interlocks are ready";
    }
}
