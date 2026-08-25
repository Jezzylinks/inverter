#include "post/post_fan.h"
#include <string.h>
#include "post/fan_controller.h"
#include "post/fan_tach.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hardware/hardware_config.h"

/*----------------------------------------------------------
 * Private Definitions
 *---------------------------------------------------------*/

#define TAG "post_fan"

/*----------------------------------------------------------
 * Private Variables
 *---------------------------------------------------------*/

static post_fan_status_t s_status;

static const fan_tach_config_t s_tach_cfg =
    {
        .tach_gpio = GPIO_FAN_TACH,

        .interrupt_type = GPIO_INTR_NEGEDGE,

        .pullup_enable = true,

        .pulldown_enable = false,

        .timer_resolution_hz = FAN_TACH_TIMER_RESOLUTION_HZ,

        .pulses_per_revolution = FAN_TACH_PULSES_PER_REV,

        .timeout_us = FAN_TACH_TIMEOUT_US,
};

/*----------------------------------------------------------
 * Private Functions
 *---------------------------------------------------------*/

static void post_fan_reset_status(void)
{
    memset(&s_status, 0, sizeof(s_status));

    s_status.result = POST_FAN_RESULT_TIMEOUT;
}

static esp_err_t post_fan_prepare(void)
{
    ESP_RETURN_ON_ERROR(
        fan_tach_reset(),
        TAG,
        "Failed to reset tachometer");

    ESP_RETURN_ON_ERROR(
        fan_controller_set_speed(
            POST_FAN_TEST_SPEED_PERCENT),
        TAG,
        "Failed to set fan speed");

    ESP_RETURN_ON_ERROR(
        fan_controller_on(),
        TAG,
        "Failed to start fan");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Public Functions
 *---------------------------------------------------------*/

esp_err_t post_fan_init(void)
{
    post_fan_reset_status();

    ESP_RETURN_ON_ERROR(
        fan_tach_init(&s_tach_cfg),
        TAG,
        "Failed to initialize fan tach");

    ESP_RETURN_ON_ERROR(
        fan_tach_start(),
        TAG,
        "Failed to start fan tach");

    ESP_LOGI(TAG,
             "Fan POST initialized");

    return ESP_OK;
}

esp_err_t post_fan_stop(void)
{
    esp_err_t err;

    err = fan_controller_off();

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "Failed to stop fan");
    }

    err = fan_tach_stop();

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "Failed to stop tachometer");
    }

    return ESP_OK;
}

/*----------------------------------------------------------
 * Public Functions
 *---------------------------------------------------------*/

post_fan_result_t post_fan_test(void)
{
    TickType_t start_tick;
    TickType_t elapsed_tick;

    esp_err_t err;

    post_fan_reset_status();

    err = post_fan_prepare();

    if (err != ESP_OK)
    {
        s_status.result = POST_FAN_RESULT_DRIVER_ERROR;
        post_fan_stop();
        return s_status.result;
    }

    vTaskDelay(
        pdMS_TO_TICKS(
            POST_FAN_STARTUP_DELAY_MS));

    start_tick = xTaskGetTickCount();

    while (true)
    {
        elapsed_tick =
            xTaskGetTickCount() - start_tick;

        s_status.elapsed_ms =
            elapsed_tick * portTICK_PERIOD_MS;

        if (s_status.elapsed_ms >=
            POST_FAN_TIMEOUT_MS)
        {
            s_status.result =
                POST_FAN_RESULT_TIMEOUT;

            break;
        }

        s_status.tach_detected =
            fan_tach_is_alive();

        if (!s_status.tach_detected)
        {
            vTaskDelay(
                pdMS_TO_TICKS(20));

            continue;
        }

        if (!fan_tach_is_ready())
        {
            vTaskDelay(
                pdMS_TO_TICKS(20));

            continue;
        }

        s_status.rpm =
            fan_tach_get_rpm();

        if (s_status.rpm >=
            POST_FAN_MIN_RPM)
        {
            s_status.result =
                POST_FAN_RESULT_PASS;

            break;
        }

        vTaskDelay(
            pdMS_TO_TICKS(20));
    }

    if (s_status.result ==
        POST_FAN_RESULT_TIMEOUT)
    {
        if (!s_status.tach_detected)
        {
            s_status.result =
                POST_FAN_RESULT_NO_TACH;
        }
        else
        {
            s_status.result =
                POST_FAN_RESULT_LOW_RPM;
        }
    }

    post_fan_stop();

    ESP_LOGI(TAG,
             "POST Result=%d RPM=%lu Time=%lu ms",
             s_status.result,
             (unsigned long)s_status.rpm,
             (unsigned long)s_status.elapsed_ms);

    return s_status.result;
}

uint32_t post_fan_get_rpm(void)
{
    return s_status.rpm;
}

post_fan_result_t post_fan_get_result(void)
{
    return s_status.result;
}

void post_fan_get_status(
    post_fan_status_t *status)
{
    if (status == NULL)
    {
        return;
    }

    memcpy(status,
           &s_status,
           sizeof(post_fan_status_t));
}

bool post_fan_passed(void)
{
    return (s_status.result ==
            POST_FAN_RESULT_PASS);
}

const char *post_fan_result_string(
    post_fan_result_t result)
{
    switch (result)
    {
    case POST_FAN_RESULT_PASS:
        return "PASS";

    case POST_FAN_RESULT_INIT_FAILED:
        return "INIT FAILED";

    case POST_FAN_RESULT_START_FAILED:
        return "START FAILED";

    case POST_FAN_RESULT_NO_TACH:
        return "NO TACH";

    case POST_FAN_RESULT_LOW_RPM:
        return "LOW RPM";

    case POST_FAN_RESULT_TIMEOUT:
        return "TIMEOUT";

    case POST_FAN_RESULT_DRIVER_ERROR:
        return "DRIVER ERROR";

    default:
        return "UNKNOWN";
    }
}