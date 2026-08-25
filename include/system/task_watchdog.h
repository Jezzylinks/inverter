#ifndef TASK_WATCHDOG_H
#define TASK_WATCHDOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TASK_WATCHDOG_MAX_TASKS 32U
#define TASK_WATCHDOG_STALE_MS 10000U
#define TASK_WATCHDOG_NAME_LENGTH 24U

typedef struct {
    char name[TASK_WATCHDOG_NAME_LENGTH];
    bool registered;
    uint32_t last_feed_ms;
    uint32_t feed_count;
    uint32_t stack_high_water_words;
} task_watchdog_snapshot_t;

/* Register the calling FreeRTOS task with the ESP task watchdog. */
void task_watchdog_register(const char *task_name);

/* Register only in the health registry; do not subscribe the task to ESP TWDT. */
void task_watchdog_register_health_only(const char *task_name);

/* Feed the real ESP task watchdog and update the task-health heartbeat. */
void task_watchdog_feed(void);

/* Update only the health registry for tasks intentionally outside ESP TWDT. */
void task_watchdog_health_feed(void);

/* Start the health supervisor once after the task watchdog is configured. */
bool task_watchdog_start_supervisor(void);

/* Copy registered-task health data for diagnostics. */
size_t task_watchdog_get_snapshot(task_watchdog_snapshot_t *out,
                                  size_t capacity);

/* True when every registered task has fed within the health window. */
bool task_watchdog_all_healthy(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* TASK_WATCHDOG_H */
