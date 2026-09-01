#include "adc/adc_manager.h"
#include "adc/adc_driver.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "app/app_runtime.h"
#include "battery/battery_estimator.h"
#include "battery/battery_filter.h"
#include "cloud/cloud_reporting.h"
#include "events/protection_handler.h"
#include "lcd/lcd_writer.h"
#include "server/websocket/websocket_server.h"
#include "system/inverter_errors.h"
#include "system/task_watchdog.h"
#include "telemetry/telemetry_health.h"
#include "wifi/wifi_monitor.h"

#define ADC_MULTISAMPLING_COUNT 10U
#define TELEMETRY_STALE_TIMEOUT_MS 1000U
/* Startup coordination must fail promptly when acquisition never produces a
 * valid required snapshot; app_main has a separate 10 s safety timeout, but
 * waiting for it leaves the LCD on HARDWARE CHECK unnecessarily. */
#define ADC_STARTUP_FAILURE_TIMEOUT_MS 2000U
#define BATTERY_ADC_PHYSICAL_MARGIN 1.15f
#define AC_ADC_PHYSICAL_MAX_V 350.0f
#define ADC_TASK_STACK_SIZE 4096U
#define ADC_TASK_PRIORITY 5U
#define ADC_MANAGER_DRIVER_TAG "ADC_INIT"

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

#define ADC_REQUIRED_MASK ((1UL << TELEMETRY_CHANNEL_BATTERY_VOLTAGE) | \
                          (1UL << TELEMETRY_CHANNEL_INVERTER_OUTPUT_VOLTAGE))
#define EVT_ADC_VALID (1U << 1)

enum
{
    ADC1_CHANNEL_LOW_BATTERY = ADC_CHANNEL_6,
    ADC1_CHANNEL_OVER_UNDER_VOLTAGE = ADC_CHANNEL_0,
    ADC1_CHANNEL_BATTERY_VOLTAGE = ADC_CHANNEL_7,
    ADC1_CHANNEL_OUTPUT_VOLTAGE = ADC_CHANNEL_4,
};

typedef enum
{
    CHANNEL_ID_LOW_BATTERY = 0,
    CHANNEL_ID_OVER_UNDER_VOLTAGE,
    CHANNEL_ID_BATTERY_VOLTAGE,
    CHANNEL_ID_INVERTER_OUTPUT_VOLTAGE,
    CHANNEL_ID_COUNT
} adc_channel_id_t;

typedef struct
{
    adc_channel_t channel;
    adc_channel_id_t channel_id;
    float *target_value;
    float threshold_low;
    float threshold_high;
    bool has_high_threshold;
    uint32_t error_flag;
    const char *name;
    float voltage_divider_ratio;
} adc_channel_config_t;

extern battery_estimator_t bat_estimate;
extern void check_protections(void);
extern void inverter_emergency_disable(const char *reason);
extern const char *get_error_string(uint32_t flags);

static battery_filter_t battery_voltage_filter;
static adc_manager_state_t adc_manager_state = ADC_MANAGER_STATE_RESET;
static portMUX_TYPE adc_manager_state_lock = portMUX_INITIALIZER_UNLOCKED;
static adc_manager_snapshot_t s_snapshot;
static adc_driver_channel_t adc_driver_channels[
    ADC_MANAGER_CHANNEL_COUNT];
static void *adc_driver_context;
static adc_driver_status_t adc_driver_status;
static adc_manager_measurement_t adc_manager_measurements[ADC_MANAGER_CHANNEL_COUNT];

