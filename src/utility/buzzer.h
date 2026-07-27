#ifndef BUZZER_EVENT_TASK_H
#define BUZZER_EVENT_TASK_H

#include <stdint.h>
#include <stdbool.h>

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
    void buzzer_event_task(void *pvParameters);
    void buzzer_init(void);
    void post_buzzer_event(bool success);

    /* Lower-level API, still called directly from a few places (e.g. the
     * Sound On/Off settings toggle, ad hoc feedback outside the event
     * pipeline). All of these respect sys_state.sound_enabled. */
    void buzzer_beep(uint32_t frequency, uint8_t duty_percent, uint32_t duration_ms);
    void update_buzzer(uint16_t freq_hz, uint8_t volume_percent);
    void buzzer_off(void);
    void buzzer_alert(void);
    void buzzer_error(void);
    void buzzer_success(void);

#ifdef __cplusplus
}
#endif

#endif /* BUZZER_EVENT_TASK_H */