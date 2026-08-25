#ifndef INVERTER_ADC_BACKEND_H
#define INVERTER_ADC_BACKEND_H

#include <stddef.h>

#include "adc/adc.h"
#include "adc/inverter_adc.h"
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    adc_channel_t channel;
    adc_channel_state_t state;
} inverter_adc_backend_channel_t;

typedef struct
{
    inverter_adc_backend_state_t state;
    uint32_t frames_received;
    uint32_t frames_dropped;
    uint32_t pool_overflows;
} inverter_adc_backend_runtime_t;

esp_err_t inverter_adc_backend_init(const adc_channel_t *channels,
                                     size_t channel_count,
                                     inverter_adc_backend_channel_t *states,
                                     void **backend_context);
esp_err_t inverter_adc_backend_read_sample(
    void *backend_context,
    const inverter_adc_backend_channel_t *channel_state,
    float *out_voltage);
void inverter_adc_backend_deinit(void *backend_context,
                                 inverter_adc_backend_channel_t *states,
                                 size_t channel_count);
const char *inverter_adc_backend_name(void);
esp_err_t inverter_adc_backend_get_runtime(
    void *backend_context, inverter_adc_backend_runtime_t *out);

#ifdef __cplusplus
}
#endif

#endif /* INVERTER_ADC_BACKEND_H */
