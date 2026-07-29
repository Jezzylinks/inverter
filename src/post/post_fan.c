#include "post_fan.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "hardware_config.h"
#include "system_state.h"

extern system_state_t sys_state;

static const char *TAG = "POST_FAN";

#define POST_FAN_TEST_DURATION_MS 2000

bool post_fan_test(void)
{
    /* GPIO_FAN_TEST is already configured as an output by init_hardware(),
     * which always runs before this. */
    esp_err_t err = gpio_set_level(GPIO_FAN_TEST, 1);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to drive GPIO_FAN_TEST high: %s", esp_err_to_name(err));
        return false;
    }

    /* Let the fan physically spin up, and let adc_task (running
     * concurrently on its own task) take several fresh samples of
     * ADC_FAN during this wait -- sys_state.fan.speed only updates if
     * adc_task keeps running while we wait here. */
    vTaskDelay(pdMS_TO_TICKS(POST_FAN_TEST_DURATION_MS));

    float speed = sys_state.fan.speed;

    gpio_set_level(GPIO_FAN_TEST, 0);

    if (speed < FAN_SPEED_THRESHOLD)
    {
        ESP_LOGE(TAG, "Fan speed reading %.2fV below threshold %.2fV",
                 speed, (float)FAN_SPEED_THRESHOLD);
        return false;
    }

    ESP_LOGI(TAG, "Fan OK: %.2fV >= %.2fV", speed, (float)FAN_SPEED_THRESHOLD);
    return true;
}
