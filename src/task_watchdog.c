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
static uint32_t s_generation_counter;

bool task_watchdog_init(bool enable_task_wdt, bool panic_on_hang)
{
    if (!enable_task_wdt) {
        return true;
    }

    esp_task_wdt_config_t config = {
        .timeout_ms = 15000U,
        .idle_core_mask = (1U << portNUM_PROCESSORS) - 1U,
        .trigger_panic = panic_on_hang,
    };
    /* Platform startup may auto-initialize TWDT from sdkconfig. Probe first
     * so normal auto-init does not emit a misleading esp_task_wdt_init()
     * error before we apply the application policy. */
    const esp_err_t status = esp_task_wdt_status(NULL);
    esp_err_t err = status == ESP_ERR_INVALID_STATE
                        ? esp_task_wdt_init(&config)
                        : esp_task_wdt_reconfigure(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure task watchdog: %s",
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static int find_record_locked(TaskHandle_t handle)
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
    const UBaseType_t stack_words = uxTaskGetStackHighWaterMark(current);
    bool registered = false;

    taskENTER_CRITICAL(&s_lock);
    int index = find_record_locked(current);
    if (index >= 0) {
        const task_watchdog_mode_t existing_mode = s_records[index].snapshot.mode;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "Duplicate/conflicting watchdog registration rejected for %s (mode=%d)",
                 task_name ? task_name : "task", (int)existing_mode);
        return false;
    }
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
    if (index < 0) {
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGE(TAG, "Task watchdog registry full; task health unavailable");
        return false;
    }

    task_watchdog_snapshot_t *snapshot = &s_records[index].snapshot;
    snapshot->registered = true;
    snapshot->health_registered = true;
    snapshot->twdt_subscribed = twdt_subscribed;
    snapshot->mode = twdt_subscribed ? TASK_WATCHDOG_MODE_TWDT_AND_HEALTH
                                     : TASK_WATCHDOG_MODE_HEALTH_ONLY;
    if (++s_generation_counter == 0U) {
        ++s_generation_counter;
    }
    snapshot->generation = s_generation_counter;
    snapshot->last_feed_ms = timestamp;
    snapshot->feed_count = 0U;
    snapshot->stack_high_water_words = stack_words;
    if (task_name && task_name[0] != '\0') {
        strncpy(snapshot->name, task_name, sizeof(snapshot->name) - 1U);
        snapshot->name[sizeof(snapshot->name) - 1U] = '\0';
    }
    registered = true;
    taskEXIT_CRITICAL(&s_lock);

    if (!registered) {
        ESP_LOGE(TAG, "Task watchdog registry full; task health unavailable");
    }
    return registered;
}

static bool verify_and_subscribe_current(const char *task_name,
                                         bool *new_subscription)
{
    *new_subscription = false;
    const esp_err_t status_before = esp_task_wdt_status(NULL);
    if (status_before == ESP_OK) {
        return true;
    }
    if (status_before != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "TWDT status unavailable for %s: %s",
                 task_name ? task_name : "task", esp_err_to_name(status_before));
        return false;
    }

    const esp_err_t add_err = esp_task_wdt_add(NULL);
    if (add_err != ESP_OK) {
        const esp_err_t status_after_error = esp_task_wdt_status(NULL);
        ESP_LOGE(TAG, "Could not subscribe %s to TWDT: add=%s, status=%s",
                 task_name ? task_name : "task", esp_err_to_name(add_err),
                 esp_err_to_name(status_after_error));
        return false;
    }

    const esp_err_t status_after = esp_task_wdt_status(NULL);
    if (status_after != ESP_OK) {
        ESP_LOGE(TAG, "TWDT subscription verification failed for %s: %s",
                 task_name ? task_name : "task", esp_err_to_name(status_after));
        const esp_err_t rollback = esp_task_wdt_delete(NULL);
        if (rollback != ESP_OK && rollback != ESP_ERR_NOT_FOUND &&
            rollback != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "TWDT rollback delete failed for %s: %s",
                     task_name ? task_name : "task", esp_err_to_name(rollback));
        }
        return false;
    }
    *new_subscription = true;
    return true;
}

bool task_watchdog_register(const char *task_name)
{
    bool new_subscription = false;
    if (!verify_and_subscribe_current(task_name, &new_subscription)) {
        return false;
    }

    if (!record_health_registration(task_name, true)) {
        if (new_subscription) {
            const esp_err_t rollback = esp_task_wdt_delete(NULL);
            if (rollback != ESP_OK && rollback != ESP_ERR_NOT_FOUND &&
                rollback != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "TWDT rollback failed after registry failure: %s",
                         esp_err_to_name(rollback));
            }
        }
        return false;
    }
    return true;
}

bool task_watchdog_register_health_only(const char *task_name)
{
    return record_health_registration(task_name, false);
}

uint32_t task_watchdog_current_generation(void)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    uint32_t generation = 0U;
    taskENTER_CRITICAL(&s_lock);
    const int index = find_record_locked(current);
    if (index >= 0) {
        generation = s_records[index].snapshot.generation;
    }
    taskEXIT_CRITICAL(&s_lock);
    return generation;
}