static const adc_channel_config_t adc_manager_channel_configs[] = {
    {.channel = ADC1_CHANNEL_LOW_BATTERY,
     .channel_id = CHANNEL_ID_LOW_BATTERY,
     .target_value = &sys_state.inverter.low_bat_egs002_signal,
     .threshold_low = LOW_BATTERY_VOLTAGE_THRESHOLD,
     .has_high_threshold = false,
     .error_flag = ERR_LOW_BAT,
     .name = "Low Battery",
     .voltage_divider_ratio = LOW_BATTERY_DIVIDER_RATIO},
    {.channel = ADC1_CHANNEL_OVER_UNDER_VOLTAGE,
     .channel_id = CHANNEL_ID_OVER_UNDER_VOLTAGE,
     .target_value = &sys_state.inverter.over_under_voltage,
     .threshold_low = UNDER_VOLTAGE_THRESHOLD,
     .threshold_high = OVER_VOLTAGE_THRESHOLD,
     .has_high_threshold = true,
     .error_flag = ERR_AC_FAULT,
     .name = "AC Voltage",
     .voltage_divider_ratio = AC_VOLTAGE_DIVIDER_RATIO},
    {.channel = ADC1_CHANNEL_BATTERY_VOLTAGE,
     .channel_id = CHANNEL_ID_BATTERY_VOLTAGE,
     .target_value = &sys_state.inverter.battery.voltage,
     .threshold_low = BATTERY_VOLTAGE_THRESHOLD,
     .has_high_threshold = false,
     .error_flag = ERR_BATTERY_VOLTAGE,
     .name = "Battery Voltage",
     .voltage_divider_ratio = BATTERY_VOLTAGE_DIVIDER_RATIO},
    {.channel = ADC1_CHANNEL_OUTPUT_VOLTAGE,
     .channel_id = CHANNEL_ID_INVERTER_OUTPUT_VOLTAGE,
     .target_value = &sys_state.inverter.output_voltage,
     .threshold_low = INVERTER_OUTPUT_VOLTAGE_THRESHOLD,
     .has_high_threshold = false,
     .error_flag = ERR_INVERTER_VOLTAGE,
     .name = "Inverter Voltage",
     .voltage_divider_ratio = INVERTER_VOLTAGE_DIVIDER_RATIO},
};

#define ADC_CONFIG_COUNT (sizeof(adc_manager_channel_configs) / sizeof(adc_manager_channel_configs[0]))

#define ADC_MEASURED_MIN 0.4f
#define ADC_MEASURED_MAX 3.12f
#define ADC_TARGET_MIN 0.0f
#define ADC_TARGET_MAX 3.3f

