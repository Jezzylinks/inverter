#include "system/task_watchdog.h"

#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd/lcd_watchdog.h"

static const char *TAG = "TASK_WDT";

#define TASK_WATCHDOG_SUPERVISOR_STACK 3072U
#define TASK_WATCHDOG_SUPERVISOR_PRIORITY 6U
#define TASK_WATCHDOG_SUPERVISOR_PERIOD_MS 5000U

typedef struct {
    TaskHandle_t handle;
    task_watchdog_snapshot_t snapshot;
} task_record_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static task_record_t s_records[TASK_WATCHDOG_MAX_TASKS];
static TaskHandle_t s_supervisor_task;
static TaskHandle_t s_last_feed_error_task;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static int find_record(TaskHandle_t handle)
{
    for (size_t i = 0U; i < TASK_WATCHDOG_MAX_TASKS; ++i) {
        if (s_records[i].snapshot.registered && s_records[i].handle == handle) {
            return (int)i;
        }
    }
    return -1;
}

static bool record_health_registration(const char *task_name, bool twdt_subscribed)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    const uint32_t timestamp = now_ms();
    taskENTER_CRITICAL(&s_lock);
    int index = find_record(current);
    if (index < 0) {
        for (size_t i = 0U; i < TASK_WATCHDOG_MAX_TASKS; ++i) {
            if (!s_records[i].snapshot.registered) {
                index = (int)i;
                memset(&s_records[i], 0, sizeof(s_records[i]));
                s_records[i].handle = current;
                break;
            }
        }
    }
    if (index >= 0) {
        task_watchdog_snapshot_t *snapshot = &s_records[index].snapshot;
        snapshot->registered = true;
        snapshot->health_registered = true;
        snapshot->twdt_subscribed = twdt_subscribed;
        snapshot->mode = twdt_subscribed ? TASK_WATCHDOG_MODE_TWDT_AND_HEALTH
                                         : TASK_WATCHDOG_MODE_HEALTH_ONLY;
        snapshot->last_feed_ms = timestamp;
        if (task_name && task_name[0] != '\0') {
            strncpy(snapshot->name, task_name, sizeof(snapshot->name) - 1U);
            snapshot->name[sizeof(snapshot->name) - 1U] = '\0';
        }
        snapshot->stack_high_water_words = uxTaskGetStackHighWaterMark(current);
    }
    taskEXIT_CRITICAL(&s_lock);

    if (index < 0) {
        ESP_LOGE(TAG, "Task watchdog registry full; task health unavailable");
        return false;
    }
    return true;
}

bool task_watchdog_register(const char *task_name)
{
    const esp_err_t status = esp_task_wdt_status(NULL);
    bool twdt_subscribed = status == ESP_OK;
    if (status == ESP_ERR_NOT_FOUND) {
        const esp_err_t err = esp_task_wdt_add(NULL);
        if (err == ESP_OK) {
            twdt_subscribed = true;
        } else {
            ESP_LOGE(TAG, "Could not subscribe %s to TWDT: %s",
                     task_name ? task_name : "task", esp_err_to_name(err));
        }
    } else if (status != ESP_OK) {
        ESP_LOGE(TAG, "TWDT status unavailable for %s: %s",
                 task_name ? task_name : "task", esp_err_to_name(status));
    }
    const bool health_registered =
        record_health_registration(task_name, twdt_subscribed);
    return twdt_subscribed && health_registered;
}

bool task_watchdog_register_health_only(const char *task_name)
{
    return record_health_registration(task_name, false);
}

static bool update_health_heartbeat(TaskHandle_t current)
{
    const uint32_t timestamp = now_ms();
    taskENTER_CRITICAL(&s_lock);
    const int index = find_record(current);
    if (index >= 0) {
        s_records[index].snapshot.last_feed_ms = timestamp;
        s_records[index].snapshot.feed_count++;
        s_records[index].snapshot.stack_high_water_words =
            uxTaskGetStackHighWaterMark(current);
    }
    taskEXIT_CRITICAL(&s_lock);
    return index >= 0;
}

void task_watchdog_unregister_task(TaskHandle_t task_handle)
{
    if (task_handle == NULL) {
        return;
    }

    bool twdt_subscribed = false;
    taskENTER_CRITICAL(&s_lock);
    const int existing = find_record(task_handle);
    if (existing >= 0) {
        twdt_subscribed = s_records[existing].snapshot.twdt_subscribed;
    }
    taskEXIT_CRITICAL(&s_lock);

    /* A task may be subscribed before its health record is created (fatal
     * startup cleanup), so verify the real TWDT state in that case. */
    if (existing < 0) {
        twdt_subscribed = esp_task_wdt_status(task_handle) == ESP_OK;
    }
    if (twdt_subscribed) {
        const esp_err_t err = esp_task_wdt_delete(task_handle);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE &&
            err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Task watchdog unregister failed for %p: %s",
                     task_handle, esp_err_to_name(err));
        }
    }

    taskENTER_CRITICAL(&s_lock);
    const int index = find_record(task_handle);
    if (index >= 0) {
        memset(&s_records[index], 0, sizeof(s_records[index]));
    }
    taskEXIT_CRITICAL(&s_lock);
}

