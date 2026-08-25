/**
 * @file signal_history.c
 * @brief WiFi Signal Strength History & Trending
 */

#include "server/signal/signal_history.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "SIGNAL_HISTORY";

static signal_history_t s_history;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_initialized = false;

/*----------------------------------------------------------
 * Initialize history buffer
 *---------------------------------------------------------*/
esp_err_t signal_history_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_history, 0, sizeof(s_history));
    s_history.count = 0;
    s_history.head = 0;
    s_history.min_rssi = 0;
    s_history.max_rssi = -127;
    s_history.avg_rssi = 0;

    s_initialized = true;

    ESP_LOGI(TAG, "Signal history initialized (capacity=%d)", SIGNAL_HISTORY_SIZE);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Deinitialize
 *---------------------------------------------------------*/
esp_err_t signal_history_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_OK;
    }

    if (s_mutex)
    {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    memset(&s_history, 0, sizeof(s_history));
    s_initialized = false;

    return ESP_OK;
}

/*----------------------------------------------------------
 * Record a new RSSI sample
 *---------------------------------------------------------*/
esp_err_t signal_history_record(int8_t rssi, uint32_t timestamp_ms)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }

    /* Store sample */
    s_history.samples[s_history.head].rssi = rssi;
    s_history.samples[s_history.head].timestamp_ms = timestamp_ms;
    s_history.head = (s_history.head + 1) % SIGNAL_HISTORY_SIZE;

    if (s_history.count < SIGNAL_HISTORY_SIZE)
    {
        s_history.count++;
    }

    /* Recalculate statistics */
    int32_t sum = 0;
    int8_t min_r = 0;
    int8_t max_r = -127;
    uint16_t valid = 0;

    for (uint16_t i = 0; i < s_history.count; i++)
    {
        int8_t sample = s_history.samples[i].rssi;
        if (sample > -127) /* Valid signal */
        {
            sum += sample;
            if (sample < min_r || valid == 0)
                min_r = sample;
            if (sample > max_r)
                max_r = sample;
            valid++;
        }
    }

    if (valid > 0)
    {
        s_history.avg_rssi = (int8_t)(sum / valid);
        s_history.min_rssi = min_r;
        s_history.max_rssi = max_r;
    }

    /* Calculate trend */
    if (s_history.count >= 3)
    {
        int16_t recent = 0, older = 0;
        uint16_t half = s_history.count / 2;

        for (uint16_t i = 0; i < half; i++)
        {
            uint16_t idx = (s_history.head + SIGNAL_HISTORY_SIZE - 1 - i) % SIGNAL_HISTORY_SIZE;
            recent += s_history.samples[idx].rssi;
        }

        for (uint16_t i = half; i < s_history.count && i < half * 2; i++)
        {
            uint16_t idx = (s_history.head + SIGNAL_HISTORY_SIZE - 1 - i) % SIGNAL_HISTORY_SIZE;
            older += s_history.samples[idx].rssi;
        }

        int16_t diff = (recent / half) - (older / half);

        if (diff > SIGNAL_TREND_THRESHOLD)
        {
            s_history.trend = SIGNAL_TREND_IMPROVING;
        }
        else if (diff < -SIGNAL_TREND_THRESHOLD)
        {
            s_history.trend = SIGNAL_TREND_WORSENING;
        }
        else
        {
            s_history.trend = SIGNAL_TREND_STABLE;
        }
    }
    else
    {
        s_history.trend = SIGNAL_TREND_UNKNOWN;
    }

    if (s_mutex)
    {
        xSemaphoreGive(s_mutex);
    }

    return ESP_OK;
}

/*----------------------------------------------------------
 * Get current statistics
 *---------------------------------------------------------*/
esp_err_t signal_history_get_stats(signal_history_t *out)
{
    if (out == NULL || !s_initialized)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }

    memcpy(out, &s_history, sizeof(signal_history_t));

    if (s_mutex)
    {
        xSemaphoreGive(s_mutex);
    }

    return ESP_OK;
}

