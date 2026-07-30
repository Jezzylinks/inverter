/******************************************************************************
 * @file battery_soc.h
 * @brief Battery State of Charge (SOC) Lookup Tables
 *
 * Non-linear State of Charge (SOC) estimation using chemistry-specific
 * voltage lookup tables with linear interpolation.
 *
 * Supported Chemistries:
 *  - Lead Acid
 *  - AGM
 *  - GEL
 *  - Lithium-Ion
 *  - LiFePO4
 *  - NiMH
 *
 * The lookup tables are normalized to a 12 V battery. Higher voltage
 * systems (24 V, 48 V, 96 V, etc.) are automatically scaled to their
 * 12 V equivalent before the lookup is performed.
 ******************************************************************************/

#ifndef BATTERY_SOC_H
#define BATTERY_SOC_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>

#include "system_state.h"

  /******************************************************************************
   * Data Types
   ******************************************************************************/

  /**
   * @brief One point on a battery discharge curve.
   */
  typedef struct
  {
    float voltage_12v;
    uint8_t soc_pct;

  } soc_point_t;

  /**
   * @brief SOC lookup table.
   */
  typedef struct
  {
    const soc_point_t *points;

    uint8_t count;

    const char *name;

  } soc_table_t;

  /******************************************************************************
   * SOC Calculation
   ******************************************************************************/

  /**
   * @brief Calculate SOC from battery voltage.
   *
   * @param voltage_v Measured battery voltage.
   * @param nominal_v Battery nominal voltage (12/24/48/96 V).
   * @param table Pointer to chemistry lookup table.
   *
   * @return Battery State of Charge (0–100%).
   */
  uint8_t battery_soc_from_voltage(
      float voltage_v,
      float nominal_v,
      const soc_table_t *table);

  /**
   * @brief Get the lookup table for a battery chemistry.
   *
   * @param chemistry Battery chemistry.
   *
   * @return Pointer to SOC lookup table.
   */
  const soc_table_t *battery_soc_get_table(
      battery_chemistry_t chemistry);

  /**
   * @brief Calculate battery percentage.
   *
   * Convenience wrapper around
   * battery_soc_get_table() and
   * battery_soc_from_voltage().
   *
   * @param voltage Measured battery voltage.
   * @param chemistry Battery chemistry.
   * @param nominal_v Battery nominal voltage.
   *
   * @return Battery percentage (0–100%).
   */
  uint8_t calculate_battery_percentage(
      float voltage,
      battery_chemistry_t chemistry,
      float nominal_v);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_SOC_H */