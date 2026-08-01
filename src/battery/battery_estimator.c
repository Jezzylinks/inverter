#include "battery_estimator.h"
#include "coulomb_counter.h"
#include "stdlib.h"
#include "system_state.h"

static float save_timer_seconds = 0.0f;
extern battery_system_t battery;
extern system_state_t sys_state;

void battery_estimator_init(
    battery_estimator_t *est,
    battery_chemistry_t chemistry,
    float nominal_voltage,
    float battery_capacity_ah)
{
    if (est == NULL)
        return;

    est->chemistry = chemistry;
    est->nominal_voltage = nominal_voltage;
    est->rated_capacity_ah = battery_capacity_ah;

    battery_filter_init(
        &est->voltage_filter,
        0.05f);

    battery_rest_init(
        &est->rest_detector,
        0.5f,
        300);

    coulomb_counter_init(
        &est->counter,
        battery_capacity_ah);

    battery_health_init(
        &est->health,
        battery_capacity_ah);

    est->soc = 100.0f;
    est->soh = 100.0f;
    est->remaining_ah = battery_capacity_ah;
}

void battery_estimator_update(
    battery_estimator_t *est,
    float battery_voltage,
    float battery_current,
    float dt_seconds)
{
    if (est == NULL)
        return;

    /* Filter voltage */
    est->filtered_voltage =
        battery_filter_update(
            &est->voltage_filter,
            battery_voltage);

    /* No IR/temperature compensation model exists in this firmware yet
     * (no current or temperature sensor to compensate with) -- alias to
     * the filtered voltage rather than leaving this unset (it was never
     * assigned at all before, so the resting-recalibration branch below
     * was reading garbage/zero every time). */
    est->compensated_voltage = est->filtered_voltage;

    /* Coulomb counter */
    coulomb_counter_update(
        &est->counter,
        battery_current,
        dt_seconds);

    /* SOH update */
    battery_health_update(
        &est->health,
        battery_current,
        dt_seconds);

    /* Rest detector */
    est->battery_resting =
        battery_rest_update(
            &est->rest_detector,
            battery_current,
            (uint32_t)dt_seconds);

    /*
     * If battery is resting,
     * recalibrate coulomb counter using voltage lookup.
     */
    if (est->battery_resting)
    {
        uint8_t soc =
            calculate_battery_percentage(
                est->compensated_voltage,
                est->chemistry,
                est->nominal_voltage);

        coulomb_counter_set_soc(
            &est->counter,
            soc);
    }

    est->soc =
        coulomb_counter_get_soc(
            &est->counter);

    est->remaining_ah =
        coulomb_counter_get_remaining_ah(
            &est->counter);

    est->soh =
        battery_health_get_soh(
            &est->health);

    battery.voltage = est->filtered_voltage;
    battery.current = battery_current;
    battery.soc = est->soc;
    battery.soh = est->soh;

    save_timer_seconds += dt_seconds;

    if (save_timer_seconds >= 60.0f)
    {
        save_timer_seconds = 0.0f;

        battery.storage.version = BATTERY_STORAGE_VERSION;
        battery.storage.soc = est->soc;
        battery.storage.soh = est->soh;
        battery.storage.equivalent_full_cycles =
            battery_health_get_cycle_count(&est->health);
        battery.storage.measured_capacity_ah =
            battery_health_get_capacity(&est->health);
        battery.storage.rated_capacity_ah = est->rated_capacity_ah;
        battery.storage.chemistry = est->chemistry;

        battery_storage_save(&battery.storage);
    }
}

float battery_estimator_get_soc(
    const battery_estimator_t *est)
{
    return est ? est->soc : 0.0f;
}

float battery_estimator_get_soh(
    const battery_estimator_t *est)
{
    return est ? est->soh : 0.0f;
}

float battery_estimator_get_remaining_ah(
    const battery_estimator_t *est)
{
    return est ? est->remaining_ah : 0.0f;
}

bool battery_estimator_is_resting(
    const battery_estimator_t *est)
{
    return est ? est->battery_resting : false;
}