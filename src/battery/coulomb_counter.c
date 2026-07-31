/******************************************************************************
 * @file coulomb_counter.c
 * @brief Coulomb Counter Implementation
 ******************************************************************************/

#include "coulomb_counter.h"

#include <math.h>

/******************************************************************************
 * Internal Helpers
 ******************************************************************************/

static float clampf(float value, float min, float max)
{
    if (value < min)
    {
        return min;
    }

    if (value > max)
    {
        return max;
    }

    return value;
}

/******************************************************************************
 * Initialization
 ******************************************************************************/

void coulomb_counter_init(
    coulomb_counter_t *cc,
    float rated_capacity)
{
    if (cc == NULL)
    {
        return;
    }

    if (rated_capacity <= 0.0f)
    {
        rated_capacity = 1.0f;
    }

    cc->rated_capacity_ah = rated_capacity;

    cc->remaining_capacity_ah = rated_capacity;

    cc->used_capacity_ah = 0.0f;

    cc->net_ah = 0.0f;

    cc->total_charge_ah = 0.0f;

    cc->total_discharge_ah = 0.0f;

    cc->soc_percent = 100.0f;

    cc->battery_full = true;

    cc->battery_empty = false;
}

/******************************************************************************
 * Reset
 ******************************************************************************/

void coulomb_counter_reset(
    coulomb_counter_t *cc)
{
    if (cc == NULL)
    {
        return;
    }

    coulomb_counter_init(
        cc,
        cc->rated_capacity_ah);
}

/******************************************************************************
 * Runtime Update
 ******************************************************************************/

void coulomb_counter_update(
    coulomb_counter_t *cc,
    float current,
    float dt_seconds)
{
    if ((cc == NULL) || (dt_seconds <= 0.0f))
    {
        return;
    }

    /* Convert current to Ah */
    float delta_ah = current * (dt_seconds / 3600.0f);

    cc->net_ah += delta_ah;

    if (current > 0.0f)
    {
        /* Charging */

        cc->remaining_capacity_ah += delta_ah;

        cc->total_charge_ah += delta_ah;
    }
    else if (current < 0.0f)
    {
        /* Discharging */

        cc->remaining_capacity_ah += delta_ah;

        cc->total_discharge_ah += fabsf(delta_ah);
    }

    cc->remaining_capacity_ah =
        clampf(
            cc->remaining_capacity_ah,
            0.0f,
            cc->rated_capacity_ah);

    cc->used_capacity_ah =
        cc->rated_capacity_ah -
        cc->remaining_capacity_ah;

    cc->soc_percent =
        (cc->remaining_capacity_ah /
         cc->rated_capacity_ah) *
        100.0f;

    cc->soc_percent =
        clampf(
            cc->soc_percent,
            COULOMB_COUNTER_MIN_SOC,
            COULOMB_COUNTER_MAX_SOC);

    cc->battery_full =
        (cc->soc_percent >= 99.5f);

    cc->battery_empty =
        (cc->soc_percent <= 0.5f);
}

/******************************************************************************
 * Synchronisation
 ******************************************************************************/

void coulomb_counter_set_soc(
    coulomb_counter_t *cc,
    float soc)
{
    if (cc == NULL)
    {
        return;
    }

    soc = clampf(
        soc,
        COULOMB_COUNTER_MIN_SOC,
        COULOMB_COUNTER_MAX_SOC);

    cc->soc_percent = soc;

    cc->remaining_capacity_ah =
        (soc / 100.0f) *
        cc->rated_capacity_ah;

    cc->used_capacity_ah =
        cc->rated_capacity_ah -
        cc->remaining_capacity_ah;

    cc->battery_full =
        (soc >= 99.5f);

    cc->battery_empty =
        (soc <= 0.5f);
}

/******************************************************************************/

void coulomb_counter_set_capacity(
    coulomb_counter_t *cc,
    float capacity_ah)
{
    if (cc == NULL)
    {
        return;
    }

    capacity_ah = clampf(
        capacity_ah,
        0.0f,
        cc->rated_capacity_ah);

    cc->remaining_capacity_ah =
        capacity_ah;

    cc->used_capacity_ah =
        cc->rated_capacity_ah -
        capacity_ah;

    cc->soc_percent =
        (capacity_ah /
         cc->rated_capacity_ah) *
        100.0f;

    cc->soc_percent = clampf(
        cc->soc_percent,
        COULOMB_COUNTER_MIN_SOC,
        COULOMB_COUNTER_MAX_SOC);

    cc->battery_full =
        (cc->soc_percent >= 99.5f);

    cc->battery_empty =
        (cc->soc_percent <= 0.5f);
}

/******************************************************************************
 * Getters
 ******************************************************************************/

float coulomb_counter_get_soc(
    const coulomb_counter_t *cc)
{
    return (cc != NULL) ? cc->soc_percent : 0.0f;
}

/******************************************************************************/

float coulomb_counter_get_remaining_ah(
    const coulomb_counter_t *cc)
{
    return (cc != NULL) ? cc->remaining_capacity_ah : 0.0f;
}

/******************************************************************************/

float coulomb_counter_get_used_ah(
    const coulomb_counter_t *cc)
{
    return (cc != NULL) ? cc->used_capacity_ah : 0.0f;
}

/******************************************************************************/

float coulomb_counter_get_net_ah(
    const coulomb_counter_t *cc)
{
    return (cc != NULL) ? cc->net_ah : 0.0f;
}

/******************************************************************************/

float coulomb_counter_get_total_charge(
    const coulomb_counter_t *cc)
{
    return (cc != NULL) ? cc->total_charge_ah : 0.0f;
}

/******************************************************************************/

float coulomb_counter_get_total_discharge(
    const coulomb_counter_t *cc)
{
    return (cc != NULL) ? cc->total_discharge_ah : 0.0f;
}

/******************************************************************************/

bool coulomb_counter_is_full(
    const coulomb_counter_t *cc)
{
    return (cc != NULL) ? cc->battery_full : false;
}

/******************************************************************************/

bool coulomb_counter_is_empty(
    const coulomb_counter_t *cc)
{
    return (cc != NULL) ? cc->battery_empty : false;
}

void coulomb_counter_sync(
    coulomb_counter_t *cc,
    float soc)
{
    coulomb_counter_set_soc(cc, soc);
}