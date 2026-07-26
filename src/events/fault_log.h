/**
 * fault_log.h
 *
 * System-wide fault/event ring buffer. This is deliberately a level
 * below your existing error_log_scroll.h/.c: error_log_scroll owns the
 * LCD-facing scrollable presentation, while fault_log is the single
 * source of truth that ANY subsystem (protection.c, watchdog.c,
 * restart_policy.c, button controller, NVS layer, etc.) writes into.
 * error_log_scroll can subscribe to this (via fault_log_get_entries)
 * rather than each subsystem inventing its own ad-hoc logging path -
 * that fragmentation is exactly what bit you with the flash-message
 * and save/load drift bugs earlier in this project.
 *
 * Storage: RAM ring buffer (fast, always available) + periodic
 * snapshot to NVS so history survives a reboot. We do NOT write NVS
 * on every log_add() call - flash wear and timing make that a bad
 * idea on an interrupt/fault path. Instead, critical faults set a
 * dirty flag and a low-priority task flushes to NVS on a timer AND
 * on graceful shutdown/brownout notification.
 */

#ifndef FAULT_LOG_H
#define FAULT_LOG_H

#include <stdint.h>
#include <stdbool.h>
#include "system_events.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define FAULT_LOG_CAPACITY 32
#define FAULT_LOG_NVS_NAMESPACE "fault_log"
#define FAULT_LOG_NVS_KEY "entries_v1"

    typedef enum
    {
        FAULT_SEV_INFO = 0, // e.g. recovered-from-warning, normal restart
        FAULT_SEV_WARNING,  // approaching a limit
        FAULT_SEV_DERATED,  // output reduced
        FAULT_SEV_FAULT,    // output shut down
        FAULT_SEV_CRITICAL  // hardware-level concern (brownout, watchdog reset, corruption)
    } fault_severity_t;

    typedef enum
    {
        FAULT_SRC_PROTECTION_AC_VOLTAGE = 0,
        FAULT_SRC_PROTECTION_CURRENT,
        FAULT_SRC_PROTECTION_TEMPERATURE,
        FAULT_SRC_PROTECTION_BATTERY,
        FAULT_SRC_WATCHDOG,
        FAULT_SRC_BROWNOUT,
        FAULT_SRC_NVS,
        FAULT_SRC_LCD,
        FAULT_SRC_BUTTON,
        FAULT_SRC_RESTART_POLICY,
        FAULT_SRC_FAN,
        FAULT_SRC_POWER_METER,
        FAULT_SRC_OTHER,
        FAULT_SRC_COUNT
    } fault_source_t;

    typedef struct
    {
        uint32_t timestamp_ms; // millis since boot; caller may also stamp with RTC if available
        fault_severity_t severity;
        fault_source_t source;
        float value;      // reading that triggered the event, if applicable
        char message[48]; // short human-readable note, NUL-terminated
    } fault_log_entry_t;

    /* Call once at startup, after NVS is initialized. Attempts to load
     * prior history from NVS; starts with an empty log if none exists or
     * the stored blob fails a checksum check. */
    bool fault_log_init(void);

    /* Add an entry. Safe to call from any task (mutex-protected) but NOT
     * from an ISR - use fault_log_add_from_isr for that path. */
    void fault_log_add(fault_severity_t severity, fault_source_t source, float value, const char *message);
    void fault_log_add_event(const system_event_t *evt);

    /* ISR-safe variant: pushes into a small lock-free staging buffer that
     * a background task drains into the main ring buffer. Never blocks. */
    void fault_log_add_from_isr(fault_severity_t severity, fault_source_t source, float value);

    /* Copies up to max_entries into out_entries, most recent first.
     * Returns the number actually copied. */
    uint32_t fault_log_get_entries(fault_log_entry_t *out_entries, uint32_t max_entries);

    /* Force an immediate NVS flush (e.g. called from brownout handler or
     * before a deliberate factory reset / firmware update). */
    bool fault_log_flush_to_nvs(void);

    /* Clears the in-RAM log and the persisted NVS copy. Used by factory
     * reset flow - gate this behind the existing PIN-protected flow. */
    bool fault_log_clear(void);

    const char *fault_log_severity_name(fault_severity_t s);
    const char *fault_log_source_name(fault_source_t s);
    void fault_log_event_task(void *pv);

#ifdef __cplusplus
}
#endif

#endif // FAULT_LOG_H