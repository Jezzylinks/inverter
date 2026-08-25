#ifndef ADC_H
#define ADC_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

/* Override these at compile time when the target ADC uses another reference
 * voltage or raw-count resolution. */
#ifndef ADC_DRIVER_REFERENCE_VOLTAGE
#define ADC_DRIVER_REFERENCE_VOLTAGE 3.3f
#endif
#ifndef ADC_DRIVER_RAW_FULL_SCALE
#define ADC_DRIVER_RAW_FULL_SCALE 4095.0f
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Per-channel calibration state owned by the reusable ADC driver. */
typedef struct
{
    adc_cali_handle_t cali_handle;
    bool is_calibrated;
} adc_channel_state_t;

/** Initialize an ESP-IDF ADC oneshot unit. */
bool adc_unit_init(adc_oneshot_unit_handle_t *handle, adc_unit_t unit_id);

/** Create the best supported ESP-IDF calibration scheme for one channel. */
bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel,
                          adc_atten_t atten,
                          adc_cali_handle_t *out_handle);

/** Release a channel calibration handle created by adc_calibration_init(). */
void adc_calibration_deinit(adc_cali_handle_t handle);

/** Read and average multiple samples, returning volts at the ADC pin.
 * Calibrated reads use ESP-IDF’s calibration result; uncalibrated reads use
 * ADC_DRIVER_REFERENCE_VOLTAGE and ADC_DRIVER_RAW_FULL_SCALE. */
esp_err_t adc_read_with_multisampling(adc_oneshot_unit_handle_t handle,
                                      adc_channel_t channel,
                                      adc_cali_handle_t cali_handle,
                                      bool is_calibrated,
                                      float *out_voltage,
                                      uint8_t samples);

/** Release channel calibration handles and delete an ADC unit. */
void adc_resources_cleanup(adc_oneshot_unit_handle_t handle,
                           adc_channel_state_t *states,
                           int channel_count);

#ifdef __cplusplus
}
#endif

#endif /* ADC_H */
