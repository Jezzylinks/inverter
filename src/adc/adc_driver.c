#include "adc/adc_driver.h"
#include "adc/adc_config.h"

/* Mode-specific implementations are private to this dispatcher. */
esp_err_t adc_continuous_driver_init(const adc_channel_t *channels,
                                     size_t channel_count,
                                     adc_driver_channel_t *channel_states,
                                     void **driver_context);
esp_err_t adc_continuous_driver_read_sample(
    void *driver_context,
    const adc_driver_channel_t *channel_state,
    float *out_voltage);
void adc_continuous_driver_deinit(void *driver_context,
                                  adc_driver_channel_t *channel_states,
                                  size_t channel_count);
const char *adc_continuous_driver_get_name(void);
esp_err_t adc_continuous_driver_get_runtime(
    void *driver_context, adc_driver_runtime_t *out);

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

#if ADC_MANAGER_MODE == ADC_MANAGER_MODE_CONTINUOUS
#define ADC_DRIVER_INIT adc_continuous_driver_init
#define ADC_DRIVER_READ_SAMPLE adc_continuous_driver_read_sample
#define ADC_DRIVER_DEINIT adc_continuous_driver_deinit
#define ADC_DRIVER_GET_NAME adc_continuous_driver_get_name
#define ADC_DRIVER_GET_RUNTIME adc_continuous_driver_get_runtime
#elif ADC_MANAGER_MODE == ADC_MANAGER_MODE_ONESHOT
#define ADC_DRIVER_INIT adc_oneshot_driver_init
#define ADC_DRIVER_READ_SAMPLE adc_oneshot_driver_read_sample
#define ADC_DRIVER_DEINIT adc_oneshot_driver_deinit
#define ADC_DRIVER_GET_NAME adc_oneshot_driver_get_name
#define ADC_DRIVER_GET_RUNTIME adc_oneshot_driver_get_runtime
#else
#error "Unsupported ADC manager mode"
#endif

esp_err_t adc_driver_init(const adc_channel_t *channels,
                          size_t channel_count,
                          adc_driver_channel_t *channel_states,
                          void **driver_context)
{
    return ADC_DRIVER_INIT(channels, channel_count, channel_states,
                           driver_context);
}

esp_err_t adc_driver_read_sample(void *driver_context,
                                 const adc_driver_channel_t *channel_state,
                                 float *out_voltage)
{
    return ADC_DRIVER_READ_SAMPLE(driver_context, channel_state, out_voltage);
}

void adc_driver_deinit(void *driver_context,
                       adc_driver_channel_t *channel_states,
                       size_t channel_count)
{
    ADC_DRIVER_DEINIT(driver_context, channel_states, channel_count);
}

const char *adc_driver_get_name(void)
{
    return ADC_DRIVER_GET_NAME();
}

esp_err_t adc_driver_get_runtime(void *driver_context,
                                 adc_driver_runtime_t *out)
{
    return ADC_DRIVER_GET_RUNTIME(driver_context, out);
}
