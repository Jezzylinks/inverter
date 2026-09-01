#ifndef ADC_DRIVER_H
#define ADC_DRIVER_H

#include <stddef.h>

#include "adc/adc.h"
#include "adc/adc_manager.h"
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    adc_channel_t channel;
    adc_channel_state_t channel_state;
} adc_driver_channel_t;

typedef struct
{
    adc_driver_state_t driver_state;
    uint32_t frames_received;
    uint32_t frames_dropped;
    uint32_t pool_overflows;
} adc_driver_runtime_t;

esp_err_t adc_driver_init(const adc_channel_t *channels,
                                     size_t channel_count,
                                     adc_driver_channel_t *channel_states,
                                     void **driver_context);
esp_err_t adc_driver_read_sample(
    void *driver_context,
    const adc_driver_channel_t *channel_state,
    float *out_voltage);
void adc_driver_deinit(void *driver_context,
                                 adc_driver_channel_t *channel_states,
                                 size_t channel_count);
const char *adc_driver_get_name(void);
esp_err_t adc_driver_get_runtime(
    void *driver_context, adc_driver_runtime_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ADC_DRIVER_H */
