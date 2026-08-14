/**
 * @file wifi_scan.h
 * @brief Wi-Fi network scanning API.
 */
#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

esp_err_t wifi_scan_init(void);
esp_err_t wifi_scan_start_records(wifi_ap_record_t *records, uint16_t *count);
esp_err_t wifi_scan_start(char *output, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_SCAN_H */
