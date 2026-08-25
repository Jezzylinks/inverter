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
    INVERTER_ADC_CHANNEL_LOW_BATTERY = 0,
    INVERTER_ADC_CHANNEL_AC_VOLTAGE,
    INVERTER_ADC_CHANNEL_BATTERY_VOLTAGE,
    INVERTER_ADC_CHANNEL_OUTPUT_VOLTAGE,
    INVERTER_ADC_CHANNEL_COUNT
} inverter_adc_channel_t;

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

/** Compatibility task entry point retained for existing integrations. */
void adc_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* INVERTER_ADC_H */
