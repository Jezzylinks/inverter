#ifndef FAN_DRIVER_H
#define FAN_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Configure the fan PWM output (GPIO_FAN, LEDC) and the
     * tachometer input (GPIO_FAN_TACH, GPIO interrupt). Call once from
     * init_hardware().
     */
    void fan_driver_init(void);

    /**
     * @brief Set fan speed as a PWM duty cycle.
     * @param percent 0-100. 0 stops the fan; values are clamped to 0-100.
     * @return ESP_OK on success.
     */
    esp_err_t fan_set_speed_percent(uint8_t percent);

    /**
     * @brief Get the fan's last measured speed in RPM.
     *
     * Computed from the time between consecutive tachometer pulses
     * (pulse-period measurement, not pulse counting over a fixed
     * window) -- more accurate at low RPM and responds to speed changes
     * faster than a count-over-1-second approach, at the cost of a
     * little more bookkeeping in the ISR. See fan_driver.c for details.
     *
     * @return Current RPM, or 0 if no tach pulse has been seen recently
     *         (fan stopped, stalled, or not spinning fast enough to
     *         register -- see FAN_TACH_STALE_US in fan_driver.c).
     */
    uint32_t fan_get_rpm(void);

#ifdef __cplusplus
}
#endif

#endif /* FAN_DRIVER_H */
