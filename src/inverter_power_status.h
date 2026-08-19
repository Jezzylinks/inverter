#ifndef INVERTER_POWER_STATUS_H
#define INVERTER_POWER_STATUS_H

#include <stdbool.h>

#include "system_state.h"

typedef struct {
    bool relay_commanded;
    bool physical_feedback_supported;
    bool physical_feedback_active;
    bool interlocks_ready;
    const char *interlock_reason;
} inverter_power_status_t;

/* Builds a read-only preflight status from an already locked state snapshot.
 * It must never substitute for the full safety checks executed by
 * inverter_power_on() when a command is actually received. */
void inverter_power_status_from_snapshot(const system_state_t *state,
                                         inverter_power_status_t *status);

#endif /* INVERTER_POWER_STATUS_H */
