#ifndef TELEMETRY_HEALTH_H
#define TELEMETRY_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TELEMETRY_CHANNEL_BATTERY_VOLTAGE = 0,
    TELEMETRY_CHANNEL_AC_VOLTAGE,
    TELEMETRY_CHANNEL_INVERTER_OUTPUT_VOLTAGE,
    TELEMETRY_CHANNEL_LOW_BATTERY,
    TELEMETRY_CHANNEL_COUNT
} telemetry_channel_t;

typedef struct {
    bool valid;
    bool ever_valid;
    float last_value;
    uint32_t last_update_ms;
    uint32_t total_valid_samples;
    uint32_t total_invalid_samples;
    uint32_t consecutive_invalid_samples;
} telemetry_channel_snapshot_t;

typedef struct {
    bool initialized;
    bool battery_ready;
    bool required_ready;
    uint32_t last_update_ms;
    telemetry_channel_snapshot_t channel[TELEMETRY_CHANNEL_COUNT];
} telemetry_health_snapshot_t;

/** Initialize or reset the process-wide telemetry health state. */
void telemetry_health_init(void);

/**
 * Record a sample and validate it against the supplied physical range.
 * Returns true when the sample is finite and within the inclusive range.
 */
bool telemetry_health_record(telemetry_channel_t channel,
                             float value,
                             float minimum,
                             float maximum,
                             uint32_t now_ms);

/** Record a failed ADC/conversion sample for a channel. */
void telemetry_health_record_invalid(telemetry_channel_t channel,
                                     uint32_t now_ms);

/** Set which channels must be valid before the system is considered ready. */
void telemetry_health_set_required_mask(uint32_t required_mask);

/** Return whether a channel is valid and has not exceeded its age limit. */
bool telemetry_health_channel_valid(telemetry_channel_t channel,
                                    uint32_t now_ms,
                                    uint32_t max_age_ms);

/** Return whether all configured required channels are ready and fresh. */
bool telemetry_health_required_ready(uint32_t now_ms,
                                    uint32_t max_age_ms);

/** Return a copy of the current health state for UI/diagnostics. */
void telemetry_health_get_snapshot(telemetry_health_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_HEALTH_H */
