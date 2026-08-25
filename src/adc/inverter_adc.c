#include "adc/inverter_adc.h"
#include "adc/inverter_adc_backend.h"

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
#define BATTERY_ADC_PHYSICAL_MARGIN 1.15f
#define AC_ADC_PHYSICAL_MAX_V 350.0f
#define ADC_TASK_STACK_SIZE 4096U
#define ADC_TASK_PRIORITY 5U
#define INVERTER_ADC_DRIVER_TAG "ADC_INIT"

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
    INVERTER_ADC_LOW_BATTERY = ADC_CHANNEL_6,
    INVERTER_ADC_OVER_UNDER_VOLTAGE = ADC_CHANNEL_0,
    INVERTER_ADC_BATTERY_VOLTAGE = ADC_CHANNEL_7,
    INVERTER_ADC_OUTPUT_VOLTAGE = ADC_CHANNEL_4,
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
static inverter_adc_state_t s_state = INVERTER_ADC_STATE_RESET;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static inverter_adc_snapshot_t s_snapshot;
static inverter_adc_backend_channel_t s_backend_channels[
    INVERTER_ADC_CHANNEL_COUNT];
static void *s_backend_context;

static const adc_channel_config_t s_adc_configs[] = {
    {.channel = INVERTER_ADC_LOW_BATTERY,
     .channel_id = CHANNEL_ID_LOW_BATTERY,
     .target_value = &sys_state.inverter.low_bat_egs002_signal,
     .threshold_low = LOW_BATTERY_VOLTAGE_THRESHOLD,
     .has_high_threshold = false,
     .error_flag = ERR_LOW_BAT,
     .name = "Low Battery",
     .voltage_divider_ratio = LOW_BATTERY_DIVIDER_RATIO},
    {.channel = INVERTER_ADC_OVER_UNDER_VOLTAGE,
     .channel_id = CHANNEL_ID_OVER_UNDER_VOLTAGE,
     .target_value = &sys_state.inverter.over_under_voltage,
     .threshold_low = UNDER_VOLTAGE_THRESHOLD,
     .threshold_high = OVER_VOLTAGE_THRESHOLD,
     .has_high_threshold = true,
     .error_flag = ERR_AC_FAULT,
     .name = "AC Voltage",
     .voltage_divider_ratio = AC_VOLTAGE_DIVIDER_RATIO},
    {.channel = INVERTER_ADC_BATTERY_VOLTAGE,
     .channel_id = CHANNEL_ID_BATTERY_VOLTAGE,
     .target_value = &sys_state.inverter.battery.voltage,
     .threshold_low = BATTERY_VOLTAGE_THRESHOLD,
     .has_high_threshold = false,
     .error_flag = ERR_BATTERY_VOLTAGE,
     .name = "Battery Voltage",
     .voltage_divider_ratio = BATTERY_VOLTAGE_DIVIDER_RATIO},
    {.channel = INVERTER_ADC_OUTPUT_VOLTAGE,
     .channel_id = CHANNEL_ID_INVERTER_OUTPUT_VOLTAGE,
     .target_value = &sys_state.inverter.output_voltage,
     .threshold_low = INVERTER_OUTPUT_VOLTAGE_THRESHOLD,
     .has_high_threshold = false,
     .error_flag = ERR_INVERTER_VOLTAGE,
     .name = "Inverter Voltage",
     .voltage_divider_ratio = INVERTER_VOLTAGE_DIVIDER_RATIO},
};

#define ADC_CONFIG_COUNT (sizeof(s_adc_configs) / sizeof(s_adc_configs[0]))

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

static void adc_signal_failed(const char *reason)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_state = INVERTER_ADC_STATE_FAILED;
    taskEXIT_CRITICAL(&s_state_lock);
    sys_state.adc_ready = false;
    sys_state.adc_data_valid = false;
    sys_state.inverter.adc_data_valid = false;
    if (sys_event_group != NULL) {
        xEventGroupSetBits(sys_event_group, APP_EVENT_ADC_FAILED);
    }
    ESP_LOGE(INVERTER_ADC_DRIVER_TAG, "ADC subsystem failed: %s", reason);
}

static void snapshot_update(uint32_t timestamp_ms, bool ready)
{
    inverter_adc_snapshot_t snapshot = {
        .low_battery_voltage = sys_state.inverter.low_bat_egs002_signal,
        .ac_voltage = sys_state.inverter.over_under_voltage,
        .battery_voltage = sys_state.inverter.battery.voltage,
        .output_voltage = sys_state.inverter.output_voltage,
        .sequence = s_snapshot.sequence + 1U,
        .timestamp_ms = timestamp_ms,
        .required_data_valid = ready,
        .fresh = ready,
    };
    taskENTER_CRITICAL(&s_state_lock);
    s_snapshot = snapshot;
    taskEXIT_CRITICAL(&s_state_lock);
}

