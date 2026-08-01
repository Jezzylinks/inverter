
#include "battery_health.h"

#include <math.h>
#include <string.h>

/*==============================================================================
 * Internal Helpers
 *============================================================================*/

static float clampf(float value,
                    float minimum,
                    float maximum)
{
    if (value < minimum)
    {
        return minimum;
    }

    if (value > maximum)
    {
        return maximum;
    }

    return value;
}

/*==============================================================================
 * Initialization
 *============================================================================*/

void battery_health_init(
    battery_health_t *health,
    float rated_capacity)
{
    if (health == NULL)
    {
        return;
    }

    memset(health, 0, sizeof(*health));

    if (rated_capacity <= 0.0f)
    {
        rated_capacity = 1.0f;
    }

    health->rated_capacity_ah = rated_capacity;

    health->measured_capacity_ah = rated_capacity;

    health->remaining_capacity_ah = rated_capacity;

    health->soh_percent = 100.0f;

    health->equivalent_full_cycles = 0;

    health->accumulated_charge_ah = 0.0f;

    health->accumulated_discharge_ah = 0.0f;

    health->total_charge_ah = 0.0f;

    health->total_discharge_ah = 0.0f;

    health->end_of_life = false;
}

/*============================================================================*/

void battery_health_reset(
    battery_health_t *health)
{
    if (health == NULL)
    {
        return;
    }

    battery_health_init(
        health,
        health->rated_capacity_ah);
}

/*==============================================================================
 * Restore
 *============================================================================*/

void battery_health_restore(
    battery_health_t *health,
    float soh,
    float measured_capacity,
    uint32_t cycles)
{
    if (health == NULL)
    {
        return;
    }

    health->soh_percent =
        clampf(
            soh,
            BATTERY_MIN_SOH,
            BATTERY_MAX_SOH);

    health->measured_capacity_ah =
        clampf(
            measured_capacity,
            0.0f,
            health->rated_capacity_ah);

    health->remaining_capacity_ah =
        health->measured_capacity_ah;

    health->equivalent_full_cycles =
        cycles;

    health->end_of_life =
        (health->soh_percent <=
         BATTERY_EOL_THRESHOLD);
}

/*==============================================================================
 * Runtime Update
 *============================================================================*/

void battery_health_update(
    battery_health_t *health,
    float current,
    float dt_seconds)
{
    if ((health == NULL) || (dt_seconds <= 0.0f))
    {
        return;
    }

    float delta_ah =
        fabsf(current) * (dt_seconds / 3600.0f);

    if (current > 0.0f)
    {
        health->total_charge_ah += delta_ah;
    }
    else if (current < 0.0f)
    {
        health->total_discharge_ah += delta_ah;
    }

    /* Total battery throughput */
    health->throughput_ah += delta_ah;

    /* Equivalent Full Cycle */
    while (health->throughput_ah >=
           (2.0f * health->rated_capacity_ah))
    {
        health->equivalent_full_cycles++;

        health->throughput_ah -=
            (2.0f * health->rated_capacity_ah);
    }

    health->end_of_life =
        (health->soh_percent <= BATTERY_EOL_THRESHOLD);
}

/******************************************************************************
 * Capacity Learning
 ******************************************************************************/

void battery_health_set_capacity(
    battery_health_t *health,
    float measured_capacity)
{
    if (health == NULL)
    {
        return;
    }

    if (measured_capacity <= 0.0f)
    {
        return;
    }

    health->measured_capacity_ah =
        clampf(measured_capacity,
               0.0f,
               health->rated_capacity_ah);

    battery_health_recalculate(health);
}

/******************************************************************************
 * Estimated Lifetime
 ******************************************************************************/

void battery_health_set_cycle_life(
    battery_health_t *health,
    uint32_t cycle_life)
{
    if (health == NULL)
    {
        return;
    }

    if (cycle_life == 0U)
    {
        cycle_life = 1U;
    }

    health->estimated_cycle_life =
        cycle_life;
}

