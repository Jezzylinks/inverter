#include "adc/adc.h"

#include "esp_log.h"

#define ADC_DRIVER_TAG "ADC_DRIVER"

bool adc_unit_init(adc_oneshot_unit_handle_t *handle, adc_unit_t unit_id)
{
    if (handle == NULL)
    {
        ESP_LOGE(ADC_DRIVER_TAG, "Invalid handle pointer");
        return false;
    }

    adc_oneshot_unit_init_cfg_t config = {
        .unit_id = unit_id,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t ret = adc_oneshot_new_unit(&config, handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(ADC_DRIVER_TAG, "Failed to initialize ADC unit %d: %s", unit_id, esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(ADC_DRIVER_TAG, "ADC unit %d initialized successfully", unit_id);
    return true;
}

bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    // Check if the handle pointer is valid
    if (out_handle == NULL)
    {
        ESP_LOGE("ADC_CALIB", "Invalid handle pointer");
        return false;
    }

    const char *ADC_CALIB_TAG = "ADC_CALIB";
    esp_err_t ret = ESP_FAIL;
    adc_cali_handle_t handle = NULL;
    bool calibrated = false; // FIXED: Removed 'static' - each channel needs its own calibration

    ESP_LOGI(ADC_CALIB_TAG, "Initializing ADC calibration for unit %d, channel %d, attenuation %d",
             unit, channel, atten);

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    // Try Curve Fitting calibration first
    ESP_LOGI(ADC_CALIB_TAG, "Attempting Curve Fitting calibration scheme");
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
    if (ret == ESP_OK)
    {
        calibrated = true;
        *out_handle = handle;
        ESP_LOGI(ADC_CALIB_TAG, "Curve Fitting calibration created successfully for channel %d", channel);
        return true;
    }
    else
    {
        ESP_LOGW(ADC_CALIB_TAG, "Curve Fitting calibration failed: %s", esp_err_to_name(ret));
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    // If Curve Fitting is not supported or failed, try Line Fitting
    if (!calibrated)
    {
        ESP_LOGI(ADC_CALIB_TAG, "Attempting Line Fitting calibration scheme");
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };

        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK)
        {
            calibrated = true;
            *out_handle = handle;
            ESP_LOGI(ADC_CALIB_TAG, "Line Fitting calibration created successfully for channel %d", channel);
            return true;
        }
        else
        {
            ESP_LOGW(ADC_CALIB_TAG, "Line Fitting calibration failed: %s", esp_err_to_name(ret));
        }
    }
#endif

    // If we get here, calibration failed
    *out_handle = NULL;

    // Log specific error
    switch (ret)
    {
    case ESP_ERR_INVALID_ARG:
        ESP_LOGE(ADC_CALIB_TAG, "Invalid argument for calibration scheme");
        break;
    case ESP_ERR_NO_MEM:
        ESP_LOGE(ADC_CALIB_TAG, "No memory for calibration scheme");
        break;
    case ESP_ERR_NOT_FOUND:
        ESP_LOGE(ADC_CALIB_TAG, "Calibration data not found");
        break;
    case ESP_ERR_NOT_SUPPORTED:
        ESP_LOGE(ADC_CALIB_TAG, "Calibration scheme not supported on this chip");
        break;
    case ESP_ERR_INVALID_STATE:
        ESP_LOGE(ADC_CALIB_TAG, "Invalid state for calibration scheme");
        break;
    default:
        ESP_LOGE(ADC_CALIB_TAG, "Calibration failed with error: %s (0x%x)",
                 esp_err_to_name(ret), ret);
        break;
    }

    return false;
}

void adc_calibration_deinit(adc_cali_handle_t handle)
{
    if (handle == NULL)
    {
        return;
    }

    const char *TAG = "ADC_CALIBRATION";

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    ESP_LOGI(TAG, "Deregistering Curve Fitting calibration scheme");
    esp_err_t ret = adc_cali_delete_scheme_curve_fitting(handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to delete Curve Fitting scheme: %s", esp_err_to_name(ret));
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    ESP_LOGI(TAG, "Deregistering Line Fitting calibration scheme");
    esp_err_t ret = adc_cali_delete_scheme_line_fitting(handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to delete Line Fitting scheme: %s", esp_err_to_name(ret));
    }
#endif
}

esp_err_t adc_read_with_multisampling(adc_oneshot_unit_handle_t handle,
                                             adc_channel_t channel,
                                             adc_cali_handle_t cali_handle,
                                             bool is_calibrated,
                                             float *out_voltage,
                                             uint8_t samples)
{
    if (handle == NULL || out_voltage == NULL || samples == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t sum_raw = 0;
    int32_t sum_voltage = 0;
    uint8_t valid_samples = 0;

    for (uint8_t i = 0; i < samples; i++)
    {
        int raw_value = 0;
        esp_err_t ret = adc_oneshot_read(handle, channel, &raw_value);

        if (ret != ESP_OK)
        {
            ESP_LOGW(ADC_DRIVER_TAG, "ADC read failed for channel %d: %s", channel, esp_err_to_name(ret));
            continue;
        }

        if (is_calibrated && cali_handle != NULL)
        {
            int voltage_mv = 0;
            ret = adc_cali_raw_to_voltage(cali_handle, raw_value, &voltage_mv);
            if (ret == ESP_OK)
            {
                sum_voltage += voltage_mv;
                valid_samples++;
            }
            else
            {
                ESP_LOGW(ADC_DRIVER_TAG, "Calibration conversion failed: %s", esp_err_to_name(ret));
            }
        }
        else
        {
            ESP_LOGI(ADC_DRIVER_TAG, "Raw ADC value for channel %d: %d", channel, raw_value);
            sum_raw += raw_value;
            valid_samples++;
        }
    }

    if (valid_samples == 0)
    {
        ESP_LOGE(ADC_DRIVER_TAG, "No valid samples obtained for channel %d", channel);
        return ESP_ERR_INVALID_STATE;
    }

    // Calculate average and convert to volts
    if (is_calibrated && cali_handle != NULL)
    {
        // Use calibrated voltage in millivolts, convert to volts
        *out_voltage = (float)(sum_voltage / valid_samples) / 1000.0f;
    }
    else
    {
        // Fallback: approximate conversion without calibration
        // Use the configured reference voltage and raw-count full scale.
        int avg_raw = sum_raw / valid_samples;
        *out_voltage = (float)avg_raw * ADC_DRIVER_REFERENCE_VOLTAGE /
                        ADC_DRIVER_RAW_FULL_SCALE;
        ESP_LOGW(ADC_DRIVER_TAG, "Using uncalibrated ADC reading for channel %d", channel);
    }

    return ESP_OK;
}

void adc_resources_cleanup(adc_oneshot_unit_handle_t handle,
                                  adc_channel_state_t *states,
                                  int channel_count)
{
    if (states != NULL)
    {
        for (int i = 0; i < channel_count; i++)
        {
            if (states[i].cali_handle != NULL)
            {
                adc_calibration_deinit(states[i].cali_handle);
                states[i].cali_handle = NULL;
                states[i].is_calibrated = false;
            }
        }
    }

    if (handle != NULL)
    {
        adc_oneshot_del_unit(handle);
    }
}