/*----------------------------------------------------------
 * Get average RSSI
 *---------------------------------------------------------*/
int8_t signal_history_get_average(void)
{
    if (!s_initialized)
    {
        return -127;
    }

    int8_t avg;

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        avg = s_history.avg_rssi;
        xSemaphoreGive(s_mutex);
    }
    else
    {
        avg = s_history.avg_rssi;
    }

    return avg;
}

/*----------------------------------------------------------
 * Get trend direction
 *---------------------------------------------------------*/
signal_trend_t signal_history_get_trend(void)
{
    if (!s_initialized)
    {
        return SIGNAL_TREND_UNKNOWN;
    }

    signal_trend_t trend;

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        trend = s_history.trend;
        xSemaphoreGive(s_mutex);
    }
    else
    {
        trend = s_history.trend;
    }

    return trend;
}

/*----------------------------------------------------------
 * Get min/max RSSI
 *---------------------------------------------------------*/
esp_err_t signal_history_get_min_max(int8_t *min_rssi, int8_t *max_rssi)
{
    if (min_rssi == NULL || max_rssi == NULL || !s_initialized)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        *min_rssi = s_history.min_rssi;
        *max_rssi = s_history.max_rssi;
        xSemaphoreGive(s_mutex);
    }
    else
    {
        *min_rssi = s_history.min_rssi;
        *max_rssi = s_history.max_rssi;
    }

    return ESP_OK;
}

/*----------------------------------------------------------
 * Clear history
 *---------------------------------------------------------*/
esp_err_t signal_history_clear(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }

    memset(&s_history.samples, 0, sizeof(s_history.samples));
    s_history.count = 0;
    s_history.head = 0;
    s_history.min_rssi = 0;
    s_history.max_rssi = -127;
    s_history.avg_rssi = 0;
    s_history.trend = SIGNAL_TREND_UNKNOWN;

    if (s_mutex)
    {
        xSemaphoreGive(s_mutex);
    }

    ESP_LOGI(TAG, "Signal history cleared");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Get samples as JSON string
 *---------------------------------------------------------*/
esp_err_t signal_history_to_json(char *buffer, size_t max_len)
{
    if (buffer == NULL || max_len == 0 || !s_initialized)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }

    int written = snprintf(buffer, max_len,
                           "{\"count\":%u,\"avg\":%d,\"min\":%d,\"max\":%d,\"trend\":\"%s\",\"samples\":[",
                           s_history.count,
                           s_history.avg_rssi,
                           s_history.min_rssi,
                           s_history.max_rssi,
                           s_history.trend == SIGNAL_TREND_IMPROVING ? "improving" : s_history.trend == SIGNAL_TREND_WORSENING ? "worsening"
                                                                                 : s_history.trend == SIGNAL_TREND_STABLE      ? "stable"
                                                                                                                               : "unknown");

    if (written < 0 || (size_t)written >= max_len)
    {
        if (s_mutex)
            xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    size_t pos = written;

    for (uint16_t i = 0; i < s_history.count && pos < max_len - 32; i++)
    {
        uint16_t idx = (s_history.head + SIGNAL_HISTORY_SIZE - s_history.count + i) % SIGNAL_HISTORY_SIZE;
        int n = snprintf(buffer + pos, max_len - pos,
                         "%s{\"rssi\":%d,\"time\":%lu}",
                         i > 0 ? "," : "",
                         s_history.samples[idx].rssi,
                         (unsigned long)s_history.samples[idx].timestamp_ms);

        if (n < 0 || (size_t)n >= max_len - pos)
        {
            break;
        }
        pos += n;
    }

    if (pos < max_len - 2)
    {
        buffer[pos++] = ']';
        buffer[pos++] = '}';
        buffer[pos] = '\0';
    }
    else
    {
        buffer[max_len - 1] = '\0';
    }

    if (s_mutex)
    {
        xSemaphoreGive(s_mutex);
    }

    return ESP_OK;
}