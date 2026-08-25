#include "adc/inverter_adc_backend.h"

#include <stdlib.h>

#include "adc/inverter_adc_config.h"
#include "esp_adc/adc_continuous.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

#if INVERTER_ADC_MODE == INVERTER_ADC_MODE_CONTINUOUS

#define ADC_CONTINUOUS_ATTEN ADC_ATTEN_DB_12
#define ADC_CONTINUOUS_SAMPLES 10U
/* ESP32 requires 20 kHz..2 MHz for the digital controller. The rate is the
 * aggregate conversion rate across the four-channel pattern. */
#define ADC_CONTINUOUS_SAMPLE_FREQ_HZ 20000U
/* ESP-IDF's ESP32 continuous example uses a 256-byte frame. This is a
 * multiple of the 4-byte DMA conversion unit and gives the application a
 * complete, stable batch to inspect instead of very small frame fragments. */
#define ADC_CONTINUOUS_FRAME_SIZE 256U
#define ADC_CONTINUOUS_FRAME_COUNT 8U
#define ADC_CONTINUOUS_TAG "ADC_CONTINUOUS"

_Static_assert((ADC_CONTINUOUS_FRAME_SIZE % SOC_ADC_DIGI_DATA_BYTES_PER_CONV) == 0U,
               "ADC DMA frame must contain whole conversion units");

typedef struct
{
    adc_continuous_handle_t handle;
    size_t channel_count;
    volatile uint32_t frames_produced;
    volatile uint32_t pool_overflows;
    volatile uint32_t last_frame_size;
} continuous_context_t;

static bool IRAM_ATTR continuous_on_conv_done(
    adc_continuous_handle_t handle,
    const adc_continuous_evt_data_t *event,
    void *user_data)
{
    (void)handle;
    continuous_context_t *context = user_data;
    if (context != NULL && event != NULL) {
        context->last_frame_size = event->size;
        context->frames_produced++;
    }
    return false;
}

static bool IRAM_ATTR continuous_on_pool_overflow(
    adc_continuous_handle_t handle,
    const adc_continuous_evt_data_t *event,
    void *user_data)
{
    (void)handle;
    (void)event;
    continuous_context_t *context = user_data;
    if (context != NULL) {
        context->pool_overflows++;
    }
    return false;
}

