/**
 * @file signal_history.h
 * @brief WiFi Signal Strength History Interface
 */

#ifndef SIGNAL_HISTORY_H
#define SIGNAL_HISTORY_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define SIGNAL_HISTORY_SIZE 120  /* 10 minutes at 5s interval */
#define SIGNAL_TREND_THRESHOLD 3 /* dBm change for trend detection */

    typedef enum
    {
        SIGNAL_TREND_UNKNOWN = 0,
        SIGNAL_TREND_IMPROVING,
        SIGNAL_TREND_WORSENING,
        SIGNAL_TREND_STABLE,
    } signal_trend_t;

    typedef struct
    {
        int8_t rssi;
        uint32_t timestamp_ms;
    } signal_sample_t;

    typedef struct
    {
        signal_sample_t samples[SIGNAL_HISTORY_SIZE];
        uint16_t count;
        uint16_t head;
        int8_t min_rssi;
        int8_t max_rssi;
        int8_t avg_rssi;
        signal_trend_t trend;
    } signal_history_t;

    /**
     * @brief Initialize signal history buffer
     * @return ESP_OK on success
     */
    esp_err_t signal_history_init(void);

    /**
     * @brief Deinitialize signal history
     * @return ESP_OK on success
     */
    esp_err_t signal_history_deinit(void);

    /**
     * @brief Record a new RSSI sample
     * @param rssi Signal strength in dBm
     * @param timestamp_ms Timestamp in milliseconds
     * @return ESP_OK on success
     */
    esp_err_t signal_history_record(int8_t rssi, uint32_t timestamp_ms);

    /**
     * @brief Get current statistics
     * @param out Output structure
     * @return ESP_OK on success
     */
    esp_err_t signal_history_get_stats(signal_history_t *out);

    /**
     * @brief Get average RSSI
     * @return Average RSSI or -127 if no data
     */
    int8_t signal_history_get_average(void);

    /**
     * @brief Get current trend direction
     * @return Trend enum value
     */
    signal_trend_t signal_history_get_trend(void);

    /**
     * @brief Get minimum and maximum RSSI recorded
     * @param min_rssi Output minimum
     * @param max_rssi Output maximum
     * @return ESP_OK on success
     */
    esp_err_t signal_history_get_min_max(int8_t *min_rssi, int8_t *max_rssi);

    /**
     * @brief Clear all history data
     * @return ESP_OK on success
     */
    esp_err_t signal_history_clear(void);

    /**
     * @brief Export history as JSON string
     * @param buffer Output buffer
     * @param max_len Buffer size
     * @return ESP_OK on success
     */
    esp_err_t signal_history_to_json(char *buffer, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* SIGNAL_HISTORY_H */