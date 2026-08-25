/******************************************************************************
 * battery_storage.h
 ******************************************************************************/

#ifndef BATTERY_STORAGE_H
#define BATTERY_STORAGE_H

#include <stdint.h>
#include <stdbool.h>

#define BATTERY_STORAGE_VERSION (1U)

/*
 * Data stored in NVS.
 * Only values that should survive a reboot are stored.
 */
typedef struct
{
    uint32_t version;

    /* Battery State of Charge (%) */
    float soc;

    /* Battery State of Health (%) */
    float soh;

    /* Equivalent Full Cycles */
    uint32_t equivalent_full_cycles;

    /* Learnt battery capacity (Ah) */
    float measured_capacity_ah;

    /* User configured battery capacity (Ah) */
    float rated_capacity_ah;

    /* User selected battery chemistry */
    uint8_t chemistry;

} battery_storage_data_t;

bool battery_storage_save(
    const battery_storage_data_t *data);

bool battery_storage_load(
    battery_storage_data_t *data);

bool battery_storage_erase(void);

#endif