/******************************************************************************
 * SOH Recalculation
 ******************************************************************************/

void battery_health_recalculate(
    battery_health_t *health)
{
    if (health == NULL)
    {
        return;
    }

    /*
     * Capacity-based SOH
     */

    float capacity_soh =
        (health->measured_capacity_ah /
         health->rated_capacity_ah) *
        100.0f;

    /*
     * Cycle ageing contribution
     */

    float cycle_soh = 100.0f;

    if (health->estimated_cycle_life > 0U)
    {
        cycle_soh =
            100.0f *
            (1.0f -
             ((float)health->equivalent_full_cycles /
              (float)health->estimated_cycle_life));

        cycle_soh =
            clampf(cycle_soh,
                   0.0f,
                   100.0f);
    }

    /*
     * Commercial weighted model
     *
     * Capacity = 80%
     * Cycle ageing = 20%
     */

    health->soh_percent =
        (capacity_soh * 0.80f) +
        (cycle_soh * 0.20f);

    health->soh_percent =
        clampf(health->soh_percent,
               BATTERY_MIN_SOH,
               BATTERY_MAX_SOH);

    /*
     * Remaining useful life
     */

    if (health->estimated_cycle_life > 0U)
    {
        health->rul_percent =
            100.0f -
            (((float)health->equivalent_full_cycles /
              (float)health->estimated_cycle_life) *
             100.0f);

        health->rul_percent =
            clampf(health->rul_percent,
                   0.0f,
                   100.0f);
    }
    else
    {
        health->rul_percent = 100.0f;
    }

    /*
     * Remaining usable capacity
     */

    health->remaining_capacity_ah =
        health->rated_capacity_ah *
        (health->soh_percent / 100.0f);

    /*
     * Coulombic efficiency
     */

    if (health->total_charge_ah > 0.1f)
    {
        health->coulombic_efficiency =
            (health->total_discharge_ah /
             health->total_charge_ah) *
            100.0f;
    }
    else
    {
        health->coulombic_efficiency = 100.0f;
    }

    /*
     * Charge efficiency
     */

    if (health->total_charge_ah > 0.1f)
    {
        health->charge_efficiency =
            (health->measured_capacity_ah /
             health->rated_capacity_ah) *
            100.0f;
    }
    else
    {
        health->charge_efficiency = 100.0f;
    }

    health->end_of_life =
        (health->soh_percent <=
         BATTERY_EOL_THRESHOLD);
}

/*==============================================================================
 * Getters
 *============================================================================*/

float battery_health_get_soh(const battery_health_t *health)
{
    return (health != NULL) ? health->soh_percent : 0.0f;
}

float battery_health_get_capacity(const battery_health_t *health)
{
    return (health != NULL) ? health->measured_capacity_ah : 0.0f;
}

float battery_health_get_remaining_capacity(const battery_health_t *health)
{
    return (health != NULL) ? health->remaining_capacity_ah : 0.0f;
}

uint32_t battery_health_get_cycle_count(const battery_health_t *health)
{
    return (health != NULL) ? health->equivalent_full_cycles : 0U;
}

float battery_health_get_total_charge(const battery_health_t *health)
{
    return (health != NULL) ? health->total_charge_ah : 0.0f;
}

float battery_health_get_total_discharge(const battery_health_t *health)
{
    return (health != NULL) ? health->total_discharge_ah : 0.0f;
}

bool battery_health_is_end_of_life(const battery_health_t *health)
{
    return (health != NULL) ? health->end_of_life : false;
}

float battery_health_get_rul(const battery_health_t *health)
{
    return (health != NULL) ? health->rul_percent : 0.0f;
}

float battery_health_get_coulombic_efficiency(const battery_health_t *health)
{
    return (health != NULL) ? health->coulombic_efficiency : 0.0f;
}

float battery_health_get_charge_efficiency(const battery_health_t *health)
{
    return (health != NULL) ? health->charge_efficiency : 0.0f;
}