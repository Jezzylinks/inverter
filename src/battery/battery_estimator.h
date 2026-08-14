#ifndef BATTERY_ESTIMATOR_H
#define BATTERY_ESTIMATOR_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>

#include "battery_filter.h"
#include "battery_rest.h"
#include "battery_soc.h"
#include "coulomb_counter.h"
#include "battery_health.h"
#include "system_state.h"

    typedef struct
    {
        /* Configuration */
        battery_chemistry_t chemistry;
        float nominal_voltage;
        float rated_capacity_ah;

        /* Modules */
        battery_filter_t voltage_filter;
        battery_rest_t rest_detector;
        coulomb_counter_t counter;
        battery_health_t health;

        /* Latest measurements */
        float filtered_voltage;
        float compensated_voltage;

        /* Results */
        float soc;
        float soh;
        float remaining_ah;
        bool battery_resting;

    } battery_estimator_t;

    void battery_estimator_init(
        battery_estimator_t *est,
        battery_chemistry_t chemistry,
        float nominal_voltage,
        float battery_capacity_ah);

    /* Reconfigure a live estimator without discarding learned SOC/SOH. */
    void battery_estimator_reconfigure(
        battery_estimator_t *est,
        battery_chemistry_t chemistry,
        float nominal_voltage,
        float battery_capacity_ah);

    void battery_estimator_update(
        battery_estimator_t *est,
        float battery_voltage,
        float battery_current,
        float dt_seconds);

    float battery_estimator_get_soc(
        const battery_estimator_t *est);

    float battery_estimator_get_soh(
        const battery_estimator_t *est);

    float battery_estimator_get_remaining_ah(
        const battery_estimator_t *est);

    bool battery_estimator_is_resting(
        const battery_estimator_t *est);

#ifdef __cplusplus
}
#endif

#endif