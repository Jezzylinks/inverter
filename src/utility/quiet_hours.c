#include "quiet_hours.h"

#include <time.h>
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
