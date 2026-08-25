#ifndef POST_FAN_H
#define POST_FAN_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*----------------------------------------------------------
 * Configuration
 *---------------------------------------------------------*/

/**
 * @brief Fan spin-up delay before RPM measurement.
 */
#ifndef POST_FAN_STARTUP_DELAY_MS
#define POST_FAN_STARTUP_DELAY_MS 300U
#endif

/**
 * @brief Maximum time allowed for POST.
 */
#ifndef POST_FAN_TIMEOUT_MS
#define POST_FAN_TIMEOUT_MS 1000U
#endif

/**
 * @brief Minimum acceptable RPM.
 */
#ifndef POST_FAN_MIN_RPM
#define POST_FAN_MIN_RPM 800U
#endif

/**
 * @brief PWM duty used during POST.
 */
#ifndef POST_FAN_TEST_SPEED_PERCENT
#define POST_FAN_TEST_SPEED_PERCENT 100U
#endif

    /*----------------------------------------------------------
     * POST Result
     *---------------------------------------------------------*/

    typedef enum
    {
        POST_FAN_RESULT_PASS = 0,

        POST_FAN_RESULT_INIT_FAILED,

        POST_FAN_RESULT_START_FAILED,

        POST_FAN_RESULT_NO_TACH,

        POST_FAN_RESULT_LOW_RPM,

        POST_FAN_RESULT_TIMEOUT,

        POST_FAN_RESULT_DRIVER_ERROR

    } post_fan_result_t;

    /*----------------------------------------------------------
     * Statistics
     *---------------------------------------------------------*/

    typedef struct
    {
        uint32_t rpm;

        uint32_t elapsed_ms;

        bool tach_detected;

        post_fan_result_t result;

    } post_fan_status_t;

    /*----------------------------------------------------------
     * API
     *---------------------------------------------------------*/

    /**
     * @brief Initialize the fan POST module.
     *
     * @return ESP_OK on success.
     */
    esp_err_t post_fan_init(void);

    /**
     * @brief Execute the fan Power-On Self-Test.
     *
     * Test sequence:
     * 1. Reset tachometer history.
     * 2. Configure fan speed.
     * 3. Start fan.
     * 4. Wait for spin-up.
     * 5. Verify tachometer pulses.
     * 6. Measure RPM.
     * 7. Stop fan.
     *
     * @return POST result.
     */
    post_fan_result_t post_fan_test(void);

    /**
     * @brief Stop the POST and turn the fan off.
     *
     * Safe to call even if POST is not running.
     *
     * @return ESP_OK on success.
     */
    esp_err_t post_fan_stop(void);

    /**
     * @brief Returns the latest measured RPM.
     *
     * @return Fan RPM.
     */
    uint32_t post_fan_get_rpm(void);

    /**
     * @brief Returns the latest POST result.
     *
     * @return POST result.
     */
    post_fan_result_t post_fan_get_result(void);

    /**
     * @brief Returns detailed POST status.
     *
     * @param status Pointer to status structure.
     */
    void post_fan_get_status(post_fan_status_t *status);

    /**
     * @brief Returns true if the last POST passed.
     *
     * @return true if PASS.
     */
    bool post_fan_passed(void);

    /**
     * @brief Convert POST result into a readable string.
     *
     * @param result POST result.
     *
     * @return Constant string.
     */
    const char *post_fan_result_string(post_fan_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* POST_FAN_H */