static float clamp_float(float value, float minimum, float maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static float map_adc_to_full_range(float adc_voltage)
{
    if (adc_voltage < ADC_MEASURED_MIN) {
        adc_voltage = ADC_MEASURED_MIN;
    }
    if (adc_voltage > ADC_MEASURED_MAX) {
        adc_voltage = ADC_MEASURED_MAX;
    }
    return ((adc_voltage - ADC_MEASURED_MIN) /
            (ADC_MEASURED_MAX - ADC_MEASURED_MIN)) *
               (ADC_TARGET_MAX - ADC_TARGET_MIN) +
           ADC_TARGET_MIN;
}

static float selected_battery_voltage_multiplier(void)
{
    float nominal_voltage = sys_state.battery_profile.nominal_voltage;
    if (nominal_voltage < 11.0f) {
        nominal_voltage = (float)sys_state.battery_voltage_system;
    }
    if (nominal_voltage < 11.0f) {
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
    default:
        return TELEMETRY_CHANNEL_LOW_BATTERY;
    }
}

static void driver_status_refresh(void)
{
    adc_driver_runtime_t runtime = {
        .driver_state = ADC_DRIVER_UNINITIALIZED,
    };
    if (adc_driver_context == NULL ||
        adc_driver_get_runtime(adc_driver_context, &runtime) != ESP_OK) {
        runtime.driver_state = ADC_DRIVER_FAULT;
    }

    taskENTER_CRITICAL(&adc_manager_state_lock);
    adc_driver_status.driver_state = runtime.driver_state;
    adc_driver_status.frames_received = runtime.frames_received;
    adc_driver_status.frames_dropped = runtime.frames_dropped;
    adc_driver_status.pool_overflows = runtime.pool_overflows;
    taskEXIT_CRITICAL(&adc_manager_state_lock);
}

static void measurement_record(adc_channel_id_t channel_id,
                               float voltage,
                               uint32_t sample_count,
                               uint32_t timestamp_ms,
                               bool valid,
                               bool calibrated,
                               bool saturated)
{
    if (channel_id < 0 || channel_id >= CHANNEL_ID_COUNT) {
        return;
    }
    taskENTER_CRITICAL(&adc_manager_state_lock);
    adc_manager_measurement_t *measurement = &adc_manager_measurements[channel_id];
    measurement->voltage = voltage;
    measurement->sample_count = sample_count;
    measurement->timestamp_ms = timestamp_ms;
    measurement->valid = valid;
    measurement->calibrated = calibrated;
    measurement->fresh = valid;
    measurement->saturated = saturated;
    if (valid) {
        ++adc_driver_status.consecutive_successes;
        adc_driver_status.consecutive_failures = 0U;
        adc_driver_status.last_success_ms = timestamp_ms;
        if (adc_driver_status.consecutive_successes == 0U) {
            adc_driver_status.consecutive_successes = UINT32_MAX;
        }
    } else {
        ++measurement->error_count;
        ++adc_driver_status.invalid_samples;
        ++adc_driver_status.consecutive_failures;
        adc_driver_status.consecutive_successes = 0U;
    }
    if (saturated) {
        ++adc_driver_status.saturated_samples;
    }
    taskEXIT_CRITICAL(&adc_manager_state_lock);
}

static void measurement_record_read_error(adc_channel_id_t channel_id,
                                          uint32_t timestamp_ms,
                                          bool calibrated)
{
    if (channel_id < 0 || channel_id >= CHANNEL_ID_COUNT) {
        return;
    }
    taskENTER_CRITICAL(&adc_manager_state_lock);
    adc_manager_measurement_t *measurement = &adc_manager_measurements[channel_id];
    measurement->timestamp_ms = timestamp_ms;
    measurement->sample_count = 0U;
    measurement->valid = false;
    measurement->calibrated = calibrated;
    measurement->fresh = false;
    measurement->saturated = false;
    ++measurement->error_count;
    ++adc_driver_status.read_errors;
    ++adc_driver_status.invalid_samples;
    ++adc_driver_status.consecutive_failures;
    adc_driver_status.consecutive_successes = 0U;
    taskEXIT_CRITICAL(&adc_manager_state_lock);
}

static void adc_signal_failed(const char *reason)
{
    taskENTER_CRITICAL(&adc_manager_state_lock);
    adc_manager_state = ADC_MANAGER_STATE_FAILED;
    adc_driver_status.driver_state = ADC_DRIVER_FAULT;
    taskEXIT_CRITICAL(&adc_manager_state_lock);
    sys_state.adc_ready = false;
    sys_state.adc_data_valid = false;
    sys_state.inverter.adc_data_valid = false;
    if (sys_event_group != NULL) {
        xEventGroupSetBits(sys_event_group, APP_EVENT_ADC_FAILED);
    }
    ESP_LOGE(ADC_MANAGER_DRIVER_TAG, "ADC subsystem failed: %s", reason);
}

static void snapshot_update(uint32_t timestamp_ms, bool ready)
{
    adc_manager_snapshot_t snapshot = {
        .low_battery_voltage = sys_state.inverter.low_bat_egs002_signal,
        .ac_voltage = sys_state.inverter.over_under_voltage,
        .battery_voltage = sys_state.inverter.battery.voltage,
        .output_voltage = sys_state.inverter.output_voltage,
        .sequence = s_snapshot.sequence + 1U,
        .timestamp_ms = timestamp_ms,
        .required_data_valid = ready,
        .fresh = ready,
        .driver_state = adc_driver_status.driver_state,
        .driver_degraded = adc_driver_status.driver_state == ADC_DRIVER_FALLBACK,
        .driver_status = adc_driver_status,
    };
    taskENTER_CRITICAL(&adc_manager_state_lock);
    memcpy(snapshot.channel, adc_manager_measurements, sizeof(adc_manager_measurements));
    for (size_t i = 0U; i < ADC_MANAGER_CHANNEL_COUNT; ++i) {
        snapshot.channel[i].fresh = snapshot.channel[i].valid &&
            (uint32_t)(timestamp_ms - snapshot.channel[i].timestamp_ms) <=
                TELEMETRY_STALE_TIMEOUT_MS;
    }
    snapshot.driver_status = adc_driver_status;
    snapshot.driver_state = adc_driver_status.driver_state;
    snapshot.driver_degraded = adc_driver_status.driver_state == ADC_DRIVER_FALLBACK;
    s_snapshot = snapshot;
    taskEXIT_CRITICAL(&adc_manager_state_lock);
}

static bool process_adc_reading(const adc_channel_config_t *config,
                                const adc_driver_channel_t *driver_channel)
{
    if (config == NULL || driver_channel == NULL) {
        return false;
    }

    float adc_voltage = 0.0f;
    const esp_err_t read_result = adc_driver_read_sample(
        adc_driver_context, driver_channel, &adc_voltage);
    const uint32_t sample_time_ms =
        (uint32_t)(esp_timer_get_time() / 1000ULL);
    const telemetry_channel_t health_channel =
        health_channel_for_adc(config->channel_id);
    if (read_result != ESP_OK) {
        measurement_record_read_error(config->channel_id, sample_time_ms,
                                      driver_channel->channel_state.is_calibrated);
        telemetry_health_record_invalid(health_channel, sample_time_ms);
        ESP_LOGW(ADC_MANAGER_DRIVER_TAG,
                 "%s sample acquisition failed: %s",
                 config->name, esp_err_to_name(read_result));
        return false;
    }
    if (config->voltage_divider_ratio <= 0.0f || !isfinite(adc_voltage)) {
        telemetry_health_record_invalid(health_channel, sample_time_ms);
        ESP_LOGE(ADC_MANAGER_DRIVER_TAG, "%s has invalid ADC conversion", config->name);
        return false;
    }

    const bool saturated = adc_voltage >= (ADC_MEASURED_MAX * 0.995f);
    adc_voltage = map_adc_to_full_range(adc_voltage);
    float actual_voltage = adc_voltage * config->voltage_divider_ratio;
    float threshold_low = config->threshold_low;
    if (config->channel_id == CHANNEL_ID_BATTERY_VOLTAGE) {
        actual_voltage *= selected_battery_voltage_multiplier();
        threshold_low = sys_state.battery_profile.cutoff_voltage_12v;
        if (threshold_low <= 0.0f) {
            threshold_low = config->threshold_low * selected_battery_voltage_multiplier();
        }
        battery_filter_update(&battery_voltage_filter, actual_voltage);
    }

    float telemetry_min = 0.0f;
    float telemetry_max = 350.0f;
    if (config->channel_id == CHANNEL_ID_BATTERY_VOLTAGE ||
        config->channel_id == CHANNEL_ID_LOW_BATTERY) {
        telemetry_min = sys_state.battery_profile.cutoff_voltage_min_12v * 0.50f;
        telemetry_max = sys_state.battery_profile.overvoltage_protection_12v *
                        BATTERY_ADC_PHYSICAL_MARGIN;
    } else if (config->channel_id == CHANNEL_ID_INVERTER_OUTPUT_VOLTAGE) {
        telemetry_max = AC_ADC_PHYSICAL_MAX_V;
    }

    const bool telemetry_valid = telemetry_health_record(
        health_channel, actual_voltage, telemetry_min, telemetry_max,
        sample_time_ms);
    *(config->target_value) = actual_voltage;
    measurement_record(config->channel_id, actual_voltage,
                       ADC_MULTISAMPLING_COUNT, sample_time_ms,
                       telemetry_valid, driver_channel->channel_state.is_calibrated,
                       saturated);

    const bool error_detected = config->has_high_threshold
                                    ? (actual_voltage < threshold_low ||
                                       actual_voltage > config->threshold_high)
                                    : (actual_voltage < threshold_low);
    if (error_detected) {
        sys_state.error.error_flags |= config->error_flag;
    } else {
        sys_state.error.error_flags &= ~config->error_flag;
    }
    if (!telemetry_valid) {
        ESP_LOGW(ADC_MANAGER_DRIVER_TAG,
                 "%s sample outside safe range: %.2fV [%.2f, %.2f]",
                 config->name, actual_voltage, telemetry_min, telemetry_max);
    }
    return telemetry_valid;
}

static void update_snapshot_and_outputs(uint32_t sample_time_ms,
                                        bool telemetry_ready,
                                        uint8_t sample_count,
                                        uint32_t *last_ws_publish_ms)
{
    sys_state.adc_data_valid = telemetry_ready;
    sys_state.inverter.adc_data_valid = telemetry_ready;
    snapshot_update(sample_time_ms, telemetry_ready);

    const float battery_soc = clamp_float(
        battery_estimator_get_soc(&bat_estimate), 0.0f, 100.0f);
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
                            ? sys_state.dc_input_voltage * sys_state.dc_input_current / 1000.0f
                            : 0.0f;
    const float load_kw = (sys_state.inverter.output_voltage > 0.0f &&
                           sys_state.inverter.output_current > 0.0f)
                              ? sys_state.inverter.output_voltage *
                                    sys_state.inverter.output_current / 1000.0f
                              : 0.0f;
    uint16_t remaining_minutes = 0U;
    const float remaining_ah = battery_estimator_get_remaining_ah(&bat_estimate);
    const float battery_voltage = sys_state.inverter.battery.voltage;
    const float efficiency = (sys_state.efficiency > 0.50f &&
                              sys_state.efficiency <= 1.0f)
                                 ? sys_state.efficiency
                                 : 0.90f;
    if (remaining_ah > 0.05f && load_kw > 0.02f && battery_voltage > 5.0f) {
        const float battery_current_a =
            (load_kw * 1000.0f) / (battery_voltage * efficiency);
        if (battery_current_a > 0.05f) {
            const float minutes = (remaining_ah / battery_current_a) * 60.0f;
            remaining_minutes = (minutes >= 65535.0f) ? UINT16_MAX : (uint16_t)minutes;
        }
    }

    lcd_update_main_power(pv_kw, 0.0f, load_kw,
                          sys_state.inverter.output_voltage,
                          remaining_minutes,
                          (uint8_t)sys_state.battery_profile.nominal_voltage,
                          sys_state.inverter.operating_mode);
    lcd_update_wifi_status(wifi_monitor_is_online(), wifi_monitor_get_rssi());
    if ((uint32_t)(sample_time_ms - *last_ws_publish_ms) >= 1000U) {
        *last_ws_publish_ms = sample_time_ms;
        websocket_broadcast_device_status();
        cloud_reporting_publish(&sys_state, pv_kw, load_kw,
                                wifi_monitor_get_rssi());
    }

    if (sys_lcd.screen == LCD_SCREEN_STANDBY) {
        lcd_show_standby(sys_state.inverter.battery.voltage,
                         battery_pct, sys_state.inverter.connected);
    }
    if (sys_state.error.error_flags && telemetry_ready &&
        sample_count >= ADC_MULTISAMPLING_COUNT) {
        const char *err = get_error_string(sys_state.error.error_flags);
        char l0[LCD_LINE_SIZE], l1[LCD_LINE_SIZE];
        snprintf(l0, LCD_LINE_SIZE, "%-16.16s", err);
        snprintf(l1, LCD_LINE_SIZE, "%-16s", "Check system    ");
        lcd_show_fault(l0, l1);
    } else if (sys_lcd.screen == LCD_SCREEN_FAULT) {
        lcd_clear_fault();
    }
}

static void adc_task_body(void)
{
    task_watchdog_register("adc_task");
    telemetry_health_init();
    telemetry_health_set_required_mask(ADC_REQUIRED_MASK);
    battery_filter_init(&battery_voltage_filter, 0.20f);
    taskENTER_CRITICAL(&adc_manager_state_lock);
    memset(&adc_driver_status, 0, sizeof(adc_driver_status));
    memset(adc_manager_measurements, 0, sizeof(adc_manager_measurements));
    adc_driver_status.driver_state = ADC_DRIVER_UNINITIALIZED;
    taskEXIT_CRITICAL(&adc_manager_state_lock);

    const adc_channel_t channels[ADC_CONFIG_COUNT] = {
        ADC1_CHANNEL_LOW_BATTERY,
        ADC1_CHANNEL_OVER_UNDER_VOLTAGE,
        ADC1_CHANNEL_BATTERY_VOLTAGE,
        ADC1_CHANNEL_OUTPUT_VOLTAGE,
    };
    const bool init_result = adc_driver_init(
        channels, ADC_CONFIG_COUNT, adc_driver_channels, &adc_driver_context);
    if (init_result != ESP_OK) {
        adc_signal_failed(esp_err_to_name(init_result));
        task_watchdog_unregister();
        vTaskDelete(NULL);
        return;
    }

    taskENTER_CRITICAL(&adc_manager_state_lock);
    adc_manager_state = ADC_MANAGER_STATE_RUNNING;
    taskEXIT_CRITICAL(&adc_manager_state_lock);
    driver_status_refresh();
    ESP_LOGI(ADC_MANAGER_DRIVER_TAG, "ADC %s driver initialized",
             adc_driver_get_name());

    bool telemetry_shutdown_latched = false;
    uint8_t sample_count = 0U;
    uint32_t last_ws_publish_ms = 0U;
    bool readiness_reported = false;
    bool startup_failure_reported = false;
    const uint32_t startup_started_ms =
        (uint32_t)(esp_timer_get_time() / 1000ULL);

    while (true) {
        task_watchdog_feed();
        for (size_t i = 0U; i < ADC_CONFIG_COUNT; ++i) {
            (void)process_adc_reading(&adc_manager_channel_configs[i], &adc_driver_channels[i]);
        }

        driver_status_refresh();
        const uint32_t sample_time_ms =
            (uint32_t)(esp_timer_get_time() / 1000ULL);
        const bool telemetry_ready = telemetry_health_required_ready(
            sample_time_ms, TELEMETRY_STALE_TIMEOUT_MS);

        /* A driver can initialize successfully yet return timeouts forever.
         * That is not a normal running state during startup: it must publish a
         * terminal failure so app_main leaves its wait immediately. Never set
         * ADC_READY here; readiness still requires valid and fresh telemetry. */
        if (!telemetry_ready && !readiness_reported &&
            !startup_failure_reported &&
            (uint32_t)(sample_time_ms - startup_started_ms) >=
                ADC_STARTUP_FAILURE_TIMEOUT_MS) {
            startup_failure_reported = true;
            adc_signal_failed("required telemetry did not become valid/fresh during startup deadline");
        }

        if (telemetry_ready && sample_count < ADC_MULTISAMPLING_COUNT) {
            sys_state.error.error_flags = 0U;
            ++sample_count;
            ESP_LOGI(ADC_MANAGER_DRIVER_TAG, "ADC warmup: %u/%u",
                     sample_count, ADC_MULTISAMPLING_COUNT);
        }

        if (telemetry_ready && !readiness_reported) {
            readiness_reported = true;
            taskENTER_CRITICAL(&adc_manager_state_lock);
            adc_manager_state = ADC_MANAGER_STATE_READY;
            taskEXIT_CRITICAL(&adc_manager_state_lock);
            sys_state.adc_ready = true;
            if (sys_event_group != NULL) {
                xEventGroupSetBits(sys_event_group, APP_EVENT_ADC_READY);
            }
            ESP_LOGI(ADC_MANAGER_DRIVER_TAG,
                     "ADC ready: required telemetry is valid and fresh");
        }

        if (telemetry_ready) {
            telemetry_shutdown_latched = false;
            if (sys_event_group != NULL) {
                xEventGroupSetBits(sys_event_group, EVT_ADC_VALID);
            }
        } else {
            sys_state.adc_ready = readiness_reported;
            if (sys_event_group != NULL) {
                xEventGroupClearBits(sys_event_group, EVT_ADC_VALID);
            }
            sys_state.error.error_flags |= ERR_BATTERY_VOLTAGE;
            if (!telemetry_shutdown_latched &&
                (sys_state.inverter.inverter_active ||
                 sys_state.inverter.inverter_state == INVERTER_STARTING)) {
                telemetry_shutdown_latched = true;
                inverter_emergency_disable("required ADC telemetry invalid or stale");
            }
        }

        if (telemetry_ready && sample_count >= ADC_MULTISAMPLING_COUNT) {
            check_protections();
        }
        update_snapshot_and_outputs(sample_time_ms, telemetry_ready,
                                    sample_count, &last_ws_publish_ms);

        if (readiness_reported && sample_count >= ADC_MULTISAMPLING_COUNT &&
            !sys_state.inverter.adc_data_valid) {
            /* The data has gone stale after a prior successful boot. The event
             * remains a historical boot completion marker, while the snapshot
             * and interlock flags immediately report the unsafe condition. */
            ESP_LOGW(ADC_MANAGER_DRIVER_TAG, "ADC telemetry lost freshness");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void adc_task(void *arg)
{
    (void)arg;
    adc_task_body();
}

esp_err_t adc_manager_start(void)
{
    taskENTER_CRITICAL(&adc_manager_state_lock);
    const adc_manager_state_t current = adc_manager_state;
    if (current == ADC_MANAGER_STATE_INITIALIZING ||
        current == ADC_MANAGER_STATE_RUNNING ||
        current == ADC_MANAGER_STATE_READY) {
        taskEXIT_CRITICAL(&adc_manager_state_lock);
        return ESP_OK;
    }
    if (current == ADC_MANAGER_STATE_FAILED) {
        taskEXIT_CRITICAL(&adc_manager_state_lock);
        return ESP_FAIL;
    }
    adc_manager_state = ADC_MANAGER_STATE_INITIALIZING;
    taskEXIT_CRITICAL(&adc_manager_state_lock);

    const BaseType_t result = xTaskCreate(
        adc_task, "adc_task", ADC_TASK_STACK_SIZE, NULL,
        ADC_TASK_PRIORITY, NULL);
    if (result != pdPASS) {
        adc_signal_failed("ADC task creation failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

adc_manager_state_t adc_manager_get_state(void)
{
    taskENTER_CRITICAL(&adc_manager_state_lock);
    const adc_manager_state_t state = adc_manager_state;
    taskEXIT_CRITICAL(&adc_manager_state_lock);
    return state;
}

bool adc_manager_is_ready(void)
{
    return adc_manager_get_state() == ADC_MANAGER_STATE_READY;
}

esp_err_t adc_manager_get_snapshot(adc_manager_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&adc_manager_state_lock);
    *out = s_snapshot;
    taskEXIT_CRITICAL(&adc_manager_state_lock);
    return ESP_OK;
}

adc_manager_mode_t adc_manager_get_mode(void)
{
#if ADC_MANAGER_MODE == ADC_MANAGER_MODE_CONTINUOUS
    return ADC_MANAGER_MODE_CONTINUOUS_ENUM;
#else
    return ADC_MANAGER_MODE_ONESHOT_ENUM;
#endif
}

esp_err_t adc_manager_get_driver_status(adc_driver_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&adc_manager_state_lock);
    *out = adc_driver_status;
    taskEXIT_CRITICAL(&adc_manager_state_lock);
    return ESP_OK;
}

esp_err_t adc_manager_get_measurement(adc_manager_channel_t channel,
                                       adc_manager_measurement_t *out)
{
    if (out == NULL || channel < 0 || channel >= ADC_MANAGER_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    taskENTER_CRITICAL(&adc_manager_state_lock);
    *out = adc_manager_measurements[channel];
    out->fresh = out->valid &&
                 (uint32_t)(now_ms - out->timestamp_ms) <=
                     TELEMETRY_STALE_TIMEOUT_MS;
    taskEXIT_CRITICAL(&adc_manager_state_lock);
    return ESP_OK;
}
