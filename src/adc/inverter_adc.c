#include "adc/inverter_adc.h"
#include "adc/adc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"

#include "app/app_runtime.h"
#include "battery/battery_estimator.h"
#include "battery/battery_filter.h"
#include "cloud/cloud_reporting.h"
#include "telemetry/telemetry_health.h"
#include "events/protection_handler.h"
#include "lcd/lcd_writer.h"
#include "server/websocket/websocket_server.h"
#include "system/inverter_errors.h"
#include "system/task_watchdog.h"
#include "system/utils.h"
#include "wifi/wifi_monitor.h"

#define CONFIG_USE_ADC 1
#define ADC_ATTEN_USED ADC_ATTEN_DB_12
#define ADC_MULTISAMPLING_COUNT 10
#define TELEMETRY_STALE_TIMEOUT_MS 1000U
#define BATTERY_ADC_PHYSICAL_MIN_V 0.5f
#define AC_ADC_PHYSICAL_MAX_V 350.0f
#define BATTERY_ADC_PHYSICAL_MARGIN 1.15f

#define LOW_BATTERY_VOLTAGE_THRESHOLD 10.5f
#define UNDER_VOLTAGE_THRESHOLD 160.0f
#define OVER_VOLTAGE_THRESHOLD 260.0f
#define BATTERY_VOLTAGE_THRESHOLD 12.0f
#define INVERTER_OUTPUT_VOLTAGE_THRESHOLD 220.0f

#define R1_BATTERY_VOLTAGE 56000.0f
#define R2_BATTERY_VOLTAGE 15000.0f
#define BATTERY_VOLTAGE_DIVIDER_RATIO ((R1_BATTERY_VOLTAGE + R2_BATTERY_VOLTAGE) / R2_BATTERY_VOLTAGE)
#define R1_LOW_BATTERY 56000.0f
#define R2_LOW_BATTERY 15000.0f
#define LOW_BATTERY_DIVIDER_RATIO ((R1_LOW_BATTERY + R2_LOW_BATTERY) / R2_LOW_BATTERY)
#define R1_AC_VOLTAGE 56000.0f
#define R2_AC_VOLTAGE 15000.0f
#define AC_VOLTAGE_DIVIDER_RATIO ((R1_AC_VOLTAGE + R2_AC_VOLTAGE) / R2_AC_VOLTAGE)
#define R1_INVERTER_VOLTAGE 56000.0f
#define R2_INVERTER_VOLTAGE 15000.0f
#define INVERTER_VOLTAGE_DIVIDER_RATIO ((R1_INVERTER_VOLTAGE + R2_INVERTER_VOLTAGE) / R2_INVERTER_VOLTAGE)

#define INVERTER_ADC_DRIVER_TAG "ADC_INIT"

enum
{
    INVERTER_ADC_LOW_BATTERY = ADC_CHANNEL_6,
    INVERTER_ADC_OVER_UNDER_VOLTAGE = ADC_CHANNEL_0,
    INVERTER_ADC_BATTERY_VOLTAGE = ADC_CHANNEL_7,
    INVERTER_ADC_FAN = ADC_CHANNEL_5,
    INVERTER_ADC_OUTPUT_VOLTAGE = ADC_CHANNEL_4,
};

extern battery_estimator_t bat_estimate;
extern void check_protections(void);
extern void inverter_emergency_disable(const char *reason);
extern const char *get_error_string(uint32_t flags);

static battery_filter_t battery_voltage_filter;

