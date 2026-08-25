#ifndef SYSTEM_DIAGNOSTICS_H
#define SYSTEM_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t boot_count;
    esp_reset_reason_t last_reset_reason;
    uint32_t last_fault_flags;
    float last_battery_voltage;
    uint32_t last_fault_timestamp_ms;
    uint32_t last_uptime_seconds;
} system_diagnostics_snapshot_t;

bool system_diagnostics_init(void);
void system_diagnostics_record_fault(uint32_t fault_flags,
                                     float battery_voltage,
                                     uint32_t timestamp_ms);
void system_diagnostics_record_uptime(uint32_t uptime_seconds);
bool system_diagnostics_get_snapshot(system_diagnostics_snapshot_t *out);
const char *system_diagnostics_reset_reason_name(esp_reset_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_DIAGNOSTICS_H */
