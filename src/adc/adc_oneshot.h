#ifndef ADC_ONESHOT_H
#define ADC_ONESHOT_H

#include "adc/adc_driver.h"

/* Private oneshot implementation interface. */
esp_err_t adc_oneshot_driver_init(const adc_channel_t *channels,
                                  size_t channel_count,
                                  adc_driver_channel_t *channel_states,
                                  void **driver_context);
esp_err_t adc_oneshot_driver_read_sample(
    void *driver_context,
    const adc_driver_channel_t *channel_state,
    float *out_voltage);
void adc_oneshot_driver_deinit(void *driver_context,
                               adc_driver_channel_t *channel_states,
                               size_t channel_count);
const char *adc_oneshot_driver_get_name(void);
esp_err_t adc_oneshot_driver_get_runtime(
    void *driver_context, adc_driver_runtime_t *out);

#endif /* ADC_ONESHOT_H */
