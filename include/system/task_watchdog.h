#ifndef TASK_WATCHDOG_H
#define TASK_WATCHDOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TASK_WATCHDOG_MAX_TASKS 32U
#define TASK_WATCHDOG_STALE_MS 10000U
#define TASK_WATCHDOG_NAME_LENGTH 24U

typedef enum {
    TASK_WATCHDOG_MODE_NONE = 0,
    TASK_WATCHDOG_MODE_TWDT,
    TASK_WATCHDOG_MODE_HEALTH_ONLY,
    TASK_WATCHDOG_MODE_TWDT_AND_HEALTH,
} task_watchdog_mode_t;

typedef struct {
    char name[TASK_WATCHDOG_NAME_LENGTH];
    bool registered;
    bool twdt_subscribed;
    bool health_registered;
    task_watchdog_mode_t mode;
    uint32_t generation;
    uint32_t last_feed_ms;
    uint32_t feed_count;
    uint32_t stack_high_water_words;
} task_watchdog_snapshot_t;

/* Register the calling FreeRTOS task with the ESP task watchdog. */
bool task_watchdog_init(bool enable_task_wdt, bool panic_on_hang);
bool task_watchdog_register(const char *task_name);

/* Register only in the health registry; do not subscribe the task to ESP TWDT. */
bool task_watchdog_register_health_only(const char *task_name);

/* Return the current task's lifecycle generation, or zero if unregistered. */
uint32_t task_watchdog_current_generation(void);

/* Remove a record only when both handle and lifecycle generation match. */
bool task_watchdog_unregister_task_generation(TaskHandle_t task_handle,
                                              uint32_t generation);

/* Unregister a task before it is externally deleted or permanently stopped. */
void task_watchdog_unregister_task(TaskHandle_t task_handle);

/* Unregister the calling task before it self-deletes or is permanently stopped. */
void task_watchdog_unregister(void);

/* Feed the real ESP task watchdog and update the task-health heartbeat. */
bool task_watchdog_feed(void);

/* Update only the health registry for tasks intentionally outside ESP TWDT. */
bool task_watchdog_health_feed(void);

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
