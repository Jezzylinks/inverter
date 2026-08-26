#ifndef BUZZER_EVENT_TASK_H
#define BUZZER_EVENT_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief FreeRTOS task that consumes buzzer events from the event dispatcher.
     *
     * Subscribe to EVENT_SUB_BUZZER, then start this task.  All audio feedback
     * for protection, system and button events is handled here.
     */
    typedef enum
    {
        BUZZER_PATTERN_NONE = 0,
        BUZZER_PATTERN_CLICK,
        BUZZER_PATTERN_LIMIT,
        BUZZER_PATTERN_SUCCESS,
        BUZZER_PATTERN_ERROR,
        BUZZER_PATTERN_CRITICAL,
        BUZZER_PATTERN_WARNING,
        BUZZER_PATTERN_DERATE,
        BUZZER_PATTERN_SHUTDOWN,
        BUZZER_PATTERN_RECOVERED,
        BUZZER_PATTERN_ON,
        BUZZER_PATTERN_OFF,
        BUZZER_PATTERN_COUNT
    } buzzer_pattern_t;

    typedef struct
    {
        bool initialized;
        bool enabled;
        uint32_t requests_received;
        uint32_t requests_played;
        uint32_t requests_dropped;
        uint32_t queue_overflows;
        buzzer_pattern_t last_pattern;
        uint32_t last_play_timestamp_ms;
        buzzer_pattern_t current_pattern;
    } buzzer_diagnostic_t;

    void buzzer_event_task(void *pvParameters);
    esp_err_t buzzer_init(void);
    esp_err_t buzzer_self_test(void);
    esp_err_t buzzer_get_diagnostic(buzzer_diagnostic_t *out);
    /** Record whether the dispatcher accepted a buzzer subscriber request. */
    void buzzer_record_dispatch_result(bool accepted);
    void post_buzzer_event(bool success);
    void buzzer_button_click(void);
    void post_buzzer_limit_event(void);

    /* Lower-level API, still called directly from a few places (e.g. the
     * Sound On/Off settings toggle, ad hoc feedback outside the event
     * pipeline). All of these respect sys_state.sound_enabled. */
    void buzzer_beep(uint32_t frequency, uint8_t duty_percent, uint32_t duration_ms);
    void update_buzzer(uint16_t freq_hz, uint8_t volume_percent);
    void buzzer_off(void);
    void buzzer_request_critical_preemption(void);
    void buzzer_alert(void);
    void buzzer_error(void);
    void buzzer_success(void);

#ifdef __cplusplus
}
#endif

#endif /* BUZZER_EVENT_TASK_H */