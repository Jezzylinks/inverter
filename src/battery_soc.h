#pragma once
/*==============================================================================
  battery_soc.h
  Non-linear State of Charge (SOC) calculation using per-chemistry
  voltage lookup tables with linear interpolation between points.

  Why this matters:
  - Lead-acid, LiFePO4, NiMH all have very different discharge curves.
  - A linear voltage→SOC map is only accurate for lead-acid at C/20.
  - LiFePO4 has a flat plateau from 20%–80% where linear mapping
    shows almost no change, then suddenly drops — badly misleading.
  - Each battery_profile_t now carries a pointer to the correct table.
==============================================================================*/
#include <stdint.h>
#include <stdbool.h>

/* ── One point on the discharge curve ───────────────────────────────── */
typedef struct
{
    float voltage_12v; /* voltage at this SOC for a 12 V system       */
    uint8_t soc_pct;   /* state of charge 0–100 %                     */
} soc_point_t;

/* ── Full lookup table for one chemistry ─────────────────────────────── */
typedef struct
{
    const soc_point_t *points;
    uint8_t count;
    const char *name;
} soc_table_t;

/*
 * Calculate SOC percentage for a given voltage.
 *
 * @param voltage_v     Measured battery voltage in volts (any system voltage).
 * @param nominal_v     System nominal voltage (12, 24, 48, 96).
 * @param table         Pointer to the chemistry-specific lookup table.
 * @return              SOC 0–100 %, linearly interpolated between table points.
 */
uint8_t battery_soc_from_voltage(float voltage_v,
                                 float nominal_v,
                                 const soc_table_t *table);

/*
 * Returns the lookup table for a given battery chemistry index.
 * chemistry values match battery_chemistry_t in main.c.
 */
const soc_table_t *battery_soc_get_table(uint8_t chemistry);

/*
 * Convenience wrapper around battery_soc_get_table() + battery_soc_from_voltage().
 *
 * @param voltage    Measured battery voltage in volts (any system voltage).
 * @param chemistry  Battery chemistry index — pass sys_state.battery_profile.chemistry
 *                   (values match battery_chemistry_t in system_state.h, which is
 *                   kept in the same order as the lookup tables in battery_soc.c).
 * @param nominal_v  System nominal voltage — pass sys_state.battery_profile.nominal_voltage
 *                   (12, 24, 48, or 96).
 */
uint8_t calculate_battery_percentage(float voltage, uint8_t chemistry, float nominal_v);