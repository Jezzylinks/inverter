#include "post_adc.h"

#include "esp_log.h"
#include "system_state.h"

extern system_state_t sys_state;

static const char *TAG = "POST_ADC";

/* Output should be silent before the inverter has ever been started. */
#define POST_MAX_OUTPUT_VOLTAGE_IDLE 5.0f

bool post_adc_test(void)
{
    bool all_passed = true;

    /* Battery voltage plausibility -- bounds derived from the active,
     * chemistry+voltage-system-scaled battery profile (NOT a fixed
     * 12V-class range), so this works correctly whether the inverter is
     * configured for 12/24/48V, lead-acid or lithium. A reading well
     * below cutoff or above the hard overvoltage limit means the sensor
     * is disconnected, shorted, or wildly out of calibration -- not a
     * real battery condition. */
    float battery_voltage = sys_state.inverter.battery.voltage;
    float plausible_min = sys_state.battery_profile.cutoff_voltage_12v * 0.7f;
    float plausible_max = sys_state.battery_profile.overvoltage_protection_12v * 1.1f;

    if (battery_voltage < plausible_min || battery_voltage > plausible_max)
    {
        ESP_LOGE(TAG, "Battery voltage implausible: %.2fV (expected %.2f-%.2fV)",
                 battery_voltage, plausible_min, plausible_max);
        all_passed = false;
    }
    else
    {
        ESP_LOGI(TAG, "Battery voltage OK: %.2fV", battery_voltage);
    }

    /* Before the inverter has ever been started, there should be
     * essentially no AC output. */
    float output_voltage = sys_state.inverter.output_voltage;
    if (output_voltage > POST_MAX_OUTPUT_VOLTAGE_IDLE)
    {
        ESP_LOGE(TAG, "Output voltage not idle: %.2fV (expected < %.2fV)",
                 output_voltage, POST_MAX_OUTPUT_VOLTAGE_IDLE);
        all_passed = false;
    }
    else
    {
        ESP_LOGI(TAG, "Output voltage idle OK: %.2fV", output_voltage);
    }

    return all_passed;
}