esp_err_t inverter_adc_backend_init(const adc_channel_t *channels,
                                     size_t channel_count,
                                     inverter_adc_backend_channel_t *states,
                                     void **backend_context)
{
    if (channels == NULL || states == NULL || backend_context == NULL ||
        channel_count == 0U || channel_count > SOC_ADC_MAX_CHANNEL_NUM) {
        return ESP_ERR_INVALID_ARG;
    }

    *backend_context = NULL;
    continuous_context_t *context = calloc(1U, sizeof(*context));
    adc_digi_pattern_config_t *pattern = calloc(
        channel_count, sizeof(*pattern));
    if (context == NULL || pattern == NULL) {
        free(context);
        free(pattern);
        return ESP_ERR_NO_MEM;
    }

    const adc_continuous_handle_cfg_t handle_config = {
        .max_store_buf_size = ADC_CONTINUOUS_FRAME_SIZE * ADC_CONTINUOUS_FRAME_COUNT,
        .conv_frame_size = ADC_CONTINUOUS_FRAME_SIZE,
        .flags.flush_pool = 1U,
    };
    esp_err_t result = adc_continuous_new_handle(&handle_config,
                                                  &context->handle);
    if (result != ESP_OK) {
        ESP_LOGE(ADC_CONTINUOUS_TAG, "Continuous handle creation failed: %s",
                 esp_err_to_name(result));
        free(pattern);
        free(context);
        return result;
    }

    for (size_t i = 0U; i < channel_count; ++i) {
        states[i].channel = channels[i];
        states[i].state.cali_handle = NULL;
        states[i].state.is_calibrated = false;
        pattern[i] = (adc_digi_pattern_config_t){
            .atten = ADC_CONTINUOUS_ATTEN,
            .channel = channels[i] & 0x7U,
            .unit = ADC_UNIT_1,
            .bit_width = ADC_BITWIDTH_12,
        };
        int gpio_num = -1;
        const esp_err_t gpio_result = adc_continuous_channel_to_io(
            ADC_UNIT_1, channels[i], &gpio_num);
        if (gpio_result == ESP_OK) {
            ESP_LOGI(ADC_CONTINUOUS_TAG,
                     "Pattern[%u]: ADC1 channel %u -> GPIO%d",
                     (unsigned)i, (unsigned)(channels[i] & 0x7U), gpio_num);
        } else {
            ESP_LOGW(ADC_CONTINUOUS_TAG,
                     "Pattern[%u]: ADC1 channel %u has no GPIO mapping: %s",
                     (unsigned)i, (unsigned)(channels[i] & 0x7U),
                     esp_err_to_name(gpio_result));
        }
        states[i].state.is_calibrated = adc_calibration_init(
            ADC_UNIT_1, channels[i], ADC_CONTINUOUS_ATTEN,
            &states[i].state.cali_handle);
        if (!states[i].state.is_calibrated) {
            ESP_LOGW(ADC_CONTINUOUS_TAG,
                     "Channel %d calibration unavailable; raw conversion is retained",
                     channels[i]);
        }
    }

    const adc_continuous_config_t config = {
        .pattern_num = channel_count,
        .adc_pattern = pattern,
        .sample_freq_hz = ADC_CONTINUOUS_SAMPLE_FREQ_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    result = adc_continuous_config(context->handle, &config);
    free(pattern);
    if (result != ESP_OK) {
        ESP_LOGE(ADC_CONTINUOUS_TAG, "Continuous configuration failed: %s",
                 esp_err_to_name(result));
        inverter_adc_backend_deinit(context, states, channel_count);
        return result;
    }

    const adc_continuous_evt_cbs_t callbacks = {
        .on_conv_done = continuous_on_conv_done,
        .on_pool_ovf = continuous_on_pool_overflow,
    };
    result = adc_continuous_register_event_callbacks(
        context->handle, &callbacks, context);
    if (result != ESP_OK) {
        ESP_LOGE(ADC_CONTINUOUS_TAG,
                 "Continuous callback registration failed: %s",
                 esp_err_to_name(result));
        inverter_adc_backend_deinit(context, states, channel_count);
        return result;
    }

    ESP_LOGI(ADC_CONTINUOUS_TAG,
             "Configured ADC1 DMA: frame=%u bytes, ring=%u bytes, rate=%u Hz, channels=%u",
             (unsigned)ADC_CONTINUOUS_FRAME_SIZE,
             (unsigned)(ADC_CONTINUOUS_FRAME_SIZE * ADC_CONTINUOUS_FRAME_COUNT),
             (unsigned)ADC_CONTINUOUS_SAMPLE_FREQ_HZ,
             (unsigned)channel_count);

    result = adc_continuous_start(context->handle);
    if (result != ESP_OK) {
        ESP_LOGE(ADC_CONTINUOUS_TAG, "Continuous start failed: %s",
                 esp_err_to_name(result));
        inverter_adc_backend_deinit(context, states, channel_count);
        return result;
    }

    context->channel_count = channel_count;
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

    continuous_context_t *context = backend_context;
    uint8_t buffer[ADC_CONTINUOUS_FRAME_SIZE];
    uint32_t valid_samples = 0U;
    int64_t sum_voltage_mv = 0;
    int64_t sum_raw = 0;
    uint32_t attempts = 0U;
    bool timeout_reported = false;

    while (valid_samples < ADC_CONTINUOUS_SAMPLES && attempts < 4U) {
        uint32_t bytes_read = 0U;
        const esp_err_t result = adc_continuous_read(
            context->handle, buffer, sizeof(buffer), &bytes_read, 100U);
        if (result != ESP_OK) {
            if (result == ESP_ERR_INVALID_STATE) {
                const esp_err_t flush_result =
                    adc_continuous_flush_pool(context->handle);
                if (flush_result != ESP_OK) {
                    ESP_LOGW(ADC_CONTINUOUS_TAG,
                             "DMA pool flush failed after overflow: %s",
                             esp_err_to_name(flush_result));
                }
            }
            if (result != ESP_ERR_TIMEOUT || !timeout_reported) {
                ESP_LOGW(ADC_CONTINUOUS_TAG,
                         "DMA read failed: %s (frames=%lu overflows=%lu last_frame=%lu)",
                         esp_err_to_name(result),
                         (unsigned long)context->frames_produced,
                         (unsigned long)context->pool_overflows,
                         (unsigned long)context->last_frame_size);
                timeout_reported = (result == ESP_ERR_TIMEOUT);
            }
            return result;
        }

        for (uint32_t offset = 0U;
             offset + SOC_ADC_DIGI_DATA_BYTES_PER_CONV <= bytes_read;
             offset += SOC_ADC_DIGI_DATA_BYTES_PER_CONV) {
            const adc_digi_output_data_t *sample =
                (const adc_digi_output_data_t *)(buffer + offset);
            if (sample->type1.channel != (channel_state->channel & 0x7U)) {
                continue;
            }

            const int raw_value = sample->type1.data;
            if (channel_state->state.is_calibrated &&
                channel_state->state.cali_handle != NULL) {
                int voltage_mv = 0;
                const esp_err_t conversion_result = adc_cali_raw_to_voltage(
                    channel_state->state.cali_handle, raw_value, &voltage_mv);
                if (conversion_result != ESP_OK) {
                    continue;
                }
                sum_voltage_mv += voltage_mv;
            } else {
                sum_raw += raw_value;
            }
            ++valid_samples;
            if (valid_samples >= ADC_CONTINUOUS_SAMPLES) {
                break;
            }
        }
        ++attempts;
    }

    if (valid_samples == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel_state->state.is_calibrated &&
        channel_state->state.cali_handle != NULL) {
        *out_voltage = (float)sum_voltage_mv /
                       ((float)valid_samples * 1000.0f);
    } else {
        *out_voltage = ((float)sum_raw / (float)valid_samples) *
                       ADC_DRIVER_REFERENCE_VOLTAGE /
                       ADC_DRIVER_RAW_FULL_SCALE;
    }
    return ESP_OK;
}

void inverter_adc_backend_deinit(void *backend_context,
                                 inverter_adc_backend_channel_t *states,
                                 size_t channel_count)
{
    if (backend_context == NULL) {
        return;
    }
    continuous_context_t *context = backend_context;
    if (context->handle != NULL) {
        const esp_err_t stop_result = adc_continuous_stop(context->handle);
        if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(ADC_CONTINUOUS_TAG, "Continuous stop failed: %s",
                     esp_err_to_name(stop_result));
        }
        const esp_err_t deinit_result = adc_continuous_deinit(context->handle);
        if (deinit_result != ESP_OK) {
            ESP_LOGW(ADC_CONTINUOUS_TAG, "Continuous deinit failed: %s",
                     esp_err_to_name(deinit_result));
        }
    }
    if (states != NULL) {
        for (size_t i = 0U; i < channel_count; ++i) {
            if (states[i].state.cali_handle != NULL) {
                adc_calibration_deinit(states[i].state.cali_handle);
                states[i].state.cali_handle = NULL;
                states[i].state.is_calibrated = false;
            }
        }
    }
    free(context);
}

const char *inverter_adc_backend_name(void)
{
    return "continuous";
}

#endif /* INVERTER_ADC_MODE == INVERTER_ADC_MODE_CONTINUOUS */
