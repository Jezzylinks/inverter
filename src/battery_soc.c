/*==============================================================================
  battery_soc.c
  Non-linear SOC lookup tables and interpolation engine.

  All tables are for a 12 V reference system.
  battery_soc_from_voltage() scales the measured voltage down to 12 V
  equivalent before looking it up, so the same tables work for 24/48/96 V.

  Table points are ordered from HIGHEST voltage (100 %) to
  LOWEST voltage (0 %) — i.e. descending voltage order.
  This matches discharge direction and makes the binary search trivial.

  Sources:
  - Lead-Acid / AGM / GEL: Battery University BU-903
  - LiFePO4: Victron/CATL datasheet discharge curves
  - Li-Ion (3S 11.1 V nominal → scaled to 12 V equivalent): common 18650 data
  - NiMH (10-cell 12 V pack): Energizer / Panasonic application notes
==============================================================================*/
#include "battery_soc.h"
#include <stddef.h>

/* ── Lead-Acid (flooded, 12 V, C/20 discharge) ───────────────────────── */
static const soc_point_t s_lead_acid_pts[] = {
    {12.70f, 100},
    {12.50f, 90},
    {12.42f, 80},
    {12.32f, 70},
    {12.20f, 60},
    {12.06f, 50},
    {11.90f, 40},
    {11.75f, 30},
    {11.58f, 20},
    {11.31f, 10},
    {10.50f, 0},
};

/* ── AGM (VRLA, 12 V, C/20) ──────────────────────────────────────────── */
static const soc_point_t s_agm_pts[] = {
    {12.80f, 100},
    {12.60f, 90},
    {12.50f, 80},
    {12.35f, 70},
    {12.24f, 60},
    {12.10f, 50},
    {11.96f, 40},
    {11.81f, 30},
    {11.66f, 20},
    {11.51f, 10},
    {10.80f, 0},
};

/* ── GEL (12 V, C/20) ────────────────────────────────────────────────── */
static const soc_point_t s_gel_pts[] = {
    {12.85f, 100},
    {12.65f, 90},
    {12.55f, 80},
    {12.40f, 70},
    {12.30f, 60},
    {12.15f, 50},
    {12.00f, 40},
    {11.88f, 30},
    {11.75f, 20},
    {11.58f, 10},
    {11.00f, 0},
};

/* ── LiFePO4 (12.8 V nominal, 4-cell, C/5) ───────────────────────────── */
/* Note the characteristic flat plateau 30%–80% — this is where linear    */
/* mapping fails most dramatically.                                         */
static const soc_point_t s_lifepo4_pts[] = {
    {14.40f, 100}, /* top of charge                                    */
    {13.60f, 99},  /* after surface charge drops off                   */
    {13.45f, 95},
    {13.35f, 90},
    {13.30f, 80},
    {13.28f, 70}, /* flat plateau begins                               */
    {13.25f, 60},
    {13.22f, 50},
    {13.20f, 40},
    {13.15f, 30}, /* flat plateau ends                                 */
    {13.00f, 20},
    {12.50f, 10},
    {12.00f, 5},
    {11.00f, 0},
};

/* ── Li-Ion (3S 11.1 V nominal, scaled to 12 V equivalent) ──────────── */
/* Actual cell voltages: full=4.2 V, empty=3.0 V → 3S = 12.6 V / 9.0 V  */
static const soc_point_t s_liion_pts[] = {
    {12.60f, 100},
    {12.45f, 95},
    {12.30f, 90},
    {12.15f, 80},
    {12.00f, 70},
    {11.88f, 60},
    {11.76f, 50},
    {11.61f, 40},
    {11.46f, 30},
    {11.22f, 20},
    {10.95f, 10},
    {9.00f, 0},
};

/* ── NiMH (10-cell, 1.2 V/cell, ~12 V nominal) ──────────────────────── */
/* NiMH has a very flat discharge; SOC from voltage alone is unreliable   */
/* below 50% — use current integration if available.                       */
static const soc_point_t s_nimh_pts[] = {
    {14.40f, 100},
    {13.80f, 90},
    {13.20f, 80},
    {12.80f, 70},
    {12.50f, 60},
    {12.30f, 50},
    {12.10f, 40},
    {11.90f, 30},
    {11.60f, 20},
    {11.20f, 10},
    {10.00f, 0},
};

/* ── Table registry ──────────────────────────────────────────────────── */
/* Indexed by battery_chemistry_t from main.c:
   0=LEAD_ACID, 1=AGM, 2=GEL, 3=LITHIUM_ION, 4=LIFEPO4, 5=NIMH           */

#define MAKE_TABLE(arr, nm) \
    {(arr), (uint8_t)(sizeof(arr) / sizeof((arr)[0])), (nm)}

