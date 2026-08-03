/**
 * @file wifi_scan.c
 * @brief Wi-Fi Network Scanner
 */

#include "wifi_scan.h"
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "wifi_config.h"

static const char *TAG = "WIFI_SCAN";

static bool s_initialized = false;

/*----------------------------------------------------------
 *
 * INIT
 *
 *---------------------------------------------------------*/

esp_err_t wifi_scan_init(void)
{
    s_initialized = true;

    ESP_LOGI(TAG, "WiFi scanner initialized");

    return ESP_OK;
}

/*----------------------------------------------------------
 *
 * SCAN
 *
 *---------------------------------------------------------*/

/* Compare function for sorting APs by RSSI (descending) */
static int compare_ap_by_rssi(const void *a, const void *b)
{
    const wifi_ap_record_t *ap_a = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *ap_b = (const wifi_ap_record_t *)b;
    return ap_b->rssi - ap_a->rssi; /* Descending order */
}

esp_err_t wifi_scan_start(
    char *output,
    size_t max_len)
{

    if (output == NULL ||
        max_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(output,
           0,
           max_len);

    wifi_scan_config_t scan_config =
        {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = WIFI_SCAN_SHOW_HIDDEN};

    ESP_LOGI(TAG,
             "Starting WiFi scan");

    esp_err_t err =
        esp_wifi_scan_start(
            &scan_config,
            true);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Scan failed %s",
                 esp_err_to_name(err));

        return err;
    }

    uint16_t ap_count = 0;

    esp_wifi_scan_get_ap_num(
        &ap_count);

    if (ap_count == 0)
    {
        snprintf(output,
                 max_len,
                 "<p>No networks found</p>");

        return ESP_OK;
    }

    /* Limit to max results */
    if (ap_count > WIFI_MAX_SCAN_RESULTS)
    {
        ap_count = WIFI_MAX_SCAN_RESULTS;
    }

    wifi_ap_record_t *records =
        calloc(ap_count,
               sizeof(wifi_ap_record_t));

    if (records == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_wifi_scan_get_ap_records(
        &ap_count,
        records);

    /* Sort by RSSI (strongest first) */
    qsort(records, ap_count, sizeof(wifi_ap_record_t), compare_ap_by_rssi);

    size_t used = 0;

    for (int i = 0;
         i < ap_count;
         i++)
    {

        /* Simple escape: replace ' with \\' in a temp buffer */
        char escaped_ssid[65] = {0};
        const char *src = (char *)records[i].ssid;
        size_t dst_idx = 0;
        for (size_t j = 0; src[j] != '\0' && dst_idx < sizeof(escaped_ssid) - 2; j++)
        {
            if (src[j] == '\'')
            {
                escaped_ssid[dst_idx++] = '\\';
            }
            escaped_ssid[dst_idx++] = src[j];
        }
        escaped_ssid[dst_idx] = '\0';

        /* Determine signal quality */
        const char *quality;
        if (records[i].rssi >= WIFI_RSSI_EXCELLENT)
            quality = "Excellent";
        else if (records[i].rssi >= WIFI_RSSI_GOOD)
            quality = "Good";
        else if (records[i].rssi >= WIFI_RSSI_FAIR)
            quality = "Fair";
        else if (records[i].rssi >= WIFI_RSSI_WEAK)
            quality = "Weak";
        else
            quality = "Very Weak";

        int written = snprintf(output + used,
                               max_len - used,
                               "<div class=\"network\">"
                               "<div class=\"net-info\">"
                               "<span class=\"net-name\">%s</span>"
                               "<span class=\"net-quality %s\">%s (%d dBm)</span>"
                               "</div>"
                               "<a class=\"btn-select\" href=\"/select?ssid=%s\">Select</a>"
                               "</div>",
                               escaped_ssid,
                               (records[i].rssi >= WIFI_RSSI_FAIR) ? "good" : "weak",
                               quality,
                               records[i].rssi,
                               escaped_ssid);

        if (written < 0 ||
            used + written >= max_len)
        {
            break;
        }

        used += written;
    }

    free(records);

    ESP_LOGI(TAG,
             "Found %d networks",
             ap_count);

    return ESP_OK;
}