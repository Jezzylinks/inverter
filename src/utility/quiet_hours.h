#ifndef QUIET_HOURS_H
#define QUIET_HOURS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief One-time SNTP setup. Call once from init_hardware() or
     * equivalent boot-time init -- this only configures the SNTP client,
     * it doesn't block waiting for a sync (there's no network yet at
     * that point in boot anyway).
     */
    void quiet_hours_sntp_init(void);

    /**
     * @brief Kick off (or re-check) time sync. Safe to call repeatedly;
     * a no-op if already synced or SNTP is still waiting on a response.
     * Call this once WiFi actually connects (see start_wifi_connection()),
     * since SNTP needs a working network path to reach its server.
     */
    void quiet_hours_request_sync(void);

    /**
     * @brief Whether the system clock has ever been synced (via SNTP or
     * manually). If false, quiet_hours_is_active() always returns false --
     * enforcing a schedule against an unset/default (1970) clock would
     * silently mute the buzzer either all the time or never, for the
     * wrong reason, which is worse than just not enforcing it yet.
     */
    bool quiet_hours_time_is_known(void);

    /**
     * @brief Whether the buzzer should be muted right now per the
     * configured quiet-hours schedule (sys_state.quiet_hours_enabled/
     * _start/_end/utc_offset_hours). Handles an overnight window (e.g.
     * start=22, end=6) as well as a same-day window (e.g. start=13,
     * end=15).
     *
     * @return false if quiet hours are disabled, the clock isn't synced
     *         yet, or the current local hour is outside the window.
     */
    bool quiet_hours_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* QUIET_HOURS_H */
