/**
 * @file wifi_scan.c
 * @brief Wi-Fi Network Scanner
 */

#include "wifi_scan.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG =
    "WIFI_SCAN";

static bool s_initialized = false;

/*==========================================================
 *
 *              INIT
 *
 *=========================================================*/

esp_err_t wifi_scan_init(void)
{
    s_initialized = true;

    ESP_LOGI(TAG,
             "WiFi scanner initialized");

    return ESP_OK;
}

/*==========================================================
 *
 *              SCAN
 *
 *=========================================================*/

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
            .show_hidden = false};

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
                 "No networks found");

        return ESP_OK;
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

    size_t used = 0;

    for (int i = 0;
         i < ap_count;
         i++)
    {

        /* Simple escape: replace ' with \' in a temp buffer */
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

        int written = snprintf(output + used,
                               max_len - used,
                               "<p>"
                               "%d. <button onclick=\"choose('%s')\">"
                               "%s"
                               "</button>"
                               "<br>"
                               "RSSI: %d dBm<br>"
                               "Channel: %d"
                               "</p>",
                               i + 1,                   /* 1. Numbering */
                               escaped_ssid,            /* 2. choose() argument */
                               (char *)records[i].ssid, /* 3. Button text */
                               records[i].rssi,         /* 4. RSSI */
                               records[i].primary);     /* 5. Channel */

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