void task_watchdog_unregister(void)
{
    task_watchdog_unregister_task(xTaskGetCurrentTaskHandle());
}

bool task_watchdog_feed(void)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    bool twdt_subscribed = false;
    taskENTER_CRITICAL(&s_lock);
    const int index = find_record(current);
    if (index >= 0) {
        twdt_subscribed = s_records[index].snapshot.twdt_subscribed;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (!twdt_subscribed) {
        if (s_last_feed_error_task != current) {
            ESP_LOGE(TAG, "TWDT feed rejected for unsubscribed task %p", current);
            s_last_feed_error_task = current;
        }
        return false;
    }

    const esp_err_t err = esp_task_wdt_reset();
    if (err != ESP_OK) {
        if (s_last_feed_error_task != current) {
            ESP_LOGE(TAG, "TWDT feed failed for task %p: %s",
                     current, esp_err_to_name(err));
            s_last_feed_error_task = current;
        }
        return false;
    }
    return update_health_heartbeat(current);
}

bool task_watchdog_health_feed(void)
{
    return update_health_heartbeat(xTaskGetCurrentTaskHandle());
}

static void task_watchdog_supervisor(void *arg)
{
    (void)arg;
    task_watchdog_register_health_only("watchdog_supervisor");
    while (true) {
        task_watchdog_health_feed();
        const uint32_t timestamp = now_ms();
        const TaskHandle_t supervisor = xTaskGetCurrentTaskHandle();
        static task_watchdog_snapshot_t stale[TASK_WATCHDOG_MAX_TASKS];
        static task_watchdog_snapshot_t low_stack[TASK_WATCHDOG_MAX_TASKS];
        size_t stale_count = 0U;
        size_t low_stack_count = 0U;
        taskENTER_CRITICAL(&s_lock);
        for (size_t i = 0U; i < TASK_WATCHDOG_MAX_TASKS; ++i) {
            const task_watchdog_snapshot_t *snapshot = &s_records[i].snapshot;
            if (!snapshot->registered || s_records[i].handle == supervisor) {
                continue;
            }
            if ((uint32_t)(timestamp - snapshot->last_feed_ms) >
                    TASK_WATCHDOG_STALE_MS &&
                stale_count < TASK_WATCHDOG_MAX_TASKS) {
                stale[stale_count++] = *snapshot;
            }
            if (snapshot->stack_high_water_words < 128U &&
                low_stack_count < TASK_WATCHDOG_MAX_TASKS) {
                low_stack[low_stack_count++] = *snapshot;
            }
        }
        taskEXIT_CRITICAL(&s_lock);

        for (size_t i = 0U; i < stale_count; ++i) {
            ESP_LOGE(TAG, "Task heartbeat stale: %s (%lums, stack=%lu)",
                     stale[i].name,
                     (unsigned long)(timestamp - stale[i].last_feed_ms),
                     (unsigned long)stale[i].stack_high_water_words);
        }
        for (size_t i = 0U; i < low_stack_count; ++i) {
            ESP_LOGW(TAG, "Task stack margin low: %s (%lu words)",
                     low_stack[i].name,
                     (unsigned long)low_stack[i].stack_high_water_words);
        }
        (void)lcd_watchdog_check();
        vTaskDelay(pdMS_TO_TICKS(TASK_WATCHDOG_SUPERVISOR_PERIOD_MS));
    }
}

bool task_watchdog_start_supervisor(void)
{
    if (s_supervisor_task) {
        return true;
    }
    return xTaskCreate(task_watchdog_supervisor,
                       "watchdog_supervisor",
                       TASK_WATCHDOG_SUPERVISOR_STACK,
                       NULL,
                       TASK_WATCHDOG_SUPERVISOR_PRIORITY,
                       &s_supervisor_task) == pdPASS;
}

size_t task_watchdog_get_snapshot(task_watchdog_snapshot_t *out,
                                  size_t capacity)
{
    if (!out || capacity == 0U) {
        return 0U;
    }
    size_t copied = 0U;
    taskENTER_CRITICAL(&s_lock);
    for (size_t i = 0U; i < TASK_WATCHDOG_MAX_TASKS && copied < capacity; ++i) {
        if (s_records[i].snapshot.registered) {
            out[copied++] = s_records[i].snapshot;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return copied;
}

bool task_watchdog_all_healthy(uint32_t timestamp)
{
    bool healthy = true;
    taskENTER_CRITICAL(&s_lock);
    for (size_t i = 0U; i < TASK_WATCHDOG_MAX_TASKS; ++i) {
        const task_watchdog_snapshot_t *snapshot = &s_records[i].snapshot;
        if (snapshot->registered &&
            (uint32_t)(timestamp - snapshot->last_feed_ms) >
                TASK_WATCHDOG_STALE_MS) {
            healthy = false;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return healthy;
}
