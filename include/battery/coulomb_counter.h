/******************************************************************************
 * @file coulomb_counter.h
 * @brief Coulomb Counter for Battery SOC Estimation
 *
 * Integrates battery current over time to estimate:
 *
 *  • Remaining Capacity (Ah)
 *  • Used Capacity (Ah)
 *  • Net Amp-hours
 *  • State of Charge (SOC)
 *
 * Positive current  -> Charging
 * Negative current  -> Discharging
 *
 * This module is intended to be used by battery_estimator.c.
 ******************************************************************************/

#ifndef COULOMB_COUNTER_H
#define COULOMB_COUNTER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

    /******************************************************************************
     * Configuration
     ******************************************************************************/

#ifndef COULOMB_COUNTER_MIN_SOC
#define COULOMB_COUNTER_MIN_SOC (0.0f)
#endif

#ifndef COULOMB_COUNTER_MAX_SOC
#define COULOMB_COUNTER_MAX_SOC (100.0f)
#endif

    /******************************************************************************
     * Coulomb Counter State
     ******************************************************************************/

    typedef struct
    {
        /*----------------------------------------------------------
         * Configuration
         *---------------------------------------------------------*/

        /* Rated battery capacity (Ah) */
        float rated_capacity_ah;

        /*----------------------------------------------------------
         * Runtime Capacity
         *---------------------------------------------------------*/

        /* Remaining usable capacity (Ah) */
        float remaining_capacity_ah;

        /* Consumed capacity (Ah) */
        float used_capacity_ah;

        /* Net integrated capacity (Ah) */
        float net_ah;

        /*----------------------------------------------------------
         * Statistics
         *---------------------------------------------------------*/

        float total_charge_ah;

        float total_discharge_ah;

        /*----------------------------------------------------------
         * SOC
         *---------------------------------------------------------*/

        float soc_percent;

        /*----------------------------------------------------------
         * Status
         *---------------------------------------------------------*/

        bool battery_full;

        bool battery_empty;

    } coulomb_counter_t;

    /******************************************************************************
     * Initialization
     ******************************************************************************/

    /**
     * @brief Initialise coulomb counter.
     *
     * @param cc Pointer to counter object.
     * @param rated_capacity Battery capacity (Ah).
     */
    void coulomb_counter_init(
        coulomb_counter_t *cc,
        float rated_capacity);

    /**
     * @brief Reset runtime counters.
     */
    void coulomb_counter_reset(
        coulomb_counter_t *cc);

    /******************************************************************************
     * Runtime Update
     ******************************************************************************/

    /**
     * @brief Update coulomb counter.
     *
     * @param cc Counter object.
     * @param current Battery current.
     *        Positive = charging.
     *        Negative = discharging.
     * @param dt_seconds Sampling period.
     */
    void coulomb_counter_update(
        coulomb_counter_t *cc,
        float current,
        float dt_seconds);

    /******************************************************************************
     * Synchronisation
     ******************************************************************************/

    /**
     * @brief Force SOC.
     *
     * Used after:
     *  • Full charge
     *  • Rest calibration
     *  • NVS restore
     */
    void coulomb_counter_set_soc(
        coulomb_counter_t *cc,
        float soc);

    /**
     * @brief Set remaining battery capacity.
     *
     * Used after:
     *  • Capacity learning
     *  • SOH update
     */
    void coulomb_counter_set_capacity(
        coulomb_counter_t *cc,
        float capacity_ah);

    /******************************************************************************
     * Getters
     ******************************************************************************/

    /**
     * @brief Get SOC (%).
     */
    float coulomb_counter_get_soc(
        const coulomb_counter_t *cc);

    /**
     * @brief Get remaining capacity (Ah).
     */
    float coulomb_counter_get_remaining_ah(
        const coulomb_counter_t *cc);

    /**
     * @brief Get used capacity (Ah).
     */
    float coulomb_counter_get_used_ah(
        const coulomb_counter_t *cc);

    /**
     * @brief Get net amp-hours.
     */
    float coulomb_counter_get_net_ah(
        const coulomb_counter_t *cc);

    /**
     * @brief Total charged capacity.
     */
    float coulomb_counter_get_total_charge(
        const coulomb_counter_t *cc);

    /**
     * @brief Total discharged capacity.
     */
    float coulomb_counter_get_total_discharge(
        const coulomb_counter_t *cc);

    /**
     * @brief Returns true if battery is considered full.
     */
    bool coulomb_counter_is_full(
        const coulomb_counter_t *cc);

    /**
     * @brief Returns true if battery is considered empty.
     */
    bool coulomb_counter_is_empty(
        const coulomb_counter_t *cc);

    void coulomb_counter_sync(
        coulomb_counter_t *cc,
        float soc);

#ifdef __cplusplus
}
#endif

#endif /* COULOMB_COUNTER_H */