static bool update_health_heartbeat(TaskHandle_t current)
{
    const uint32_t timestamp = now_ms();
    const UBaseType_t stack_words = uxTaskGetStackHighWaterMark(current);
    bool updated = false;
    taskENTER_CRITICAL(&s_lock);
    const int index = find_record_locked(current);
    if (index >= 0 && s_records[index].snapshot.health_registered) {
        s_records[index].snapshot.last_feed_ms = timestamp;
        s_records[index].snapshot.feed_count++;
        s_records[index].snapshot.stack_high_water_words = stack_words;
        updated = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    return updated;
}

bool task_watchdog_unregister_task_generation(TaskHandle_t task_handle,
                                              uint32_t generation)
{
    if (task_handle == NULL || generation == 0U) {
        return false;
    }

    bool twdt_subscribed = false;
    taskENTER_CRITICAL(&s_lock);
    const int index = find_record_locked(task_handle);
    if (index >= 0 && s_records[index].snapshot.generation == generation) {
        twdt_subscribed = s_records[index].snapshot.twdt_subscribed;
    } else {
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (twdt_subscribed) {
        const esp_err_t err = esp_task_wdt_delete(task_handle);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE &&
            err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Task watchdog unregister failed for %p: %s",
                     task_handle, esp_err_to_name(err));
            return false;
        }
    }

    bool removed = false;
    taskENTER_CRITICAL(&s_lock);
    const int current_index = find_record_locked(task_handle);
    if (current_index >= 0 &&
        s_records[current_index].snapshot.generation == generation) {
        memset(&s_records[current_index], 0, sizeof(s_records[current_index]));
        removed = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    return removed;
}

void task_watchdog_unregister_task(TaskHandle_t task_handle)
{
    if (task_handle == NULL) {
        return;
    }
    uint32_t generation = 0U;
    taskENTER_CRITICAL(&s_lock);
    const int index = find_record_locked(task_handle);
    if (index >= 0) {
        generation = s_records[index].snapshot.generation;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (generation != 0U) {
        if (!task_watchdog_unregister_task_generation(task_handle, generation)) {
            ESP_LOGW(TAG, "Task watchdog generation cleanup rejected for %p",
                     task_handle);
        }
    }
}

void task_watchdog_unregister(void)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    const uint32_t generation = task_watchdog_current_generation();
    if (generation != 0U) {
        if (!task_watchdog_unregister_task_generation(current, generation)) {
            ESP_LOGW(TAG, "Current task watchdog cleanup rejected for %p", current);
        }
    }
}

bool task_watchdog_feed(void)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    bool twdt_subscribed = false;
    bool should_log_error = false;
    taskENTER_CRITICAL(&s_lock);
    const int index = find_record_locked(current);
    if (index >= 0) {
        twdt_subscribed = s_records[index].snapshot.twdt_subscribed;
    }
    if (!twdt_subscribed && s_last_feed_error_task != current) {
        s_last_feed_error_task = current;
        should_log_error = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (!twdt_subscribed) {
        if (should_log_error) {
            ESP_LOGE(TAG, "TWDT feed rejected for unsubscribed task %p", current);
        }
        return false;
    }

    const esp_err_t err = esp_task_wdt_reset();
    if (err != ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        should_log_error = s_last_feed_error_task != current;
        if (should_log_error) {
            s_last_feed_error_task = current;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (should_log_error) {
            ESP_LOGE(TAG, "TWDT feed failed for task %p: %s",
                     current, esp_err_to_name(err));
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
    if (!task_watchdog_register_health_only("watchdog_supervisor")) {
        ESP_LOGE(TAG, "Watchdog supervisor health registration failed");
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        if (!task_watchdog_health_feed()) {
            ESP_LOGE(TAG, "Watchdog supervisor heartbeat update failed");
            vTaskDelete(NULL);
            return;
        }
        const uint32_t timestamp = now_ms();
        const TaskHandle_t supervisor = xTaskGetCurrentTaskHandle();
        static task_watchdog_snapshot_t stale[TASK_WATCHDOG_MAX_TASKS];
        static task_watchdog_snapshot_t low_stack[TASK_WATCHDOG_MAX_TASKS];
        size_t stale_count = 0U;
        size_t low_stack_count = 0U;
        taskENTER_CRITICAL(&s_lock);
        for (size_t i = 0U; i < TASK_WATCHDOG_MAX_TASKS; ++i) {
            const task_watchdog_snapshot_t *snapshot = &s_records[i].snapshot;
            if (!snapshot->registered || !snapshot->health_registered ||
                s_records[i].handle == supervisor) {
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
            ESP_LOGE(TAG, "Task heartbeat stale: %s (%lums, expected=%ums, stack=%lu)",
                     stale[i].name,
                     (unsigned long)(timestamp - stale[i].last_feed_ms),
                     (unsigned)TASK_WATCHDOG_STALE_MS,
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
        if (snapshot->registered && snapshot->health_registered &&
            (uint32_t)(timestamp - snapshot->last_feed_ms) >
                TASK_WATCHDOG_STALE_MS) {
            healthy = false;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return healthy;
}
