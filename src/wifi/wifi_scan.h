/**
 * @file wifi_scan.h
 * @brief Wi-Fi Network Scanner
 */

#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>

#include "esp_err.h"

    /**
     * @brief Initialize WiFi scanner
     */
    esp_err_t wifi_scan_init(void);

    /**
     * @brief Scan available networks
     *
     * @param output Buffer to store HTML result
     * @param max_len Buffer size
     *
     */
    esp_err_t wifi_scan_start(
        char *output,
        size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_SCAN_H */