#ifndef BUZZER_EVENT_TASK_H
#define BUZZER_EVENT_TASK_H

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

#ifdef __cplusplus
}
#endif

#endif /* BUZZER_EVENT_TASK_H */