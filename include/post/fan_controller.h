#ifndef FAN_CONTROLLER_H
#define FAN_CONTROLLER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

/*----------------------------------------------------------
 * Configuration
 *---------------------------------------------------------*/

/* Default PWM Frequency */
#ifndef FAN_PWM_FREQUENCY_HZ
#define FAN_PWM_FREQUENCY_HZ 25000U
#endif

/* Default PWM Resolution */
#ifndef FAN_PWM_RESOLUTION
#define FAN_PWM_RESOLUTION LEDC_TIMER_10_BIT
#endif

/* Maximum Duty */
#ifndef FAN_PWM_MAX_DUTY
#define FAN_PWM_MAX_DUTY ((1 << 10) - 1)
#endif

    /*----------------------------------------------------------
     * Fan State
     *---------------------------------------------------------*/

    typedef enum
    {
        FAN_STATE_OFF = 0,

        FAN_STATE_ON

    } fan_state_t;

    /*----------------------------------------------------------
     * Fan Mode
     *---------------------------------------------------------*/

    typedef enum
    {
        FAN_MODE_MANUAL = 0,

        FAN_MODE_AUTO

    } fan_mode_t;

    /*----------------------------------------------------------
     * Fan Configuration
     *---------------------------------------------------------*/

    typedef struct
    {
        gpio_num_t fan_gpio;

        ledc_timer_t timer;

        ledc_mode_t speed_mode;

        ledc_channel_t channel;

        ledc_timer_bit_t duty_resolution;

        uint32_t pwm_frequency;

        bool active_high;

    } fan_controller_config_t;

    /*----------------------------------------------------------
     * API
     *---------------------------------------------------------*/

    /**
     * @brief Initialize the fan controller.
     *
     * @param config Pointer to configuration.
     *
     * @return ESP_OK on success.
     */
    esp_err_t fan_controller_init(
        const fan_controller_config_t *config);

    /**
     * @brief Deinitialize the fan controller.
     *
     * @return ESP_OK on success.
     */
    esp_err_t fan_controller_deinit(void);

    /**
     * @brief Turn the fan ON.
     *
     * Uses the previously configured speed.
     *
     * @return ESP_OK on success.
     */
    esp_err_t fan_controller_on(void);

    /**
     * @brief Turn the fan OFF.
     *
     * @return ESP_OK on success.
     */
    esp_err_t fan_controller_off(void);

    /**
     * @brief Toggle the fan state.
     *
     * @return ESP_OK on success.
     */
    esp_err_t fan_controller_toggle(void);

    /**
     * @brief Set fan speed.
     *
     * @param duty_percent
     * PWM duty (0-100%).
     *
     * @return ESP_OK on success.
     */
    esp_err_t fan_controller_set_speed(
        uint8_t duty_percent);

    /**
     * @brief Get current fan speed.
     *
     * @return Duty percentage.
     */
    uint8_t fan_controller_get_speed(void);

    /**
     * @brief Returns true if fan is ON.
     *
     * @return true if running.
     */
    bool fan_controller_is_running(void);

    /**
     * @brief Enable or disable automatic fan control.
     *
     * @param enable true = automatic.
     */
    void fan_controller_set_auto(
        bool enable);

    /**
     * @brief Check whether automatic mode is enabled.
     *
     * @return true if automatic mode.
     */
    bool fan_controller_is_auto(void);

    /**
     * @brief Get current fan state.
     *
     * @return Fan state.
     */
    fan_state_t fan_controller_get_state(void);

    /**
     * @brief Get current operating mode.
     *
     * @return Fan mode.
     */
    fan_mode_t fan_controller_get_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* FAN_CONTROLLER_H */