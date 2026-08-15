#include "wifi_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wifi_config.h"

#define WIFI_SCAN_TAG "WIFI_SCAN"

static bool s_initialized;
static SemaphoreHandle_t s_scan_mutex;

static int compare_ap_by_rssi(const void *a, const void *b)
{
    const wifi_ap_record_t *ap_a = a;
    const wifi_ap_record_t *ap_b = b;
    return (int)ap_b->rssi - (int)ap_a->rssi;
}

static void escape_html_text(const char *src, char *dst, size_t dst_len)
{
    if (dst == NULL || dst_len == 0U) {
        return;
    }
    size_t out = 0U;
    for (size_t i = 0U; src != NULL && src[i] != '\0' && out + 1U < dst_len; ++i) {
        const char *replacement = NULL;
        switch (src[i]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&#39;"; break;
        default: break;
        }
        if (replacement != NULL) {
            const size_t replacement_len = strlen(replacement);
            if (out + replacement_len >= dst_len) {
                break;
            }
            memcpy(dst + out, replacement, replacement_len);
            out += replacement_len;
        } else {
            const unsigned char value = (unsigned char)src[i];
            dst[out++] = value >= 0x20U && value != 0x7FU ? src[i] : '?';
        }
    }
    dst[out] = '\0';
}

esp_err_t wifi_scan_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_scan_mutex = xSemaphoreCreateMutex();
    if (s_scan_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_scan_deinit(void)
{
    if (s_scan_mutex != NULL) {
        vSemaphoreDelete(s_scan_mutex);
        s_scan_mutex = NULL;
    }
    s_initialized = false;
    return ESP_OK;
}

esp_err_t wifi_scan_start_records(wifi_ap_record_t *records, uint16_t *count)
{
    if (!s_initialized || records == NULL || count == NULL || *count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_scan_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    wifi_scan_config_t config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0U,
        .show_hidden = WIFI_SCAN_SHOW_HIDDEN,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK) {
        xSemaphoreGive(s_scan_mutex);
        ESP_LOGW(WIFI_SCAN_TAG, "Scan start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t available = 0U;
    err = esp_wifi_scan_get_ap_num(&available);
    if (err == ESP_OK) {
        const uint16_t capacity = *count;
        if (available > capacity) {
            available = capacity;
        }
        if (available > 0U) {
            err = esp_wifi_scan_get_ap_records(&available, records);
            if (err == ESP_OK) {
                uint16_t unique = 0U;
                for (uint16_t i = 0U; i < available; ++i) {
                    bool duplicate = false;
                    for (uint16_t j = 0U; j < unique; ++j) {
                        if (strncmp((const char *)records[j].ssid,
                                    (const char *)records[i].ssid,
                                    sizeof(records[i].ssid)) == 0) {
                            duplicate = true;
                            if (records[i].rssi > records[j].rssi) {
                                records[j] = records[i];
                            }
                            break;
                        }
                    }
                    if (!duplicate && unique < capacity) {
                        records[unique++] = records[i];
                    }
                }
                available = unique;
                qsort(records, available, sizeof(records[0]), compare_ap_by_rssi);
            }
        }
        *count = err == ESP_OK ? available : 0U;
    } else {
        *count = 0U;
    }
    xSemaphoreGive(s_scan_mutex);

    if (err == ESP_OK) {
        ESP_LOGI(WIFI_SCAN_TAG, "Found %u access points", (unsigned)*count);
    }
    return err;
}

esp_err_t wifi_scan_start(char *output, size_t max_len)
{
    if (output == NULL || max_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    wifi_ap_record_t records[WIFI_MAX_SCAN_RESULTS] = {0};
    uint16_t count = WIFI_MAX_SCAN_RESULTS;
    esp_err_t err = wifi_scan_start_records(records, &count);
    if (err != ESP_OK) {
        return err;
    }
    if (count == 0U) {
        (void)snprintf(output, max_len, "<p>No networks found</p>");
        return ESP_OK;
    }

    size_t used = 0U;
    for (uint16_t i = 0U; i < count; ++i) {
        char escaped[128] = {0};
        escape_html_text((const char *)records[i].ssid, escaped, sizeof(escaped));
        const char *quality = records[i].rssi >= WIFI_RSSI_EXCELLENT ? "Excellent" :
                              records[i].rssi >= WIFI_RSSI_GOOD ? "Good" :
                              records[i].rssi >= WIFI_RSSI_FAIR ? "Fair" :
                              records[i].rssi >= WIFI_RSSI_WEAK ? "Weak" : "Very Weak";
        const int written = snprintf(output + used, max_len - used,
                                     "<p><strong>%s</strong> — %s (%d dBm)</p>",
                                     escaped[0] != '\0' ? escaped : "<hidden>",
                                     quality, records[i].rssi);
        if (written < 0 || (size_t)written >= max_len - used) {
            break;
        }
        used += (size_t)written;
    }
    return ESP_OK;
}
