#include "adc/adc_driver.h"

#include <stdlib.h>

#include "adc/adc_config.h"
#include "adc/adc_oneshot.h"
#include "esp_log.h"

#if ADC_MANAGER_MODE == ADC_MANAGER_MODE_ONESHOT

#define ADC_ONESHOT_ATTEN ADC_ATTEN_DB_12
#define ADC_ONESHOT_SAMPLES 10U
#define ADC_ONESHOT_TAG "ADC_ONESHOT"

typedef struct
{
    adc_oneshot_unit_handle_t handle;
} adc_oneshot_context_t;

esp_err_t adc_oneshot_driver_init(const adc_channel_t *channels,
                                     size_t channel_count,
                                     adc_driver_channel_t *channel_states,
                                     void **driver_context)
{
    if (channels == NULL || channel_states == NULL || driver_context == NULL ||
        channel_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    *driver_context = NULL;
    adc_oneshot_context_t *context = calloc(1U, sizeof(*context));
    if (context == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (!adc_unit_init(&context->handle, ADC_UNIT_1)) {
        ESP_LOGE(ADC_ONESHOT_TAG, "ADC1 oneshot initialization failed");
        free(context);
        return ESP_FAIL;
    }
    const adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ONESHOT_ATTEN,
    };
    for (size_t i = 0U; i < channel_count; ++i) {
        channel_states[i].channel = channels[i];
        channel_states[i].channel_state.cali_handle = NULL;
        channel_states[i].channel_state.is_calibrated = false;
        const esp_err_t config_result = adc_oneshot_config_channel(
            context->handle, channels[i], &config);
        if (config_result != ESP_OK) {
            ESP_LOGE(ADC_ONESHOT_TAG, "Channel %d configuration failed: %s",
                     channels[i], esp_err_to_name(config_result));
            for (size_t cleanup_index = 0U; cleanup_index < i; ++cleanup_index) {
                if (channel_states[cleanup_index].channel_state.cali_handle != NULL) {
                    adc_calibration_deinit(channel_states[cleanup_index].channel_state.cali_handle);
                    channel_states[cleanup_index].channel_state.cali_handle = NULL;
                    channel_states[cleanup_index].channel_state.is_calibrated = false;
                }
            }
            (void)adc_oneshot_del_unit(context->handle);
            free(context);
            return config_result;
        }

        channel_states[i].channel_state.is_calibrated = adc_calibration_init(
            ADC_UNIT_1, channels[i], ADC_ONESHOT_ATTEN,
            &channel_states[i].channel_state.cali_handle);
        if (!channel_states[i].channel_state.is_calibrated) {
            ESP_LOGW(ADC_ONESHOT_TAG,
                     "Channel %d calibration unavailable; raw conversion is retained",
                     channels[i]);
        }
    }

    *driver_context = context;
    return ESP_OK;
}

esp_err_t adc_oneshot_driver_read_sample(
    void *driver_context,
    const adc_driver_channel_t *channel_state,
    float *out_voltage)
{
    if (driver_context == NULL || channel_state == NULL || out_voltage == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const adc_oneshot_context_t *context = driver_context;
    return adc_read_with_multisampling(
        context->handle, channel_state->channel,
        channel_state->channel_state.cali_handle,
        channel_state->channel_state.is_calibrated,
        out_voltage, ADC_ONESHOT_SAMPLES);
}

void adc_oneshot_driver_deinit(void *driver_context,
                                 adc_driver_channel_t *channel_states,
                                 size_t channel_count)
{
    if (driver_context == NULL) {
        return;
    }
    adc_oneshot_context_t *context = driver_context;
    if (channel_states != NULL) {
        for (size_t i = 0U; i < channel_count; ++i) {
            if (channel_states[i].channel_state.cali_handle != NULL) {
                adc_calibration_deinit(channel_states[i].channel_state.cali_handle);
                channel_states[i].channel_state.cali_handle = NULL;
                channel_states[i].channel_state.is_calibrated = false;
            }
        }
    }
    if (context->handle != NULL) {
        const esp_err_t result = adc_oneshot_del_unit(context->handle);
        if (result != ESP_OK) {
            ESP_LOGW(ADC_ONESHOT_TAG, "ADC1 oneshot cleanup failed: %s",
                     esp_err_to_name(result));
        }
    }
    free(context);
}

const char *adc_oneshot_driver_get_name(void)
{
    return "oneshot";
}

esp_err_t adc_oneshot_driver_get_runtime(
    void *driver_context, adc_driver_runtime_t *out)
{
    if (driver_context == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->driver_state = ADC_DRIVER_ONESHOT;
    out->frames_received = 0U;
    out->frames_dropped = 0U;
    out->pool_overflows = 0U;
    return ESP_OK;
}

#endif /* ADC_MANAGER_MODE == ADC_MANAGER_MODE_ONESHOT */
