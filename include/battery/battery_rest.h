/**
 * @file battery_rest.h
 * @brief Battery rest detection.
 *
 * A battery's terminal voltage is only a good indicator of State of Charge
 * (SOC) when the battery has been at rest (minimal charge/discharge current)
 * for a period of time.
 *
 * This module determines when the battery is considered "resting".
 */

#ifndef BATTERY_REST_H
#define BATTERY_REST_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

    /*----------------------------------------------------------
     * Rest Detection State
     *---------------------------------------------------------*/
    typedef struct
    {
        float current_threshold;    /**< Maximum current (A) to be considered resting */
        uint32_t rest_time_seconds; /**< Required rest time */

        float accumulated_seconds; /**< Time spent below threshold */

        bool resting; /**< Current resting state */
    } battery_rest_t;

    /*----------------------------------------------------------
     * API
     *---------------------------------------------------------*/

    /**
     * @brief Initialise rest detector.
     *
     * @param detector Pointer to detector
     * @param current_threshold Current threshold in Amps
     * @param rest_time_seconds Required rest duration
     */
    void battery_rest_init(battery_rest_t *detector,
                           float current_threshold,
                           uint32_t rest_time_seconds);

    /**
     * @brief Reset detector.
     */
    void battery_rest_reset(battery_rest_t *detector);

    /**
     * @brief Update detector.
     *
     * Should be called periodically (e.g. once every second).
     *
     * @param detector Detector object
     * @param battery_current Current in Amps
     * @param dt_seconds Elapsed time in seconds
     *
     * @return true if battery is resting
     */
    bool battery_rest_update(battery_rest_t *detector,
                             float battery_current,
                             float dt_seconds);

    /**
     * @brief Returns current resting state.
     */
    bool battery_is_resting(const battery_rest_t *detector);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_REST_H */