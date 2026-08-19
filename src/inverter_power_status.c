#include "inverter_power_status.h"

#include <string.h>

#ifndef INVERTER_ENABLE_MOCK_PHYSICAL_FEEDBACK
#define INVERTER_ENABLE_MOCK_PHYSICAL_FEEDBACK 0
#endif

void inverter_power_status_from_snapshot(const system_state_t *state,
                                         inverter_power_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    status->interlock_reason = "System state is unavailable";
    if (state == NULL) return;

    /* GPIO_POWER_RELAY is an output command, not a physical readback. */
    status->relay_commanded = state->output_enabled;
#if INVERTER_ENABLE_MOCK_PHYSICAL_FEEDBACK
    /* TEST ONLY: this build profile mirrors the relay command so mobile UI
     * flows can be exercised before a feedback GPIO exists. The REST and
     * WebSocket contract exposes physical_feedback_mocked so this must never
     * be mistaken for a measured electrical output. */
    status->physical_feedback_supported = true;
    status->physical_feedback_active = state->output_enabled;
    status->physical_feedback_mocked = true;
#else
    /* No dedicated inverter-output feedback input is configured in the
     * production board profile, so clients must not label this as measured
     * feedback or permit remote toggles from this signal alone. */
    status->physical_feedback_supported = false;
    status->physical_feedback_active = false;
#endif

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