static bool process_adc_reading(const adc_channel_config_t *config,
                                const inverter_adc_backend_channel_t *backend_state)
{
    if (config == NULL || backend_state == NULL) {
        return false;
    }

    float adc_voltage = 0.0f;
    const esp_err_t read_result = inverter_adc_backend_read_sample(
        s_backend_context, backend_state, &adc_voltage);
    const uint32_t sample_time_ms =
        (uint32_t)(esp_timer_get_time() / 1000ULL);
    const telemetry_channel_t health_channel =
        health_channel_for_adc(config->channel_id);
    if (read_result != ESP_OK) {
        telemetry_health_record_invalid(health_channel, sample_time_ms);
        ESP_LOGW(INVERTER_ADC_DRIVER_TAG,
                 "%s sample acquisition failed: %s",
                 config->name, esp_err_to_name(read_result));
        return false;
    }
    if (config->voltage_divider_ratio <= 0.0f || !isfinite(adc_voltage)) {
        telemetry_health_record_invalid(health_channel, sample_time_ms);
        ESP_LOGE(INVERTER_ADC_DRIVER_TAG, "%s has invalid ADC conversion", config->name);
        return false;
    }

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
        ESP_LOGW(INVERTER_ADC_DRIVER_TAG,
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

    const adc_channel_t channels[ADC_CONFIG_COUNT] = {
        INVERTER_ADC_LOW_BATTERY,
        INVERTER_ADC_OVER_UNDER_VOLTAGE,
        INVERTER_ADC_BATTERY_VOLTAGE,
        INVERTER_ADC_OUTPUT_VOLTAGE,
    };
    const esp_err_t init_result = inverter_adc_backend_init(
        channels, ADC_CONFIG_COUNT, s_backend_channels, &s_backend_context);
    if (init_result != ESP_OK) {
        adc_signal_failed(esp_err_to_name(init_result));
        vTaskDelete(NULL);
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_state = INVERTER_ADC_STATE_RUNNING;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(INVERTER_ADC_DRIVER_TAG, "ADC %s backend initialized",
             inverter_adc_backend_name());

    bool telemetry_shutdown_latched = false;
    uint8_t sample_count = 0U;
    uint32_t last_ws_publish_ms = 0U;
    bool readiness_reported = false;

    while (true) {
        task_watchdog_feed();
        for (size_t i = 0U; i < ADC_CONFIG_COUNT; ++i) {
            (void)process_adc_reading(&s_adc_configs[i], &s_backend_channels[i]);
        }

        const uint32_t sample_time_ms =
            (uint32_t)(esp_timer_get_time() / 1000ULL);
        const bool telemetry_ready = telemetry_health_required_ready(
            sample_time_ms, TELEMETRY_STALE_TIMEOUT_MS);
        if (telemetry_ready && sample_count < ADC_MULTISAMPLING_COUNT) {
            sys_state.error.error_flags = 0U;
            ++sample_count;
            ESP_LOGI(INVERTER_ADC_DRIVER_TAG, "ADC warmup: %u/%u",
                     sample_count, ADC_MULTISAMPLING_COUNT);
        }

        if (telemetry_ready && !readiness_reported) {
            readiness_reported = true;
            taskENTER_CRITICAL(&s_state_lock);
            s_state = INVERTER_ADC_STATE_READY;
            taskEXIT_CRITICAL(&s_state_lock);
            sys_state.adc_ready = true;
            if (sys_event_group != NULL) {
                xEventGroupSetBits(sys_event_group, APP_EVENT_ADC_READY);
            }
            ESP_LOGI(INVERTER_ADC_DRIVER_TAG,
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
            ESP_LOGW(INVERTER_ADC_DRIVER_TAG, "ADC telemetry lost freshness");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void adc_task(void *arg)
{
    (void)arg;
    adc_task_body();
}

esp_err_t inverter_adc_start(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const inverter_adc_state_t current = s_state;
    if (current == INVERTER_ADC_STATE_INITIALIZING ||
        current == INVERTER_ADC_STATE_RUNNING ||
        current == INVERTER_ADC_STATE_READY) {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    if (current == INVERTER_ADC_STATE_FAILED) {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_FAIL;
    }
    s_state = INVERTER_ADC_STATE_INITIALIZING;
    taskEXIT_CRITICAL(&s_state_lock);

    const BaseType_t result = xTaskCreate(
        adc_task, "adc_task", ADC_TASK_STACK_SIZE, NULL,
        ADC_TASK_PRIORITY, NULL);
    if (result != pdPASS) {
        adc_signal_failed("ADC task creation failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

inverter_adc_state_t inverter_adc_get_state(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const inverter_adc_state_t state = s_state;
    taskEXIT_CRITICAL(&s_state_lock);
    return state;
}

bool inverter_adc_is_ready(void)
{
    return inverter_adc_get_state() == INVERTER_ADC_STATE_READY;
}

esp_err_t inverter_adc_get_snapshot(inverter_adc_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    *out = s_snapshot;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

inverter_adc_mode_t inverter_adc_get_mode(void)
{
#if INVERTER_ADC_MODE == INVERTER_ADC_MODE_CONTINUOUS
    return INVERTER_ADC_MODE_CONTINUOUS_ENUM;
#else
    return INVERTER_ADC_MODE_ONESHOT_ENUM;
#endif
}
