#include "post/fan_controller.h"

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"

/*----------------------------------------------------------
 * Private Definitions
 *---------------------------------------------------------*/

#define TAG "fan_controller"

/*----------------------------------------------------------
 * Driver Context
 *---------------------------------------------------------*/

typedef struct
{
    fan_controller_config_t config;

    fan_state_t state;

    fan_mode_t mode;

    uint8_t duty_percent;

    bool initialized;

} fan_controller_ctx_t;

static fan_controller_ctx_t s_ctx;

/*----------------------------------------------------------
 * Private Functions
 *---------------------------------------------------------*/

static uint32_t duty_percent_to_raw(uint8_t percent)
{
    if (percent > 100)
    {
        percent = 100;
    }

    return ((uint32_t)percent * FAN_PWM_MAX_DUTY) / 100U;
}

static esp_err_t fan_controller_timer_init(void)
{
    ledc_timer_config_t timer_cfg =
        {
            .speed_mode = s_ctx.config.speed_mode,
            .timer_num = s_ctx.config.timer,
            .duty_resolution = s_ctx.config.duty_resolution,
            .freq_hz = s_ctx.config.pwm_frequency,
            .clk_cfg = LEDC_AUTO_CLK,
        };

    return ledc_timer_config(&timer_cfg);
}

static esp_err_t fan_controller_channel_init(void)
{
    ledc_channel_config_t channel_cfg =
        {
            .gpio_num = s_ctx.config.fan_gpio,
            .speed_mode = s_ctx.config.speed_mode,
            .channel = s_ctx.config.channel,
            .timer_sel = s_ctx.config.timer,
            .intr_type = LEDC_INTR_DISABLE,
            .duty = 0,
            .hpoint = 0,
        };

    return ledc_channel_config(&channel_cfg);
}

/*----------------------------------------------------------
 * Public Functions
 *---------------------------------------------------------*/

esp_err_t fan_controller_init(
    const fan_controller_config_t *config)
{
    ESP_RETURN_ON_FALSE(
        config != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Invalid configuration");

    if (s_ctx.initialized)
    {
        return ESP_OK;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));

    memcpy(
        &s_ctx.config,
        config,
        sizeof(fan_controller_config_t));

    ESP_RETURN_ON_ERROR(
        fan_controller_timer_init(),
        TAG,
        "Failed to configure LEDC timer");

    ESP_RETURN_ON_ERROR(
        fan_controller_channel_init(),
        TAG,
        "Failed to configure LEDC channel");

    s_ctx.state = FAN_STATE_OFF;

    s_ctx.mode = FAN_MODE_MANUAL;

    s_ctx.duty_percent = 100;

    s_ctx.initialized = true;

    ESP_LOGI(TAG,
             "Fan controller initialized");

    return ESP_OK;
}

esp_err_t fan_controller_deinit(void)
{
    ESP_RETURN_ON_FALSE(
        s_ctx.initialized,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Driver not initialized");

    esp_err_t err = ledc_stop(
        s_ctx.config.speed_mode,
        s_ctx.config.channel,
        !s_ctx.config.active_high);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to stop fan LEDC output: %s", esp_err_to_name(err));
        return err;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));

    ESP_LOGI(TAG,
             "Fan controller deinitialized");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Public Functions
 *---------------------------------------------------------*/

esp_err_t fan_controller_set_speed(uint8_t duty_percent)
{
    uint32_t raw_duty;

    ESP_RETURN_ON_FALSE(
        s_ctx.initialized,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Driver not initialized");

    if (duty_percent > 100U)
    {
        duty_percent = 100U;
    }

    s_ctx.duty_percent = duty_percent;

    raw_duty = duty_percent_to_raw(duty_percent);

    ESP_RETURN_ON_ERROR(
        ledc_set_duty(
            s_ctx.config.speed_mode,
            s_ctx.config.channel,
            raw_duty),
        TAG,
        "Failed to set PWM duty");

    ESP_RETURN_ON_ERROR(
        ledc_update_duty(
            s_ctx.config.speed_mode,
            s_ctx.config.channel),
        TAG,
        "Failed to update PWM duty");

    return ESP_OK;
}

esp_err_t fan_controller_on(void)
{
    ESP_RETURN_ON_FALSE(
        s_ctx.initialized,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Driver not initialized");

    ESP_RETURN_ON_ERROR(
        fan_controller_set_speed(
            s_ctx.duty_percent),
        TAG,
        "Failed to start fan");

    s_ctx.state = FAN_STATE_ON;

    ESP_LOGD(TAG,
             "Fan ON (%u%%)",
             s_ctx.duty_percent);

    return ESP_OK;
}

esp_err_t fan_controller_off(void)
{
    ESP_RETURN_ON_FALSE(
        s_ctx.initialized,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Driver not initialized");

    ESP_RETURN_ON_ERROR(
        ledc_set_duty(
            s_ctx.config.speed_mode,
            s_ctx.config.channel,
            0),
        TAG,
        "Failed to clear duty");

    ESP_RETURN_ON_ERROR(
        ledc_update_duty(
            s_ctx.config.speed_mode,
            s_ctx.config.channel),
        TAG,
        "Failed to update duty");

    s_ctx.state = FAN_STATE_OFF;

    ESP_LOGD(TAG,
             "Fan OFF");

    return ESP_OK;
}

esp_err_t fan_controller_toggle(void)
{
    if (s_ctx.state == FAN_STATE_ON)
    {
        return fan_controller_off();
    }

    return fan_controller_on();
}

uint8_t fan_controller_get_speed(void)
{
    return s_ctx.duty_percent;
}

bool fan_controller_is_running(void)
{
    return (s_ctx.state == FAN_STATE_ON);
}

void fan_controller_set_auto(bool enable)
{
    s_ctx.mode =
        enable ? FAN_MODE_AUTO : FAN_MODE_MANUAL;
}

bool fan_controller_is_auto(void)
{
    return (s_ctx.mode == FAN_MODE_AUTO);
}

fan_state_t fan_controller_get_state(void)
{
    return s_ctx.state;
}

fan_mode_t fan_controller_get_mode(void)
{
    return s_ctx.mode;
}