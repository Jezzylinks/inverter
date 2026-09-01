#ifndef ADC_MANAGER_H
#define ADC_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "adc/adc_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    ADC_MANAGER_STATE_RESET = 0,
    ADC_MANAGER_STATE_INITIALIZING,
    ADC_MANAGER_STATE_RUNNING,
    ADC_MANAGER_STATE_READY,
    ADC_MANAGER_STATE_FAILED
} adc_manager_state_t;

typedef enum
{
    ADC_DRIVER_UNINITIALIZED = 0,
    ADC_DRIVER_CONTINUOUS,
    ADC_DRIVER_ONESHOT,
    ADC_DRIVER_FALLBACK,
    ADC_DRIVER_FAULT
} adc_driver_state_t;

typedef enum
{
    ADC_MANAGER_CHANNEL_LOW_BATTERY = 0,
    ADC_MANAGER_CHANNEL_AC_VOLTAGE,
    ADC_MANAGER_CHANNEL_BATTERY_VOLTAGE,
    ADC_MANAGER_CHANNEL_OUTPUT_VOLTAGE,
    ADC_MANAGER_CHANNEL_COUNT
} adc_manager_channel_t;

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
} adc_manager_measurement_t;

typedef struct
{
    adc_driver_state_t driver_state;
    uint32_t frames_received;
    uint32_t frames_dropped;
    uint32_t pool_overflows;
    uint32_t read_errors;
    uint32_t invalid_samples;
    uint32_t saturated_samples;
    uint32_t consecutive_failures;
    uint32_t consecutive_successes;
    uint32_t last_success_ms;
} adc_driver_status_t;

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
    adc_driver_state_t driver_state;
    bool driver_degraded;
    adc_manager_measurement_t channel[ADC_MANAGER_CHANNEL_COUNT];
    adc_driver_status_t driver_status;
} adc_manager_snapshot_t;

/** Start one compile-time-selected ADC driver and its common processing task. */
esp_err_t adc_manager_start(void);

/** Return the current lifecycle state of the common ADC subsystem. */
adc_manager_state_t adc_manager_get_state(void);

/** True only after initialization, calibration/configuration, and a valid fresh
 * required first measurement have completed. */
bool adc_manager_is_ready(void);

/** Copy the latest coherent application-level ADC snapshot. */
esp_err_t adc_manager_get_snapshot(adc_manager_snapshot_t *out);

/** Return the compile-time-selected driver without exposing driver APIs. */
adc_manager_mode_t adc_manager_get_mode(void);
/** Copy the current driver state and acquisition health counters. */
esp_err_t adc_manager_get_driver_status(adc_driver_status_t *out);
/** Copy the latest quality metadata for one application ADC channel. */
esp_err_t adc_manager_get_measurement(adc_manager_channel_t channel,
                                       adc_manager_measurement_t *out);

/** Compatibility task entry point retained for existing integrations. */
void adc_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* ADC_MANAGER_H */
