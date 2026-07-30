/**
 * @file battery_health.h
 * @brief Battery State of Health (SOH) estimation.
 *
 * This module estimates battery ageing by tracking:
 *
 *  • Equivalent Full Cycles (EFC)
 *  • Remaining usable capacity
 *  • State of Health (SOH)
 *  • End-of-Life (EOL) status
 *
 * The module is chemistry-independent and is intended to work
 * alongside:
 *
 *      battery_soc.c
 *      battery_estimator.c
 *      coulomb_counter.c
 *      battery_storage.c
 *
 * SOH estimation combines:
 *
 *      1. Learned capacity
 *      2. Cycle count
 *      3. Capacity degradation
 *
 * Copyright (c)
 */

#ifndef BATTERY_HEALTH_H
#define BATTERY_HEALTH_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

    /*==============================================================================
     * Configuration
     *============================================================================*/

#ifndef BATTERY_EOL_THRESHOLD
#define BATTERY_EOL_THRESHOLD (80.0f)
#endif

#ifndef BATTERY_MAX_SOH
#define BATTERY_MAX_SOH (100.0f)
#endif

#ifndef BATTERY_MIN_SOH
#define BATTERY_MIN_SOH (0.0f)
#endif

    /*==============================================================================
     * Battery Health State
     *============================================================================*/

    typedef struct
    {
        /*------------------------------------------------------
         * Configuration
         *-----------------------------------------------------*/

        float rated_capacity_ah;

        /*------------------------------------------------------
         * Learned values
         *-----------------------------------------------------*/

        float measured_capacity_ah;

        float remaining_capacity_ah;

        float soh_percent;

        /*------------------------------------------------------
         * Equivalent Full Cycle Tracking
         *-----------------------------------------------------*/

        float accumulated_discharge_ah;

        float accumulated_charge_ah;

        uint32_t equivalent_full_cycles;

        /*------------------------------------------------------
         * Lifetime statistics
         *-----------------------------------------------------*/

        float total_charge_ah;

        float total_discharge_ah;

        /*------------------------------------------------------
         * Flags
         *-----------------------------------------------------*/

        bool end_of_life;

        uint32_t estimated_cycle_life;

        float rul_percent;

        float coulombic_efficiency;

        float charge_efficiency;

        float throughput_ah;

    } battery_health_t;

    /*==============================================================================
     * Initialisation
     *============================================================================*/

    /**
     * @brief Initialise battery health module.
     *
     * @param health Battery health object.
     * @param rated_capacity Rated battery capacity.
     */
    void battery_health_init(
        battery_health_t *health,
        float rated_capacity);

    /**
     * @brief Reset health statistics.
     */
    void battery_health_reset(
        battery_health_t *health);

    /**
     * @brief Restore data from persistent storage.
     *
     * @param health Health object.
     * @param soh Previously saved SOH.
     * @param measured_capacity Previously learned capacity.
     * @param cycles Previously saved cycle count.
     */
    void battery_health_restore(
        battery_health_t *health,
        float soh,
        float measured_capacity,
        uint32_t cycles);

    /*==============================================================================
     * Runtime Update
     *============================================================================*/

    /**
     * @brief Update battery health.
     *
     * Positive current = charging.
     *
     * Negative current = discharging.
     *
     * @param health Health object.
     * @param current Battery current.
     * @param dt_seconds Update period.
     */
    void battery_health_update(
        battery_health_t *health,
        float current,
        float dt_seconds);

    /**
     * @brief Update learned battery capacity.
     *
     * Usually called after a verified full discharge.
     *
     * @param health Health object.
     * @param measured_capacity Measured battery capacity.
     */
    void battery_health_set_capacity(
        battery_health_t *health,
        float measured_capacity);

    /*==============================================================================
     * Getters
     *============================================================================*/

    /**
     * @brief Returns State of Health.
     */
    float battery_health_get_soh(
        const battery_health_t *health);

    /**
     * @brief Returns learned capacity.
     */
    float battery_health_get_capacity(
        const battery_health_t *health);

    /**
     * @brief Returns remaining usable capacity.
     */
    float battery_health_get_remaining_capacity(
        const battery_health_t *health);

    /**
     * @brief Returns equivalent full cycles.
     */
    uint32_t battery_health_get_cycle_count(
        const battery_health_t *health);

    /**
     * @brief Returns total charged Ah.
     */
    float battery_health_get_total_charge(
        const battery_health_t *health);

    /**
     * @brief Returns total discharged Ah.
     */
    float battery_health_get_total_discharge(
        const battery_health_t *health);

    /**
     * @brief Returns true if battery has reached End-of-Life.
     *
     * Default threshold = 80% SOH.
     */
    bool battery_health_is_end_of_life(
        const battery_health_t *health);

    /*==============================================================================
     * Utility
     *============================================================================*/

    /**
     * @brief Force SOH recalculation.
     */
    void battery_health_recalculate(
        battery_health_t *health);

    /**
     * @brief Set the estimated battery lifetime in equivalent full cycles.
     *
     * Example:
     *  Lead Acid : 500 cycles
     *  AGM        : 800 cycles
     *  GEL        : 1000 cycles
     *  LiFePO4    : 6000 cycles
     */
    void battery_health_set_cycle_life(
        battery_health_t *health,
        uint32_t cycle_life);

    /**
     * @brief Remaining Useful Life (%).
     */
    float battery_health_get_rul(
        const battery_health_t *health);

    /**
     * @brief Coulombic efficiency.
     */
    float battery_health_get_coulombic_efficiency(
        const battery_health_t *health);

    /**
     * @brief Charge efficiency.
     */
    float battery_health_get_charge_efficiency(
        const battery_health_t *health);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_HEALTH_H */