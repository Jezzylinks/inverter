#include "quiet_hours.h"

#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_sntp.h"
#include "esp_log.h"
#include "system_state.h"

extern system_state_t sys_state;

static const char *TAG = "QUIET_HOURS";
static volatile bool s_time_synced = false;

static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    ESP_LOGI(TAG, "System time synced via SNTP");
}

void quiet_hours_sntp_init(void)
{
    /* Classic esp_sntp API (stable across ESP-IDF 4.x/5.x) rather than
     * the newer esp_netif_sntp wrapper, which needs a more recent IDF
     * than this project may be pinned to. POLL mode re-syncs
     * periodically on its own once network access exists -- no need to
     * manually kick it beyond the one nudge in quiet_hours_request_sync(). */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    ESP_LOGI("QUIET", "REACHED HERE");
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}

void quiet_hours_request_sync(void)
{
    if (s_time_synced)
    {
        return;
    }

    /* Prompt an immediate retry instead of waiting out SNTP's own poll
     * interval, since this is called right when WiFi has just connected
     * and a network path to the NTP server may finally exist. */
    if (esp_sntp_enabled())
    {
        sntp_restart();
    }
}

bool quiet_hours_time_is_known(void)
{
    return s_time_synced;
}

bool quiet_hours_is_active(void)
{
    if (!sys_state.quiet_hours_enabled)
    {
        return false;
    }

    if (!quiet_hours_time_is_known())
    {
        /* Don't enforce a schedule against an unset (1970) clock --
         * that would mute the buzzer for the wrong reason, either
         * always or never depending on what hour epoch-0-plus-offset
         * happens to land on. */
        return false;
    }

    uint8_t start = sys_state.quiet_hours_start;
    uint8_t end = sys_state.quiet_hours_end;
    if (start == end)
    {
        /* Degenerate window (e.g. both left at 0) -- treat as "no
         * schedule" rather than either always-on or always-off. */
        return false;
    }

    time_t now = time(NULL);
    now += (time_t)sys_state.utc_offset_hours * 3600;

    struct tm tm_info;
    gmtime_r(&now, &tm_info); /* already manually offset above, so plain
                               * gmtime_r avoids needing setenv(TZ)/tzset()
                               * plumbing for a timezone we'd have to ask
                               * the user for anyway. */
    uint8_t hour = (uint8_t)tm_info.tm_hour;

    if (start < end)
    {
        return hour >= start && hour < end;
    }

    /* Overnight window, e.g. start=22, end=6 */
    return hour >= start || hour < end;
}

/* Standard portable trick for a timegm()-equivalent: not every newlib
 * build exposes timegm() itself, but setenv/tzset/mktime are always
 * available, so temporarily forcing TZ=UTC0 around mktime() gives the
 * same "interpret this struct tm as UTC" result without depending on a
 * possibly-missing GNU extension. */
static time_t utc_mktime(struct tm *tm_info)
{
    char *old_tz = getenv("TZ");
    char old_tz_buf[32] = {0};
    bool had_tz = (old_tz != NULL);
    if (had_tz)
    {
        snprintf(old_tz_buf, sizeof(old_tz_buf), "%s", old_tz);
    }

    setenv("TZ", "UTC0", 1);
    tzset();

    time_t result = mktime(tm_info);

    if (had_tz)
    {
        setenv("TZ", old_tz_buf, 1);
    }
    else
    {
        unsetenv("TZ");
    }
    tzset();

    return result;
}

void quiet_hours_set_manual_time(uint8_t hour, uint8_t minute)
{
    time_t now = time(NULL);
    struct tm tm_info;
    gmtime_r(&now, &tm_info);

    /* The user enters their LOCAL hour/minute; convert to the UTC hour
     * that needs to be stored so quiet_hours_is_active()'s later
     * (epoch + utc_offset_hours*3600) -> gmtime_r reconstruction
     * reproduces exactly what was entered here. */
    int utc_hour = (int)hour - sys_state.utc_offset_hours;
    if (utc_hour < 0)
    {
        utc_hour += 24;
        tm_info.tm_mday -= 1;
    }
    else if (utc_hour >= 24)
    {
        utc_hour -= 24;
        tm_info.tm_mday += 1;
    }

    tm_info.tm_hour = utc_hour;
    tm_info.tm_min = minute;
    tm_info.tm_sec = 0;

    time_t new_epoch = utc_mktime(&tm_info);
    struct timeval tv = {.tv_sec = new_epoch, .tv_usec = 0};
    settimeofday(&tv, NULL);

    s_time_synced = true;
    ESP_LOGI(TAG, "Manual time set: %02u:%02u local (UTC offset %d)",
             hour, minute, sys_state.utc_offset_hours);
}

void quiet_hours_restore_manual_time(void)
{
    if (!sys_state.time_manually_set)
    {
        return; /* user has never used the Set Time menu */
    }

    if (s_time_synced)
    {
        return; /* SNTP already got a real sync in first */
    }

    ESP_LOGI(TAG, "Restoring last manually-entered time at boot "
                  "(cannot account for time elapsed while powered off)");
    quiet_hours_set_manual_time(sys_state.manual_time_hour, sys_state.manual_time_minute);
}
