/**
 * @file battery_filter.h
 * @brief Battery voltage/current low-pass filter.
 *
 * Exponential Moving Average (EMA) filter used to smooth ADC readings
 * before SOC estimation.
 *
 * y[n] = y[n-1] + α(x[n] - y[n-1])
 *
 * Smaller α:
 *      More smoothing
 *      Slower response
 *
 * Larger α:
 *      Faster response
 *      Less smoothing
 */

#ifndef BATTERY_FILTER_H
#define BATTERY_FILTER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>

    /*----------------------------------------------------------
     * Filter Structure
     *---------------------------------------------------------*/
    typedef struct
    {
        float alpha;      /**< Filter coefficient (0.0 - 1.0) */
        float filtered;   /**< Current filtered value */
        bool initialized; /**< First sample received */
    } battery_filter_t;

    /*----------------------------------------------------------
     * API
     *---------------------------------------------------------*/

    /**
     * @brief Initialise filter.
     *
     * @param filter Pointer to filter object
     * @param alpha Filter coefficient (0.01 - 1.0)
     */
    void battery_filter_init(battery_filter_t *filter,
                             float alpha);

    /**
     * @brief Reset filter.
     *
     * Next sample becomes the initial filtered value.
     */
    void battery_filter_reset(battery_filter_t *filter);

    /**
     * @brief Update filter.
     *
     * @param filter Filter object
     * @param sample New ADC sample
     *
     * @return Filtered output
     */
    float battery_filter_update(battery_filter_t *filter,
                                float sample);

    /**
     * @brief Get current filtered value.
     */
    float battery_filter_get(const battery_filter_t *filter);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_FILTER_H */