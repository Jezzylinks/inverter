#include "adc/inverter_adc_backend.h"

#include <stdlib.h>

#include "adc/inverter_adc_config.h"
#include "esp_log.h"

#if INVERTER_ADC_MODE == INVERTER_ADC_MODE_ONESHOT

#define ADC_ONESHOT_ATTEN ADC_ATTEN_DB_12
#define ADC_ONESHOT_SAMPLES 10U
#define ADC_ONESHOT_TAG "ADC_ONESHOT"

typedef struct
{
    adc_oneshot_unit_handle_t handle;
} oneshot_context_t;

esp_err_t inverter_adc_backend_init(const adc_channel_t *channels,
                                     size_t channel_count,
                                     inverter_adc_backend_channel_t *states,
                                     void **backend_context)
{
    if (channels == NULL || states == NULL || backend_context == NULL ||
        channel_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    *backend_context = NULL;
    oneshot_context_t *context = calloc(1U, sizeof(*context));
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
        states[i].channel = channels[i];
        states[i].state.cali_handle = NULL;
        states[i].state.is_calibrated = false;
        const esp_err_t config_result = adc_oneshot_config_channel(
            context->handle, channels[i], &config);
        if (config_result != ESP_OK) {
            ESP_LOGE(ADC_ONESHOT_TAG, "Channel %d configuration failed: %s",
                     channels[i], esp_err_to_name(config_result));
            for (size_t cleanup_index = 0U; cleanup_index < i; ++cleanup_index) {
                if (states[cleanup_index].state.cali_handle != NULL) {
                    adc_calibration_deinit(states[cleanup_index].state.cali_handle);
                    states[cleanup_index].state.cali_handle = NULL;
                    states[cleanup_index].state.is_calibrated = false;
                }
            }
            (void)adc_oneshot_del_unit(context->handle);
            free(context);
            return config_result;
        }

        states[i].state.is_calibrated = adc_calibration_init(
            ADC_UNIT_1, channels[i], ADC_ONESHOT_ATTEN,
            &states[i].state.cali_handle);
        if (!states[i].state.is_calibrated) {
            ESP_LOGW(ADC_ONESHOT_TAG,
                     "Channel %d calibration unavailable; raw conversion is retained",
                     channels[i]);
        }
    }

    *backend_context = context;
    return ESP_OK;
}

esp_err_t inverter_adc_backend_read_sample(
    void *backend_context,
    const inverter_adc_backend_channel_t *channel_state,
    float *out_voltage)
{
    if (backend_context == NULL || channel_state == NULL || out_voltage == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const oneshot_context_t *context = backend_context;
    return adc_read_with_multisampling(
        context->handle, channel_state->channel,
        channel_state->state.cali_handle,
        channel_state->state.is_calibrated,
        out_voltage, ADC_ONESHOT_SAMPLES);
}

void inverter_adc_backend_deinit(void *backend_context,
                                 inverter_adc_backend_channel_t *states,
                                 size_t channel_count)
{
    if (backend_context == NULL) {
        return;
    }
    oneshot_context_t *context = backend_context;
    if (states != NULL) {
        for (size_t i = 0U; i < channel_count; ++i) {
            if (states[i].state.cali_handle != NULL) {
                adc_calibration_deinit(states[i].state.cali_handle);
                states[i].state.cali_handle = NULL;
                states[i].state.is_calibrated = false;
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

const char *inverter_adc_backend_name(void)
{
    return "oneshot";
}

esp_err_t inverter_adc_backend_get_runtime(
    void *backend_context, inverter_adc_backend_runtime_t *out)
{
    if (backend_context == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->state = INVERTER_ADC_BACKEND_ONESHOT;
    out->frames_received = 0U;
    out->frames_dropped = 0U;
    out->pool_overflows = 0U;
    return ESP_OK;
}

#endif /* INVERTER_ADC_MODE == INVERTER_ADC_MODE_ONESHOT */
