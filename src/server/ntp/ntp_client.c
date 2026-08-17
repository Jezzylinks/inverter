/**
 * @file ntp_client.c
 * @brief NTP Client for time synchronization
 */

#include "ntp_client.h"
#include <string.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_netif.h"

static const char *TAG = "NTP_CLIENT";

static bool s_initialized = false;
static ntp_sync_callback_t s_sync_callback = NULL;

/*----------------------------------------------------------
 * SNTP callback
 *---------------------------------------------------------*/
static void ntp_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized: %lld seconds since epoch", (long long)tv->tv_sec);

    if (s_sync_callback)
    {
        s_sync_callback(tv);
    }
}

/*----------------------------------------------------------
 * Initialize NTP
 *---------------------------------------------------------*/
esp_err_t ntp_client_init(const char *server)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    if (server == NULL)
    {
        server = NTP_DEFAULT_SERVER;
    }

    ESP_LOGI(TAG, "Initializing SNTP with server: %s", server);

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, server);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
    esp_sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);
    esp_sntp_set_time_sync_notification_cb(ntp_sync_cb);
    esp_sntp_init();

    s_initialized = true;

    return ESP_OK;
}

/*----------------------------------------------------------
 * Deinitialize NTP
 *---------------------------------------------------------*/
esp_err_t ntp_client_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_OK;
    }

    esp_sntp_stop();
    s_initialized = false;

    ESP_LOGI(TAG, "SNTP stopped");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Force sync
 *---------------------------------------------------------*/
esp_err_t ntp_client_sync_now(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_sntp_restart();

    ESP_LOGI(TAG, "SNTP sync requested");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Check if time is set
 *---------------------------------------------------------*/
bool ntp_client_time_is_set(void)
{
    time_t now = time(NULL);
    return (now > NTP_MIN_VALID_TIME);
}

/*----------------------------------------------------------
 * Get formatted time string
 *---------------------------------------------------------*/
esp_err_t ntp_client_get_time_string(char *buffer, size_t len)
{
    if (buffer == NULL || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    strftime(buffer, len, "%Y-%m-%d %H:%M:%S", &timeinfo);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Register sync callback
 *---------------------------------------------------------*/
esp_err_t ntp_client_register_callback(ntp_sync_callback_t callback)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_sync_callback = callback;
    return ESP_OK;
}