static const soc_table_t s_tables[] = {
    MAKE_TABLE(s_lead_acid_pts, "Lead-Acid"), /* 0 */
    MAKE_TABLE(s_agm_pts, "AGM"),             /* 1 */
    MAKE_TABLE(s_gel_pts, "GEL"),             /* 2 */
    MAKE_TABLE(s_liion_pts, "Li-Ion"),        /* 3 — LITHIUM_ION           */
    MAKE_TABLE(s_lifepo4_pts, "LiFePO4"),     /* 4 — LIFEPO4               */
    MAKE_TABLE(s_nimh_pts, "NiMH"),           /* 5 */
};

#define TABLE_COUNT ((uint8_t)(sizeof(s_tables) / sizeof(s_tables[0])))

/* ── Interpolation engine ────────────────────────────────────────────── */

uint8_t battery_soc_from_voltage(float voltage_v,
                                 float nominal_v,
                                 const soc_table_t *table)
{
    if (!table || table->count == 0 || nominal_v <= 0.0f)
        return 0;

    /* Scale measured voltage down to 12 V equivalent */
    float v12 = voltage_v * (12.0f / nominal_v);

    const soc_point_t *pts = table->points;
    uint8_t n = table->count;

    /* Above or at the highest voltage → 100 % */
    if (v12 >= pts[0].voltage_12v)
        return 100;

    /* Below or at the lowest voltage → 0 % */
    if (v12 <= pts[n - 1].voltage_12v)
        return 0;

    /* Linear search for the bracket [pts[i], pts[i+1]] containing v12.    */
    /* Tables are small (≤14 points) so linear search beats binary here.   */
    for (uint8_t i = 0; i < n - 1; i++)
    {
        float v_hi = pts[i].voltage_12v;
        float v_lo = pts[i + 1].voltage_12v;
        float s_hi = (float)pts[i].soc_pct;
        float s_lo = (float)pts[i + 1].soc_pct;

        if (v12 <= v_hi && v12 >= v_lo)
        {
            /* Linear interpolation between the two bracket points */
            float span = v_hi - v_lo;
            if (span < 0.001f)
                return (uint8_t)s_lo;         /* degenerate bracket */
            float frac = (v12 - v_lo) / span; /* 0.0 at lo, 1.0 at hi */
            float soc = s_lo + frac * (s_hi - s_lo);

            /* Clamp and round */
            if (soc < 0.0f)
                soc = 0.0f;
            if (soc > 100.0f)
                soc = 100.0f;
            return (uint8_t)(soc + 0.5f);
        }
    }

    return 0; /* should never reach here */
}

const soc_table_t *battery_soc_get_table(uint8_t chemistry)
{
    if (chemistry >= TABLE_COUNT)
        return &s_tables[0]; /* default to lead-acid */
    return &s_tables[chemistry];
}

/* ── Convenience wrapper — reads sys_state ───────────────────────────── */
/* sys_state is defined in main.c; forward-declare the minimum needed.     */
/* If you move this to a separate compilation unit, include your main header */
extern struct
{
    uint8_t chemistry;
    float nominal_voltage_actual_12v;
}
__attribute__((weak)) _soc_profile_stub; /* never used — silences linker */

/* The real implementation uses sys_state directly. */
#include "freertos/FreeRTOS.h" /* for portability — no OS calls made here */

/* Declared in main.c as part of system_state_t.battery_profile */
typedef struct battery_profile_s
{
    uint8_t chemistry;
    float nominal_voltage;
    /* ... other fields not needed here ... */
} battery_profile_ref_t;

extern battery_profile_ref_t *_get_active_battery_profile(void)
    __attribute__((weak));

uint8_t calculate_battery_percentage(float voltage)
{
    /*
     * Try to get the active profile chemistry and nominal voltage.
     * If sys_state is not accessible from this translation unit,
     * fall back to the lead-acid table with 12 V nominal.
     */
    uint8_t chemistry = 0; /* default: lead-acid                      */
    float nominal_v = 12.0f;

    /* Access sys_state.battery_profile via the extern declared in main.c.  */
    /* In practice this translation unit is compiled together with main.c,  */
    /* so we can use the extern directly.                                    */
    extern void *_sys_state_battery_profile_ptr(uint8_t *chem, float *nom_v);
    /* This symbol is provided by a shim at the bottom of battery_soc.c.   */
    _sys_state_battery_profile_ptr(&chemistry, &nominal_v);

    const soc_table_t *table = battery_soc_get_table(chemistry);
    return battery_soc_from_voltage(voltage, nominal_v, table);
}

/*
 * Shim: defined weak so main.c can override with the real sys_state access.
 * If not overridden, returns lead-acid defaults.
 */
__attribute__((weak)) void *_sys_state_battery_profile_ptr(uint8_t *chem, float *nom_v)
{
    *chem = 0; /* lead-acid */
    *nom_v = 12.0f;
    return NULL;
}