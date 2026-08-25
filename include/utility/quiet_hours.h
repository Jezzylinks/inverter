#ifndef QUIET_HOURS_H
#define QUIET_HOURS_H

#include <stdbool.h>
#include "stdint.h"

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

    /**
     * @brief Manually set the wall clock to today's date at HH:MM:00 and
     * mark the clock as known (equivalent to a real SNTP sync for the
     * purposes of quiet_hours_time_is_known()/_is_active()).
     *
     * NOTE: without a battery-backed external RTC, this can't account
     * for how long the device was powered off between boots -- on a
     * cold boot, quiet_hours_restore_manual_time() re-applies whatever
     * HH:MM was last entered as a best-effort starting point, but the
     * clock will drift by however long the device was actually off.
     * SNTP sync (once WiFi is available) always takes priority over
     * this if it happens.
     */
    void quiet_hours_set_manual_time(uint8_t hour, uint8_t minute);

    /**
     * @brief Re-apply the last manually-entered time at boot, if the
     * user has ever used the Set Time menu (sys_state.time_manually_set)
     * and SNTP hasn't already synced a real time first. Call this once,
     * after load_settings() restores manual_time_hour/_minute from NVS.
     */
    void quiet_hours_restore_manual_time(void);

#ifdef __cplusplus
}
#endif

#endif /* QUIET_HOURS_H */