static float inverter_adc_clamp_float(float value, float min, float max)
{
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

// ADC channel ID enumeration for application logic
typedef enum
{
    CHANNEL_ID_LOW_BATTERY = 0,
    CHANNEL_ID_OVER_UNDER_VOLTAGE,
    CHANNEL_ID_BATTERY_VOLTAGE,
    CHANNEL_ID_FAN,
    CHANNEL_ID_INVERTER_OUTPUT_VOLTAGE,
    CHANNEL_ID_MAX
} adc_channel_id_t;

// ADC channel configuration structure
typedef struct
{
    adc_channel_t channel;       // ESP-IDF ADC channel (ADC_CHANNEL_0, ADC_CHANNEL_1, etc.)
    adc_channel_id_t channel_id; // Application-specific channel identifier
    float *target_value;
    float threshold_low;
    float threshold_high;
    bool has_high_threshold;
    uint32_t error_flag;
    const char *name;
    float voltage_divider_ratio; // Ratio = (R1 + R2) / R2, where R1 is top resistor, R2 is bottom
} adc_channel_config_t;

// ADC reading context
typedef struct
{
    adc_oneshot_unit_handle_t handle;
    adc_channel_state_t *channel_states; // Array of channel states (one per config entry)
    int channel_count;
} adc_context_t;

// Forward declarations
static bool configure_adc_channels(adc_oneshot_unit_handle_t handle,
                                   const adc_channel_config_t *configs,
                                   adc_channel_state_t *states,
                                   int channel_count,
                                   adc_unit_t unit_id);
static void process_adc_reading(const adc_channel_config_t *config,
                                const adc_channel_state_t *state,
                                adc_oneshot_unit_handle_t handle);


// ADC channel configurations

/**
 * @brief Set error flag
 */
void set_error_flag(uint32_t flag)
{
    sys_state.error.error_flags |= flag;
}

/**
 * @brief Clear error flag
 */
void clear_error_flag(uint32_t flag)
{
    sys_state.error.error_flags &= ~flag;
}

static const adc_channel_config_t adc_configs[] = {
    {.channel = INVERTER_ADC_LOW_BATTERY, // Replace with actual ADC channel for low battery
     .channel_id = CHANNEL_ID_LOW_BATTERY,
     .target_value = &sys_state.inverter.low_bat_egs002_signal,
     .threshold_low = LOW_BATTERY_VOLTAGE_THRESHOLD,
     .has_high_threshold = false,
     .error_flag = ERR_LOW_BAT,
     .name = "Low Battery",
     .voltage_divider_ratio = LOW_BATTERY_DIVIDER_RATIO},

    {.channel = INVERTER_ADC_OVER_UNDER_VOLTAGE, // Replace with actual ADC channel for AC voltage
     .channel_id = CHANNEL_ID_OVER_UNDER_VOLTAGE,
     .target_value = &sys_state.inverter.over_under_voltage,
     .threshold_low = UNDER_VOLTAGE_THRESHOLD,
     .threshold_high = OVER_VOLTAGE_THRESHOLD,
     .has_high_threshold = true,
     .error_flag = ERR_AC_FAULT,
     .name = "AC Voltage",
     .voltage_divider_ratio = AC_VOLTAGE_DIVIDER_RATIO},

    {.channel = INVERTER_ADC_BATTERY_VOLTAGE, // Replace with actual ADC channel for battery voltage
     .channel_id = CHANNEL_ID_BATTERY_VOLTAGE,
     .target_value = &sys_state.inverter.battery.voltage,
     .threshold_low = BATTERY_VOLTAGE_THRESHOLD,
     .threshold_high = 0,
     .has_high_threshold = false,
     .error_flag = ERR_BATTERY_VOLTAGE,
     .name = "Battery Voltage",
     .voltage_divider_ratio = BATTERY_VOLTAGE_DIVIDER_RATIO},

    {.channel = INVERTER_ADC_OUTPUT_VOLTAGE, // Replace with actual ADC channel for inverter voltage
     .channel_id = CHANNEL_ID_INVERTER_OUTPUT_VOLTAGE,
     .target_value = &sys_state.inverter.output_voltage,
     .threshold_low = INVERTER_OUTPUT_VOLTAGE_THRESHOLD,
     .has_high_threshold = false,
     .error_flag = ERR_INVERTER_VOLTAGE,
     .name = "Inverter Voltage",
     .voltage_divider_ratio = INVERTER_VOLTAGE_DIVIDER_RATIO}};

/* Event bits */
#define EVT_ADC_VALID (1 << 1)

void adc_task(void *arg)
{
    task_watchdog_register("adc_task");
    telemetry_health_init();
    telemetry_health_set_required_mask(
        1UL << TELEMETRY_CHANNEL_BATTERY_VOLTAGE);
#if CONFIG_USE_ADC
    ESP_LOGI(INVERTER_ADC_DRIVER_TAG, "ADC Task started");

    // Get number of configured channels
    const int config_count = sizeof(adc_configs) / sizeof(adc_configs[0]);

    // Initialize ADC1
    adc_oneshot_unit_handle_t adc1_handle = NULL;

    // Allocate channel states (one per configured channel)
    adc_channel_state_t *adc1_states = (adc_channel_state_t *)calloc(config_count, sizeof(adc_channel_state_t));
    if (adc1_states == NULL)
    {
        ESP_LOGE(INVERTER_ADC_DRIVER_TAG, "Failed to allocate memory for channel states");
        vTaskDelete(NULL);
        return;
    }

    if (!adc_unit_init(&adc1_handle, ADC_UNIT_1))
    {
        ESP_LOGE(INVERTER_ADC_DRIVER_TAG, "Failed to initialize ADC1");
        free(adc1_states);
        vTaskDelete(NULL);
        return;
    }

    if (!configure_adc_channels(adc1_handle, adc_configs, adc1_states,
                                config_count, ADC_UNIT_1))
    {
        ESP_LOGE(INVERTER_ADC_DRIVER_TAG, "Failed to configure ADC channels");
        adc_resources_cleanup(adc1_handle, adc1_states, config_count);
        free(adc1_states);
        vTaskDelete(NULL);
        return;
    }

    adc_context_t adc1_context = {
        .handle = adc1_handle,
        .channel_states = adc1_states,
        .channel_count = config_count};

#if EXAMPLE_USE_ADC2
    // WARNING: ADC2 is shared with WiFi. If WiFi is enabled, ADC2 reads may fail.
    // Ensure WiFi is not active when using ADC2 or handle read failures gracefully.
    adc_oneshot_unit_handle_t adc2_handle = NULL;
    adc_channel_state_t *adc2_states = (adc_channel_state_t *)calloc(config_count, sizeof(adc_channel_state_t));

    if (adc2_states == NULL)
    {
        ESP_LOGE(INVERTER_ADC_DRIVER_TAG, "Failed to allocate memory for ADC2 channel states");
        adc_resources_cleanup(adc1_handle, adc1_states, config_count);
        free(adc1_states);
        vTaskDelete(NULL);
        return;
    }

    if (!adc_unit_init(&adc2_handle, ADC_UNIT_2))
    {
        ESP_LOGE(INVERTER_ADC_DRIVER_TAG, "Failed to initialize ADC2");
        adc_resources_cleanup(adc1_handle, adc1_states, config_count);
        free(adc1_states);
        free(adc2_states);
        vTaskDelete(NULL);
        return;
    }

    if (!configure_adc_channels(adc2_handle, adc_configs, adc2_states,
                                config_count, ADC_UNIT_2))
    {
        ESP_LOGE(INVERTER_ADC_DRIVER_TAG, "Failed to configure ADC2 channels");
        adc_resources_cleanup(adc1_handle, adc1_states, config_count);
        adc_resources_cleanup(adc2_handle, adc2_states, config_count);
        free(adc1_states);
        free(adc2_states);
        vTaskDelete(NULL);
        return;
    }
#endif

    ESP_LOGI(INVERTER_ADC_DRIVER_TAG, "ADC initialization complete");
#endif

    bool first_sample = true;
    bool telemetry_shutdown_latched = false;
    uint8_t sample_count = 0;
    uint32_t last_ws_publish_ms = 0U;
    const uint8_t SAMPLES_BEFORE_ERROR_CHECK = 10;

    ESP_LOGI(INVERTER_ADC_DRIVER_TAG, "Starting ADC sampling and LCD updates");

    while (1)
    {
        task_watchdog_feed();
        for (int i = 0; i < config_count; i++)
        {
            process_adc_reading(&adc_configs[i],
                                &adc1_context.channel_states[i],
                                adc1_context.handle);
        }

        const uint32_t sample_time_ms =
            (uint32_t)(esp_timer_get_time() / 1000ULL);
        const bool telemetry_ready = telemetry_health_required_ready(
            sample_time_ms, TELEMETRY_STALE_TIMEOUT_MS);
        sys_state.adc_data_valid = telemetry_ready;
        sys_state.inverter.adc_data_valid = telemetry_ready;

        if (telemetry_ready && sample_count < SAMPLES_BEFORE_ERROR_CHECK)
        {
            sys_state.error.error_flags = 0; // Clear errors during warmup
            sample_count++;
            ESP_LOGI(INVERTER_ADC_DRIVER_TAG, "ADC warmup: %d/%d", sample_count, SAMPLES_BEFORE_ERROR_CHECK);
        }
        xEventGroupSetBits(sys_event_group, APP_EVENT_ADC_READY);
        if (telemetry_ready) {
            telemetry_shutdown_latched = false;
            xEventGroupSetBits(sys_event_group, EVT_ADC_VALID);
        } else {
            xEventGroupClearBits(sys_event_group, EVT_ADC_VALID);
            sys_state.error.error_flags |= ERR_BATTERY_VOLTAGE;
            ESP_LOGW(INVERTER_ADC_DRIVER_TAG, "Required battery telemetry is invalid or stale");
            if (!telemetry_shutdown_latched &&
                (sys_state.inverter.inverter_active ||
                 sys_state.inverter.inverter_state == INVERTER_STARTING)) {
                telemetry_shutdown_latched = true;
                inverter_emergency_disable("battery telemetry invalid or stale");
            }
        }

        if (telemetry_ready && sample_count >= SAMPLES_BEFORE_ERROR_CHECK)
        {
            check_protections();
        }

        /* Update main screen data for lcd_task */
        const float battery_soc =
            inverter_adc_clamp_float(battery_estimator_get_soc(&bat_estimate), 0.0f, 100.0f);
        const uint8_t battery_pct = (uint8_t)battery_soc;
        sys_state.inverter.battery.battery_soc = battery_soc;
        lcd_update_main_data(
            sys_state.inverter.battery.voltage,
            sys_state.inverter.output_voltage,
            sys_state.inverter.output_current,
            sys_state.inverter.output_frequency,
            sys_state.inverter.battery.battery_temperature,
            sys_state.inverter.load_percentage,
            battery_pct,
            sys_state.inverter.inverter_active,
            sys_state.inverter.connected,
            sys_state.battery_charging);

        const float pv_kw = (sys_state.dc_input_voltage > 0.0f &&
                             sys_state.dc_input_current > 0.0f)
                                ? (sys_state.dc_input_voltage *
                                   sys_state.dc_input_current / 1000.0f)
                                : 0.0f;
        const float load_kw = (sys_state.inverter.output_voltage > 0.0f &&
                               sys_state.inverter.output_current > 0.0f)
                                  ? (sys_state.inverter.output_voltage *
                                     sys_state.inverter.output_current / 1000.0f)
                                  : 0.0f;
        /* Estimate runtime only when a meaningful load exists. The estimate
         * uses remaining Ah from the coulomb estimator and converts load power
         * through a conservative inverter-efficiency factor. */
        uint16_t remaining_minutes = 0U;
        const float remaining_ah = battery_estimator_get_remaining_ah(&bat_estimate);
        const float battery_voltage = sys_state.inverter.battery.voltage;
        const float efficiency = (sys_state.efficiency > 0.50f &&
                                  sys_state.efficiency <= 1.0f)
                                     ? sys_state.efficiency
                                     : 0.90f;
        if (remaining_ah > 0.05f && load_kw > 0.02f && battery_voltage > 5.0f)
        {
            const float battery_current_a =
                (load_kw * 1000.0f) / (battery_voltage * efficiency);
            if (battery_current_a > 0.05f)
            {
                const float minutes = (remaining_ah / battery_current_a) * 60.0f;
                remaining_minutes = (minutes >= 65535.0f)
                                        ? UINT16_MAX
                                        : (uint16_t)minutes;
            }
        }

        /* There is no dedicated grid-power meter in the current hardware map;
         * keep this honest rather than fabricating a value. */
        lcd_update_main_power(pv_kw, 0.0f, load_kw,
                              sys_state.inverter.output_voltage,
                              remaining_minutes,
                              (uint8_t)sys_state.battery_profile.nominal_voltage,
                              sys_state.inverter.operating_mode);
        lcd_update_wifi_status(wifi_monitor_is_online(),
                               wifi_monitor_get_rssi());
        if ((uint32_t)(sample_time_ms - last_ws_publish_ms) >= 1000U) {
            last_ws_publish_ms = sample_time_ms;
            websocket_broadcast_device_status();
            cloud_reporting_publish(&sys_state, pv_kw, load_kw,
                                    wifi_monitor_get_rssi());
        }

        if (sys_lcd.screen == LCD_SCREEN_STANDBY)
        {
            lcd_show_standby(sys_state.inverter.battery.voltage,
                             battery_pct,
                             sys_state.inverter.connected);
        }

        /* Show fault screen immediately if error flags are set */
        if (sys_state.error.error_flags && telemetry_ready &&
            sample_count >= SAMPLES_BEFORE_ERROR_CHECK)
        {
            const char *err = get_error_string(sys_state.error.error_flags);
            char l0[LCD_LINE_SIZE], l1[LCD_LINE_SIZE];
            snprintf(l0, LCD_LINE_SIZE, "%-16.16s", err);
            snprintf(l1, LCD_LINE_SIZE, "%-16s", "Check system    ");
            lcd_show_fault(l0, l1);
        }
        else if (sys_lcd.screen == LCD_SCREEN_FAULT)
        {
            /* Fault cleared — return to main */
            lcd_clear_fault();
        }

        if (first_sample && telemetry_ready &&
            sample_count >= SAMPLES_BEFORE_ERROR_CHECK)
        {
            first_sample = false;
            ESP_LOGI(INVERTER_ADC_DRIVER_TAG, "First valid sample: Battery=%.2fV",
                     sys_state.inverter.battery.voltage);
            /* ADC warmup must not cut the startup presentation short. */
            if (lcd_task_handle != NULL)
                xTaskNotifyGive(lcd_task_handle);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Cleanup (unreachable in current implementation, but good practice)
    adc_resources_cleanup(adc1_handle, adc1_states, config_count);
    free(adc1_states);

#if EXAMPLE_USE_ADC2
    adc_resources_cleanup(adc2_handle, adc2_states, config_count);
    free(adc2_states);
#endif
}



/*---------------------------------------------------------------
        ADC Calibration Function (Advanced)
---------------------------------------------------------------*/





static bool configure_adc_channels(adc_oneshot_unit_handle_t handle,
                                   const adc_channel_config_t *configs,
                                   adc_channel_state_t *states,
                                   int channel_count,
                                   adc_unit_t unit_id)
{
    if (handle == NULL || configs == NULL || states == NULL)
    {
        ESP_LOGE(INVERTER_ADC_DRIVER_TAG, "Invalid parameters for channel configuration");
        return false;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_USED};

    bool at_least_one_success = false;

    for (int i = 0; i < channel_count; i++)
    {
        adc_channel_t channel = configs[i].channel;

        // Initialize state
        states[i].cali_handle = NULL;
        states[i].is_calibrated = false;

        // Configure the channel
        esp_err_t ret = adc_oneshot_config_channel(handle, channel, &chan_cfg);
        if (ret != ESP_OK)
        {
            ESP_LOGW(INVERTER_ADC_DRIVER_TAG, "Failed to configure %s (channel %d): %s",
                     configs[i].name, channel, esp_err_to_name(ret));
            continue;
        }

        // Try to initialize calibration for successfully configured channels
        states[i].is_calibrated = adc_calibration_init(unit_id, channel, ADC_ATTEN_USED,
                                                       &states[i].cali_handle);

        if (states[i].is_calibrated)
        {
            ESP_LOGI(INVERTER_ADC_DRIVER_TAG, "%s (channel %d) configured and calibrated successfully",
                     configs[i].name, channel);
        }
        else
        {
            ESP_LOGW(INVERTER_ADC_DRIVER_TAG, "%s (channel %d) configured but calibration failed - using raw conversion",
                     configs[i].name, channel);
        }

        at_least_one_success = true;
    }

    return at_least_one_success;
}



/*==============================================================================
  ESP32 ADC VOLTAGE RANGE - WHY 0.4V to 3.12V instead of 0V to 3.3V?
==============================================================================*/

/* THE PROBLEM:
   Potentiometer connected to 3.3V reads:
   - Minimum: 0.4V (should be 0V)
   - Maximum: 3.12V (should be 3.3V)

   WHY? ESP32 ADC at 11dB attenuation has limited linear range:
   - Theoretical: 0-3.3V
   - Actual usable: ~0.15V to 2.45-3.1V (varies by chip)
   - Below 0.15V and above 3.0V: Non-linear and inaccurate
*/

// These values are based on your actual measurements
// Adjust if your specific ESP32 chip shows different values
#define ADC_MEASURED_MIN 0.4f  // Minimum voltage ADC can read accurately
#define ADC_MEASURED_MAX 3.12f // Maximum voltage ADC can read accurately
#define ADC_TARGET_MIN 0.0f    // Target minimum (what you want)
#define ADC_TARGET_MAX 3.3f    // Target maximum (what you want)

// Enable/disable mapping per channel (in case some channels don't need it)
#define ENABLE_ADC_RANGE_MAPPING 1

/*------------------------------------------------------------------------------
  QUICK FIX: Software Mapping (No Hardware Changes)
------------------------------------------------------------------------------*/

/**
 * @brief Maps limited ADC range to full target range
 *
 * ESP32 ADC at 11dB attenuation reads 0.4V-3.12V instead of 0-3.3V.
 * This function maps the limited range to the full 0-3.3V range.
 *
 * @param adc_voltage Raw voltage from ADC (0.4-3.12V)
 * @return Mapped voltage (0-3.3V)
 */
static float map_adc_to_full_range(float adc_voltage)
{
#if ENABLE_ADC_RANGE_MAPPING
    // Clamp input to measured range
    if (adc_voltage < ADC_MEASURED_MIN)
    {
        adc_voltage = ADC_MEASURED_MIN;
    }
    if (adc_voltage > ADC_MEASURED_MAX)
    {
        adc_voltage = ADC_MEASURED_MAX;
    }

    // Linear mapping: y = (x - x_min) / (x_max - x_min) * (y_max - y_min) + y_min
    float mapped = ((adc_voltage - ADC_MEASURED_MIN) /
                    (ADC_MEASURED_MAX - ADC_MEASURED_MIN)) *
                       (ADC_TARGET_MAX - ADC_TARGET_MIN) +
                   ADC_TARGET_MIN;

    return mapped;
#else
    return adc_voltage; // No mapping
#endif
}

static float selected_battery_voltage_multiplier(void)
{
    float nominal_voltage = sys_state.battery_profile.nominal_voltage;
    if (nominal_voltage < 11.0f)
    {
        nominal_voltage = (float)sys_state.battery_voltage_system;
    }
    if (nominal_voltage < 11.0f)
    {
        nominal_voltage = 12.0f;
    }
    return nominal_voltage / 12.0f;
}

static telemetry_channel_t health_channel_for_adc(adc_channel_id_t channel_id)
{
    switch (channel_id) {
    case CHANNEL_ID_BATTERY_VOLTAGE:
        return TELEMETRY_CHANNEL_BATTERY_VOLTAGE;
    case CHANNEL_ID_OVER_UNDER_VOLTAGE:
        return TELEMETRY_CHANNEL_AC_VOLTAGE;
    case CHANNEL_ID_INVERTER_OUTPUT_VOLTAGE:
        return TELEMETRY_CHANNEL_INVERTER_OUTPUT_VOLTAGE;
    case CHANNEL_ID_LOW_BATTERY:
    case CHANNEL_ID_FAN:
    default:
        return TELEMETRY_CHANNEL_LOW_BATTERY;
    }
}

static void process_adc_reading(const adc_channel_config_t *config,
                                const adc_channel_state_t *state,
                                adc_oneshot_unit_handle_t handle)
{

    if (config == NULL || state == NULL)
    {
        ESP_LOGE(INVERTER_ADC_DRIVER_TAG, "Invalid parameters for ADC processing");
        return;
    }

    // Read ADC with multisampling
    float adc_voltage = 0.0f;
    esp_err_t ret = adc_read_with_multisampling(
        handle,
        config->channel,
        state->cali_handle,
        state->is_calibrated,
        &adc_voltage,
        ADC_MULTISAMPLING_COUNT);

    if (ret != ESP_OK)
    {
        ESP_LOGE(INVERTER_ADC_DRIVER_TAG, "Failed to read %s: %s", config->name, esp_err_to_name(ret));
        telemetry_health_record_invalid(
            health_channel_for_adc(config->channel_id),
            (uint32_t)(esp_timer_get_time() / 1000ULL));
        return;
    }

    // Validate voltage divider ratio
    if (config->voltage_divider_ratio <= 0.0f)
    {
        ESP_LOGE(INVERTER_ADC_DRIVER_TAG, "%s: Invalid voltage divider ratio: %.4f",
                 config->name, config->voltage_divider_ratio);
        return;
    }

    adc_voltage = map_adc_to_full_range(adc_voltage); // Apply software mapping to correct non-linearity

    // Convert the ADC pin voltage through the physical divider first.
    // The battery channel is a 12 V-equivalent analog input; scale that
    // measured value to the selected 12/24/48 V battery system afterwards.
    float actual_voltage = adc_voltage * config->voltage_divider_ratio;
    float threshold_low = config->threshold_low;
    if (config->channel_id == CHANNEL_ID_BATTERY_VOLTAGE)
    {
        const float multiplier = selected_battery_voltage_multiplier();
        actual_voltage *= multiplier;
        threshold_low = sys_state.battery_profile.cutoff_voltage_12v;
        if (threshold_low <= 0.0f)
        {
            threshold_low = config->threshold_low * multiplier;
        }
        battery_filter_update(&battery_voltage_filter, actual_voltage);
    }

    /* Reject physically implausible values before treating them as healthy.
     * The battery limit is derived from the active chemistry and voltage
     * system; the other channels remain bounded by their hardware ranges. */
    float telemetry_min = 0.0f;
    float telemetry_max = 350.0f;
    if (config->channel_id == CHANNEL_ID_BATTERY_VOLTAGE) {
        telemetry_min = sys_state.battery_profile.cutoff_voltage_min_12v * 0.50f;
        telemetry_max = sys_state.battery_profile.overvoltage_protection_12v *
                        BATTERY_ADC_PHYSICAL_MARGIN;
    } else if (config->channel_id == CHANNEL_ID_LOW_BATTERY) {
        telemetry_min = sys_state.battery_profile.cutoff_voltage_min_12v * 0.50f;
        telemetry_max = sys_state.battery_profile.overvoltage_protection_12v *
                        BATTERY_ADC_PHYSICAL_MARGIN;
    } else if (config->channel_id == CHANNEL_ID_INVERTER_OUTPUT_VOLTAGE) {
        telemetry_max = AC_ADC_PHYSICAL_MAX_V;
    }

    const uint32_t sample_time_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    const bool telemetry_valid = telemetry_health_record(
        health_channel_for_adc(config->channel_id), actual_voltage,
        telemetry_min, telemetry_max, sample_time_ms);
    if (!telemetry_valid) {
        ESP_LOGW(INVERTER_ADC_DRIVER_TAG, "%s sample outside safe range: %.2fV [%.2f, %.2f]",
                 config->name, actual_voltage, telemetry_min, telemetry_max);
    }

    // IMPORTANT: Write the system-scaled value to sys_state.
    *(config->target_value) = actual_voltage;

    // Check thresholds and set error flags using the active voltage system.
    bool error_detected = false;

    if (config->has_high_threshold)
    {
        error_detected = (actual_voltage < threshold_low ||
                          actual_voltage > config->threshold_high);
    }
    else
    {
        error_detected = (actual_voltage < threshold_low);
    }

    // Update error flags
    if (error_detected)
    {
        sys_state.error.error_flags |= config->error_flag;

        char threshold_high_str[16];
        if (config->has_high_threshold)
        {
            snprintf(threshold_high_str, sizeof(threshold_high_str), "%.2f", config->threshold_high);
        }
        else
        {
            snprintf(threshold_high_str, sizeof(threshold_high_str), "N/A");
        }
    }
    else
    {
        sys_state.error.error_flags &= ~config->error_flag;
    }
}
