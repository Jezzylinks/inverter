#include "post_fan.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "fan/fan_driver.h"
#include "hardware_config.h" /* FAN_SPEED_THRESHOLD_RPM */

static const char *TAG = "POST_FAN";

/*----------------------------------------------------------
 * Configuration
 *---------------------------------------------------------*/
#define POST_FAN_TEST_DUTY_PERCENT 80
#define POST_FAN_STABILIZE_MS 300
#define POST_FAN_MEASURE_WINDOW_MS 500
#define POST_FAN_SAMPLE_INTERVAL_MS 50

/*----------------------------------------------------------
 * Public Functions
 *---------------------------------------------------------*/

bool post_fan_test(void)
{
    if (fan_set_speed_percent(POST_FAN_TEST_DUTY_PERCENT) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to command fan PWM");
        return false;
    }

    /* Let the fan physically spin up before trusting the tachometer --
     * fan_driver's ISR keeps timestamping pulses in the background
     * throughout this wait regardless of which task is running. */
    vTaskDelay(pdMS_TO_TICKS(POST_FAN_STABILIZE_MS));

    /* Take the peak RPM reading over the measurement window rather than
     * a single sample, since a single read could land right after a
     * bounce-rejected pulse and look artificially low even with a
     * genuinely spinning, healthy fan. */
    uint32_t peak_rpm = 0;
    uint32_t elapsed_ms = 0;

    while (elapsed_ms < POST_FAN_MEASURE_WINDOW_MS)
    {
        uint32_t rpm = fan_get_rpm();
        if (rpm > peak_rpm)
        {
            peak_rpm = rpm;
        }
        vTaskDelay(pdMS_TO_TICKS(POST_FAN_SAMPLE_INTERVAL_MS));
        elapsed_ms += POST_FAN_SAMPLE_INTERVAL_MS;
    }

    fan_set_speed_percent(0);

    if (peak_rpm < (uint32_t)FAN_SPEED_THRESHOLD_RPM)
    {
        ESP_LOGE(TAG, "Fan RPM %lu below threshold %lu",
                 (unsigned long)peak_rpm, (unsigned long)FAN_SPEED_THRESHOLD_RPM);
        return false;
    }

    ESP_LOGI(TAG, "Fan OK: %lu RPM >= %lu RPM",
             (unsigned long)peak_rpm, (unsigned long)FAN_SPEED_THRESHOLD_RPM);
    return true;
}
