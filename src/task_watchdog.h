#ifndef TASK_WATCHDOG_H
#define TASK_WATCHDOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register the calling FreeRTOS task with the ESP task watchdog. */
void task_watchdog_register(const char *task_name);

/* Feed the watchdog for the calling task. */
void task_watchdog_feed(void);

#ifdef __cplusplus
}
#endif

#endif /* TASK_WATCHDOG_H */
