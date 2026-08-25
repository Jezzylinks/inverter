/**
 * @file reconnect_backoff.c
 * @brief Exponential Backoff WiFi Reconnect Strategy
 */

#include "wifi/reconnect_backoff.h"
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_random.h"

static const char *TAG = "RECONNECT";

#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 60000
#define BACKOFF_JITTER_MS 500

/*----------------------------------------------------------
 * Backoff state
 *---------------------------------------------------------*/
typedef struct
{
    uint32_t attempt;
    uint32_t next_delay_ms;
    bool active;
    TimerHandle_t timer;
    reconnect_callback_t callback;
} backoff_state_t;

static backoff_state_t s_state = {0};
static SemaphoreHandle_t s_mutex = NULL;

/*----------------------------------------------------------
 * Calculate next delay with exponential backoff + jitter
 *---------------------------------------------------------*/
static uint32_t backoff_calculate_delay(uint32_t attempt)
{
    /* Exponential: 1s, 2s, 4s, 8s, 16s, 32s, max 60s */
    uint32_t delay = BACKOFF_MIN_MS * (1 << attempt);

    if (delay > BACKOFF_MAX_MS)
    {
        delay = BACKOFF_MAX_MS;
    }

    /* Add jitter to prevent thundering herd */
    uint32_t jitter = esp_random() % BACKOFF_JITTER_MS;
    delay += jitter;

    return delay;
}

/*----------------------------------------------------------
 * Timer callback
 *---------------------------------------------------------*/
static void backoff_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;

    ESP_LOGI(TAG, "Backoff timer fired, attempt %lu", s_state.attempt);

    if (s_state.callback)
    {
        s_state.callback(s_state.attempt);
    }

    /* Increment attempt for next time */
    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }

    s_state.attempt++;
    s_state.next_delay_ms = backoff_calculate_delay(s_state.attempt);
    s_state.active = false;

    if (s_mutex)
    {
        xSemaphoreGive(s_mutex);
    }
}

/*----------------------------------------------------------
 * Initialize
 *---------------------------------------------------------*/
esp_err_t reconnect_backoff_init(void)
{
    if (s_mutex != NULL)
    {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_state, 0, sizeof(s_state));

    s_state.timer = xTimerCreate("reconnect_timer",
                                 pdMS_TO_TICKS(BACKOFF_MIN_MS),
                                 pdFALSE, /* One-shot */
                                 NULL,
                                 backoff_timer_cb);

    if (s_state.timer == NULL)
    {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Exponential backoff initialized");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Deinitialize
 *---------------------------------------------------------*/
esp_err_t reconnect_backoff_deinit(void)
{
    if (s_mutex == NULL)
    {
        return ESP_OK;
    }

    reconnect_backoff_stop();

    if (s_state.timer != NULL)
    {
        xTimerDelete(s_state.timer, portMAX_DELAY);
        s_state.timer = NULL;
    }

    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;

    return ESP_OK;
}

/*----------------------------------------------------------
 * Start backoff timer
 *---------------------------------------------------------*/
esp_err_t reconnect_backoff_start(reconnect_callback_t callback)
{
    if (s_mutex == NULL || callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }

    /* Stop any existing timer */
    if (s_state.active)
    {
        xTimerStop(s_state.timer, portMAX_DELAY);
    }

    s_state.callback = callback;
    s_state.active = true;
    s_state.next_delay_ms = backoff_calculate_delay(s_state.attempt);

    if (s_mutex)
    {
        xSemaphoreGive(s_mutex);
    }

    ESP_LOGI(TAG, "Backoff started: attempt %lu, delay %lu ms",
             s_state.attempt, s_state.next_delay_ms);

    /* Change timer period and start */
    xTimerChangePeriod(s_state.timer, pdMS_TO_TICKS(s_state.next_delay_ms), portMAX_DELAY);
    xTimerStart(s_state.timer, portMAX_DELAY);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Stop backoff
 *---------------------------------------------------------*/
esp_err_t reconnect_backoff_stop(void)
{
    if (s_mutex == NULL)
    {
        return ESP_OK;
    }

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }

    if (s_state.active && s_state.timer != NULL)
    {
        xTimerStop(s_state.timer, portMAX_DELAY);
    }

    s_state.active = false;

    if (s_mutex)
    {
        xSemaphoreGive(s_mutex);
    }

    return ESP_OK;
}

/*----------------------------------------------------------
 * Reset attempt counter (call on successful connection)
 *---------------------------------------------------------*/
esp_err_t reconnect_backoff_reset(void)
{
    if (s_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }

    s_state.attempt = 0;
    s_state.next_delay_ms = BACKOFF_MIN_MS;
    s_state.active = false;

    if (s_state.timer != NULL)
    {
        xTimerStop(s_state.timer, portMAX_DELAY);
    }

    if (s_mutex)
    {
        xSemaphoreGive(s_mutex);
    }

    ESP_LOGI(TAG, "Backoff reset");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Get current attempt count
 *---------------------------------------------------------*/
uint32_t reconnect_backoff_get_attempts(void)
{
    uint32_t attempts = 0;

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        attempts = s_state.attempt;
        xSemaphoreGive(s_mutex);
    }

    return attempts;
}

/*----------------------------------------------------------
 * Is backoff active?
 *---------------------------------------------------------*/
bool reconnect_backoff_is_active(void)
{
    bool active = false;

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        active = s_state.active;
        xSemaphoreGive(s_mutex);
    }

    return active;
}