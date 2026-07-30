#ifndef FAN_TACH_H
#define FAN_TACH_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_err.h"

/*----------------------------------------------------------
 * Default Configuration
 *---------------------------------------------------------*/

/* Timer Resolution (1 tick = 1 us) */
#ifndef FAN_TACH_TIMER_RESOLUTION_HZ
#define FAN_TACH_TIMER_RESOLUTION_HZ 1000000UL
#endif

/* Default Pulses Per Revolution */
#ifndef FAN_TACH_PULSES_PER_REV
#define FAN_TACH_PULSES_PER_REV 2U
#endif

/* Number of timestamps stored */
#ifndef FAN_TACH_HISTORY_SIZE
#define FAN_TACH_HISTORY_SIZE 8U
#endif

/* Stall timeout */
#ifndef FAN_TACH_TIMEOUT_US
#define FAN_TACH_TIMEOUT_US 500000UL
#endif

    /*----------------------------------------------------------
     * Driver State
     *---------------------------------------------------------*/

    typedef enum
    {
        FAN_TACH_STATE_UNINITIALIZED = 0,

        FAN_TACH_STATE_STOPPED,

        FAN_TACH_STATE_RUNNING

    } fan_tach_state_t;

    /*----------------------------------------------------------
     * Configuration
     *---------------------------------------------------------*/

    typedef struct
    {
        /* Tachometer GPIO */
        gpio_num_t tach_gpio;

        /* GPIO interrupt edge */
        gpio_int_type_t interrupt_type;

        /* Enable GPIO pull-up */
        bool pullup_enable;

        /* Enable GPIO pull-down */
        bool pulldown_enable;

        /* GPTimer resolution */
        uint32_t timer_resolution_hz;

        /* Pulses per revolution */
        uint32_t pulses_per_revolution;

        /* Stall timeout */
        uint32_t timeout_us;

    } fan_tach_config_t;

    /*----------------------------------------------------------
     * Driver API
     *---------------------------------------------------------*/

    /**
     * @brief Initialize the tachometer driver.
     *
     * Creates the GPTimer, configures the GPIO
     * and installs the ISR.
     *
     * @param config Driver configuration.
     *
     * @return ESP_OK on success.
     */
    esp_err_t fan_tach_init(
        const fan_tach_config_t *config);

    /**
     * @brief Deinitialize the driver.
     *
     * @return ESP_OK on success.
     */
    esp_err_t fan_tach_deinit(void);

    /**
     * @brief Start tachometer measurement.
     *
     * @return ESP_OK on success.
     */
    esp_err_t fan_tach_start(void);

    /**
     * @brief Stop tachometer measurement.
     *
     * @return ESP_OK on success.
     */
    esp_err_t fan_tach_stop(void);

    /**
     * @brief Reset timestamp history.
     */
    void fan_tach_reset(void);

    /**
     * @brief Returns true when enough samples
     * are available for RPM calculation.
     *
     * @return true if ready.
     */
    bool fan_tach_is_ready(void);

    /**
     * @brief Returns true if tachometer pulses
     * are currently being received.
     *
     * @return true if fan is alive.
     */
    bool fan_tach_is_alive(void);

    /**
     * @brief Returns the average pulse period.
     *
     * @return Period in microseconds.
     */
    uint64_t fan_tach_get_period_us(void);

    /**
     * @brief Returns the measured RPM.
     *
     * @return Fan speed in RPM.
     */
    uint32_t fan_tach_get_rpm(void);

    /**
     * @brief Returns the current driver state.
     *
     * @return Driver state.
     */
    fan_tach_state_t fan_tach_get_state(void);

    /**
     * @brief Returns the configured timer resolution.
     *
     * @return Timer resolution in Hz.
     */
    uint32_t fan_tach_get_timer_resolution(void);

    /**
     * @brief Returns the configured pulses
     * per revolution.
     *
     * @return PPR.
     */
    uint32_t fan_tach_get_ppr(void);

    /**
     * @brief Returns the number of captured
     * timestamps currently stored.
     *
     * @return Number of samples.
     */
    uint8_t fan_tach_get_sample_count(void);

#ifdef __cplusplus
}
#endif

#endif /* FAN_TACH_H */