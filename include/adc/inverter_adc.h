#ifndef INVERTER_ADC_H
#define INVERTER_ADC_H

#include <stdbool.h>
#include <stdint.h>

#include "adc/inverter_adc_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    INVERTER_ADC_STATE_RESET = 0,
    INVERTER_ADC_STATE_INITIALIZING,
    INVERTER_ADC_STATE_RUNNING,
    INVERTER_ADC_STATE_READY,
    INVERTER_ADC_STATE_FAILED
} inverter_adc_state_t;

typedef enum
{
    INVERTER_ADC_BACKEND_UNINITIALIZED = 0,
    INVERTER_ADC_BACKEND_CONTINUOUS,
    INVERTER_ADC_BACKEND_ONESHOT,
    INVERTER_ADC_BACKEND_FALLBACK,
    INVERTER_ADC_BACKEND_FAULT
} inverter_adc_backend_state_t;

typedef enum
{
    INVERTER_ADC_CHANNEL_LOW_BATTERY = 0,
    INVERTER_ADC_CHANNEL_AC_VOLTAGE,
    INVERTER_ADC_CHANNEL_BATTERY_VOLTAGE,
    INVERTER_ADC_CHANNEL_OUTPUT_VOLTAGE,
    INVERTER_ADC_CHANNEL_COUNT
} inverter_adc_channel_t;

typedef struct
{
    float voltage;
    uint32_t sample_count;
    uint32_t timestamp_ms;
    uint32_t error_count;
    bool valid;
    bool calibrated;
    bool fresh;
    bool saturated;
} inverter_adc_measurement_t;

typedef struct
{
    inverter_adc_backend_state_t state;
    uint32_t frames_received;
    uint32_t frames_dropped;
    uint32_t pool_overflows;
    uint32_t read_errors;
    uint32_t invalid_samples;
    uint32_t saturated_samples;
    uint32_t consecutive_failures;
    uint32_t consecutive_successes;
    uint32_t last_success_ms;
} inverter_adc_backend_status_t;

typedef struct
{
    float low_battery_voltage;
    float ac_voltage;
    float battery_voltage;
    float output_voltage;
    uint32_t sequence;
    uint32_t timestamp_ms;
    bool required_data_valid;
    bool fresh;
    inverter_adc_backend_state_t backend_state;
    bool backend_degraded;
    inverter_adc_measurement_t channel[INVERTER_ADC_CHANNEL_COUNT];
    inverter_adc_backend_status_t backend_status;
} inverter_adc_snapshot_t;

/** Start one compile-time-selected ADC backend and its common processing task. */
esp_err_t inverter_adc_start(void);

/** Return the current lifecycle state of the common ADC subsystem. */
inverter_adc_state_t inverter_adc_get_state(void);

/** True only after initialization, calibration/configuration, and a valid fresh
 * required first measurement have completed. */
bool inverter_adc_is_ready(void);

/** Copy the latest coherent application-level ADC snapshot. */
esp_err_t inverter_adc_get_snapshot(inverter_adc_snapshot_t *out);

/** Return the compile-time-selected backend without exposing backend APIs. */
inverter_adc_mode_t inverter_adc_get_mode(void);
/** Copy the current backend state and acquisition health counters. */
esp_err_t inverter_adc_get_backend_status(inverter_adc_backend_status_t *out);
/** Copy the latest quality metadata for one application ADC channel. */
esp_err_t inverter_adc_get_measurement(inverter_adc_channel_t channel,
                                       inverter_adc_measurement_t *out);

/** Compatibility task entry point retained for existing integrations. */
void adc_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* INVERTER_ADC_H */
