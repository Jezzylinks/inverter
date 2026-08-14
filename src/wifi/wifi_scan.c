#include "wifi_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "wifi_config.h"

static const char *TAG = "WIFI_SCAN";
static bool s_initialized;

static int compare_ap_by_rssi(const void *a, const void *b)
{
    const wifi_ap_record_t *ap_a = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *ap_b = (const wifi_ap_record_t *)b;
    return (int)ap_b->rssi - (int)ap_a->rssi;
}

static void escape_html_text(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0U) {
        return;
    }
    size_t out = 0U;
    for (size_t i = 0; src && src[i] != '\0' && out + 1U < dst_len; ++i) {
        const char *replacement = NULL;
        switch (src[i]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '\"': replacement = "&quot;"; break;
        case '\'': replacement = "&#39;"; break;
        default: break;
        }
        if (replacement) {
            const size_t length = strlen(replacement);
            if (out + length >= dst_len) break;
            memcpy(dst + out, replacement, length);
            out += length;
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

esp_err_t wifi_scan_init(void)
{
    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_scan_start_records(wifi_ap_record_t *records, uint16_t *count)
{
    if (!s_initialized || !records || !count || *count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = WIFI_SCAN_SHOW_HIDDEN,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t available = 0U;
    err = esp_wifi_scan_get_ap_num(&available);
    if (err != ESP_OK) {
        return err;
    }
    const uint16_t capacity = *count;
    if (available > capacity) {
        available = capacity;
    }
    if (available > 0U) {
        err = esp_wifi_scan_get_ap_records(&available, records);
        if (err != ESP_OK) {
            *count = 0U;
            return err;
        }
        qsort(records, available, sizeof(records[0]), compare_ap_by_rssi);
    }
    *count = available;
    ESP_LOGI(TAG, "Found %u networks", (unsigned)available);
    return ESP_OK;
}

esp_err_t wifi_scan_start(char *output, size_t max_len)
{
    if (!output || max_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(output, 0, max_len);

    wifi_ap_record_t records[WIFI_MAX_SCAN_RESULTS] = {0};
    uint16_t count = WIFI_MAX_SCAN_RESULTS;
    esp_err_t err = wifi_scan_start_records(records, &count);
    if (err != ESP_OK) {
        return err;
    }
    if (count == 0U) {
        snprintf(output, max_len, "<p>No networks found</p>");
        return ESP_OK;
    }

    size_t used = 0U;
    for (uint16_t i = 0U; i < count && used + 1U < max_len; ++i) {
        char escaped[128] = {0};
        escape_html_text((const char *)records[i].ssid, escaped, sizeof(escaped));
        const char *quality = (records[i].rssi >= WIFI_RSSI_EXCELLENT) ? "Excellent" :
                              (records[i].rssi >= WIFI_RSSI_GOOD) ? "Good" :
                              (records[i].rssi >= WIFI_RSSI_FAIR) ? "Fair" :
                              (records[i].rssi >= WIFI_RSSI_WEAK) ? "Weak" : "Very Weak";
        const int written = snprintf(output + used, max_len - used,
                                     "<div class=\"network\"><div class=\"net-info\">"
                                     "<span class=\"net-name\">%s</span>"
                                     "<span class=\"net-quality %s\">%s (%d dBm)</span>"
                                     "</div></div>", escaped,
                                     (records[i].rssi >= WIFI_RSSI_FAIR) ? "good" : "weak",
                                     quality, records[i].rssi);
        if (written < 0 || (size_t)written >= max_len - used) {
            break;
        }
        used += (size_t)written;
    }
    return ESP_OK;
}
