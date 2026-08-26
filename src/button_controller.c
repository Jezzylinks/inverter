#include "system/task_watchdog.h"
/**
 * @file button_controller.c
 * @brief Shared-task button controller implementation.
 */

#include "app/button_controller.h"

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/portmacro.h"
#include <string.h>

#define BUTTON_INVALID_LEVEL (-1)

typedef struct {
    gpio_num_t gpio_pin;
    int level;
    int64_t timestamp_us;
} button_edge_t;

typedef struct button_controller_t {
    button_config_t config;
    TaskHandle_t task_handle;
    _Atomic button_state_t current_state;
    _Atomic bool is_initialized;
    _Atomic bool is_running;
    _Atomic bool is_pressed;

    int last_raw_level;
    int stable_level;
    int64_t last_edge_us;
    int64_t press_start_us;
    int64_t last_release_us;
    uint32_t last_click_duration_ms;
    uint32_t repeat_count;
    uint8_t click_count;
    bool long_press_triggered;

    button_stats_t stats;
    button_raw_event_type_t last_event;
    int64_t last_event_timestamp_us;
    button_event_callback_t event_callback;
    button_error_callback_t error_callback;
    void *user_data;
    bool in_use;
} button_controller_t;

static button_controller_t g_button_controllers[BUTTON_MAX_CONTROLLERS];
static button_handle_t g_gpio_to_controller[GPIO_NUM_MAX];
static QueueHandle_t g_edge_queue;
static TaskHandle_t g_button_task;
static SemaphoreHandle_t g_controller_mutex;
static _Atomic bool g_system_initialized;

static const button_mapping_t g_button_mappings[] = {
    {GPIO_BUTTON_POWER, BTN_POWER, "Power"},
    {GPIO_BUTTON_ENTER_MENU, BTN_ENTER, "Enter/Menu"},
    {GPIO_BUTTON_UP, BTN_UP, "Up"},
    {GPIO_BUTTON_DOWN, BTN_DOWN, "Down"},
    {GPIO_BUTTON_BACK, BTN_BACK, "Back"},
};

static button_handle_t find_controller(gpio_num_t gpio_pin)
{
    if (gpio_pin < GPIO_NUM_0 || gpio_pin >= GPIO_NUM_MAX) {
        return NULL;
    }
    return g_gpio_to_controller[gpio_pin];
}

static bool level_is_pressed(const button_controller_t *button, int level)
{
    return button->config.active_low ? (level == 0) : (level != 0);
}

static uint32_t elapsed_ms(int64_t now_us, int64_t start_us)
{
    if (start_us <= 0 || now_us <= start_us) {
        return 0;
    }
    const int64_t delta_ms = (now_us - start_us) / 1000;
    return (delta_ms > UINT32_MAX) ? UINT32_MAX : (uint32_t)delta_ms;
}

static void emit_event(button_controller_t *button,
                       button_raw_event_type_t type,
                       int64_t timestamp_us,
                       uint32_t duration_ms,
                       uint8_t click_count)
{
    if (!button || !atomic_load(&button->is_running)) {
        return;
    }

    button_event_info_t event = {
        .event = type,
        .button_id = button->config.button_id,
        .click_count = click_count,
        .reserved = 0,
        .timestamp_us = timestamp_us,
        .press_duration_ms = duration_ms,
    };

    atomic_fetch_add(&button->stats.total_events, 1);
    button->last_event = type;
    button->last_event_timestamp_us = timestamp_us;
    if (type == BUTTON_EVENT_CLICK) {
        atomic_fetch_add(&button->stats.short_presses, 1);
    } else if (type == BUTTON_EVENT_DOUBLE_CLICK) {
        atomic_fetch_add(&button->stats.double_clicks, 1);
    } else if (type == BUTTON_EVENT_TRIPLE_CLICK) {
        atomic_fetch_add(&button->stats.triple_clicks, 1);
    } else if (type == BUTTON_EVENT_MULTI_CLICK) {
        atomic_fetch_add(&button->stats.multi_clicks, 1);
    } else if (type == BUTTON_EVENT_LONG_PRESS) {
        atomic_fetch_add(&button->stats.long_presses, 1);
    } else if (type == BUTTON_EVENT_REPEAT) {
        atomic_fetch_add(&button->stats.hold_repeats, 1);
    }

#if defined(CONFIG_INVERTER_BUTTON_DIAGNOSTICS) && CONFIG_INVERTER_BUTTON_DIAGNOSTICS
    ESP_LOGI(BUTTON_TAG, "EVENT %s button=%s gpio=%d clicks=%u duration_ms=%lu",
             button_event_to_string(type),
             button->config.controller_name,
             button->config.gpio_pin,
             (unsigned)click_count,
             (unsigned long)duration_ms);
#endif
    if (button->event_callback) {
        button->event_callback(&event, button->user_data);
    }
}

static void finalize_clicks(button_controller_t *button, int64_t now_us)
{
    if (!button || button->click_count == 0 || button->last_release_us <= 0) {
        return;
    }

    if (elapsed_ms(now_us, button->last_release_us) < button->config.double_click_ms) {
        return;
    }

    button_raw_event_type_t type;
    switch (button->click_count) {
    case 1:
        type = BUTTON_EVENT_CLICK;
        break;
    case 2:
        type = BUTTON_EVENT_DOUBLE_CLICK;
        break;
    case 3:
        type = BUTTON_EVENT_TRIPLE_CLICK;
        break;
    default:
        type = BUTTON_EVENT_MULTI_CLICK;
        break;
    }

    if (button->config.button_id == BTN_ENTER && type == BUTTON_EVENT_CLICK) {
        ESP_LOGI("BUTTON", "ENTER click generated after %ums release window",
                 elapsed_ms(now_us, button->last_release_us));
    }
    emit_event(button, type, now_us, button->last_click_duration_ms, button->click_count);
    button->click_count = 0;
    atomic_store(&button->current_state, BUTTON_STATE_IDLE);
}

static void process_button_deadlines(button_controller_t *button, int64_t now_us)
{
    if (!button || !atomic_load(&button->is_running)) {
        return;
    }

    if (atomic_load(&button->is_pressed)) {
        const uint32_t held_ms = elapsed_ms(now_us, button->press_start_us);
        if (!button->long_press_triggered && held_ms >= button->config.long_press_ms) {
            button->long_press_triggered = true;
            button->click_count = 0;
            atomic_store(&button->current_state, BUTTON_STATE_LONG_PRESS_ACTIVE);
            emit_event(button, BUTTON_EVENT_LONG_PRESS, now_us, held_ms, 0);
        }

        if (button->long_press_triggered && button->config.hold_repeat_ms > 0) {
            static const uint32_t REPEAT_ACCELERATION_START_MS = 3000U;
            uint32_t repeat_ms = button->config.hold_repeat_ms;
            if ((button->config.button_id == BTN_UP || button->config.button_id == BTN_DOWN) &&
                held_ms >= REPEAT_ACCELERATION_START_MS) {
                repeat_ms = (repeat_ms > 50U) ? (repeat_ms / 2U) : 50U;
            }

            const uint32_t since_press_ms = elapsed_ms(now_us, button->press_start_us);
            const uint32_t first_repeat_ms = button->config.long_press_ms + repeat_ms;
            if (since_press_ms >= first_repeat_ms) {
                const uint32_t repeats_elapsed =
                    (since_press_ms - first_repeat_ms) / repeat_ms;
                if (repeats_elapsed >= button->repeat_count) {
                    button->repeat_count = repeats_elapsed + 1U;
                    atomic_store(&button->current_state, BUTTON_STATE_REPEAT_ACTIVE);
                    emit_event(button, BUTTON_EVENT_REPEAT, now_us, held_ms, 0);
                }
            }
        }
    } else {
        finalize_clicks(button, now_us);
    }
}

static void process_stable_level(button_controller_t *button, int level, int64_t now_us)
{
    if (!button || !atomic_load(&button->is_running)) {
        return;
    }

    const bool pressed = level_is_pressed(button, level);
    const bool was_pressed = atomic_load(&button->is_pressed);
    if (pressed == was_pressed) {
        return;
    }

    atomic_store(&button->is_pressed, pressed);
    button->stable_level = level;

    if (pressed) {
        if (button->last_release_us > 0 &&
            elapsed_ms(now_us, button->last_release_us) > button->config.double_click_ms) {
            button->click_count = 0;
        }
        button->press_start_us = now_us;
        button->repeat_count = 0;
        button->long_press_triggered = false;
        atomic_store(&button->current_state, BUTTON_STATE_PRESSED);
        atomic_fetch_add(&button->stats.total_presses, 1);
        emit_event(button, BUTTON_EVENT_PRESS, now_us, 0, button->click_count);
    } else {
        const uint32_t duration_ms = elapsed_ms(now_us, button->press_start_us);
        button->last_release_us = now_us;
        button->last_click_duration_ms = duration_ms;

        if (button->long_press_triggered) {
            button->click_count = 0;
            atomic_store(&button->current_state, BUTTON_STATE_IDLE);
        } else {
            button->click_count = (button->click_count < BUTTON_MAX_CLICK_COUNT)
                                      ? (uint8_t)(button->click_count + 1U)
                                      : BUTTON_MAX_CLICK_COUNT;
            if (button->config.enable_multi_click) {
                atomic_store(&button->current_state, BUTTON_STATE_RELEASED);
            } else {
                emit_event(button, BUTTON_EVENT_CLICK, now_us, duration_ms, 1);
                button->click_count = 0;
                atomic_store(&button->current_state, BUTTON_STATE_IDLE);
            }
        }

        emit_event(button, BUTTON_EVENT_RELEASE, now_us, duration_ms, button->click_count);
    }
}

static void IRAM_ATTR button_gpio_isr_handler(void *arg)
{
    const gpio_num_t gpio_pin = (gpio_num_t)(uintptr_t)arg;
    button_handle_t button = find_controller(gpio_pin);
    if (!button || !g_edge_queue) {
        return;
    }

    BaseType_t higher_priority_task_woken = pdFALSE;
    button_edge_t edge = {
        .gpio_pin = gpio_pin,
        .level = gpio_get_level(gpio_pin),
        .timestamp_us = esp_timer_get_time(),
    };
    atomic_fetch_add(&button->stats.isr_calls, 1);
    if (xQueueSendFromISR(g_edge_queue, &edge, &higher_priority_task_woken) != pdPASS) {
        atomic_fetch_add(&button->stats.isr_queue_full, 1);
    }
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void button_task(void *arg)
{
    task_watchdog_register("button_task");
    (void)arg;
    button_edge_t edge;

    for (;;) {
        task_watchdog_feed();
        if (xQueueReceive(g_edge_queue, &edge, pdMS_TO_TICKS(BUTTON_TASK_POLL_INTERVAL_MS)) == pdPASS) {
            button_handle_t button = find_controller(edge.gpio_pin);
            if (button && atomic_load(&button->is_running)) {
                if (button->last_raw_level != edge.level) {
                    button->last_raw_level = edge.level;
                    button->last_edge_us = edge.timestamp_us;
                }
            }
        }

        const int64_t now_us = esp_timer_get_time();
#if defined(CONFIG_INVERTER_BUTTON_DIAGNOSTICS) && CONFIG_INVERTER_BUTTON_DIAGNOSTICS
        static int64_t next_diagnostic_us;
        if (now_us >= next_diagnostic_us) {
            next_diagnostic_us = now_us + 1000000LL;
            for (size_t diagnostic_index = 0U;
                 diagnostic_index < BUTTON_MAX_CONTROLLERS;
                 ++diagnostic_index) {
                button_handle_t diagnostic_button =
                    &g_button_controllers[diagnostic_index];
                if (!diagnostic_button->in_use) {
                    continue;
                }
                ESP_LOGI(BUTTON_TAG,
                         "DIAG %s gpio=%d raw=%d stable=%d pressed=%d state=%s isr=%lu queue_full=%lu events=%lu last=%s last_us=%lld",
                         diagnostic_button->config.controller_name,
                         diagnostic_button->config.gpio_pin,
                         diagnostic_button->last_raw_level,
                         diagnostic_button->stable_level,
                         atomic_load(&diagnostic_button->is_pressed),
                         button_state_to_string(atomic_load(&diagnostic_button->current_state)),
                         (unsigned long)atomic_load(&diagnostic_button->stats.isr_calls),
                         (unsigned long)atomic_load(&diagnostic_button->stats.isr_queue_full),
                         (unsigned long)atomic_load(&diagnostic_button->stats.total_events),
                         button_event_to_string(diagnostic_button->last_event),
                         (long long)diagnostic_button->last_event_timestamp_us);
            }
        }
#endif
        for (size_t i = 0; i < BUTTON_MAX_CONTROLLERS; ++i) {
            button_handle_t button = &g_button_controllers[i];
            if (!button->in_use || !atomic_load(&button->is_running)) {
                continue;
            }

            /*
             * Reconcile the GPIO level even if an edge interrupt was lost while
             * the ISR queue was full or briefly unavailable. Without this,
             * is_pressed can remain true after a physical release and continue
             * emitting hold-repeat events indefinitely.
             */
            const int sampled_level = gpio_get_level(button->config.gpio_pin);
            if (sampled_level != button->last_raw_level) {
                button->last_raw_level = sampled_level;
                button->last_edge_us = now_us;
            }

            if (button->last_raw_level != button->stable_level &&
                elapsed_ms(now_us, button->last_edge_us) >= button->config.debounce_ms) {
                process_stable_level(button, button->last_raw_level, now_us);
            }
            process_button_deadlines(button, now_us);
        }
    }
}

button_id_t gpio_to_button_id(gpio_num_t gpio_pin)
{
    for (size_t i = 0; i < sizeof(g_button_mappings) / sizeof(g_button_mappings[0]); ++i) {
        if (g_button_mappings[i].gpio_pin == gpio_pin) {
            return g_button_mappings[i].button_id;
        }
    }
    return BTN_COUNT;
}

const char *button_event_to_string(button_raw_event_type_t event)
{
    static const char *const names[] = {
        "NONE", "PRESS", "CLICK", "RELEASE", "REPEAT",
        "DOUBLE_CLICK", "TRIPLE_CLICK", "MULTI_CLICK", "LONG_PRESS"};
    return (event < BUTTON_EVENT_MAX) ? names[event] : "UNKNOWN";
}

const char *button_state_to_string(button_state_t state)
{
    static const char *const names[] = {
        "IDLE", "PRESSED", "RELEASED", "LONG_PRESS_ACTIVE", "REPEAT_ACTIVE"};
    return (state < BUTTON_STATE_MAX) ? names[state] : "UNKNOWN";
}

void button_controller_get_default_config(button_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->gpio_pin = GPIO_NUM_0;
    config->button_id = BTN_COUNT;
    config->debounce_ms = BUTTON_DEFAULT_DEBOUNCE_MS;
    config->long_press_ms = BUTTON_DEFAULT_LONG_PRESS_MS;
    config->double_click_ms = BUTTON_DEFAULT_DOUBLE_CLICK_MS;
    config->hold_repeat_ms = BUTTON_DEFAULT_HOLD_REPEAT_MS;
    config->active_low = true;
    config->enable_pullup = true;
    config->controller_name = "button";
    config->enable_multi_click = true;
}

esp_err_t button_controller_validate_config(const button_config_t *config)
{
    if (!config || config->gpio_pin < GPIO_NUM_0 || config->gpio_pin >= GPIO_NUM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (gpio_to_button_id(config->gpio_pin) == BTN_COUNT) {
        ESP_LOGE(BUTTON_TAG, "GPIO %d is not mapped to a supported button", config->gpio_pin);
        return ESP_ERR_INVALID_ARG;
    }
    if (config->debounce_ms == 0 || config->debounce_ms > 1000U ||
        config->long_press_ms == 0 || config->long_press_ms > 30000U ||
        config->double_click_ms == 0 || config->double_click_ms > 5000U ||
        config->hold_repeat_ms > 5000U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->enable_pullup && config->active_low == false) {
        ESP_LOGW(BUTTON_TAG, "GPIO %d uses pull-up with active-high input", config->gpio_pin);
    }
    return ESP_OK;
}

static button_handle_t allocate_controller(void)
{
    for (size_t i = 0; i < BUTTON_MAX_CONTROLLERS; ++i) {
        if (!g_button_controllers[i].in_use) {
            g_button_controllers[i].in_use = true;
            return &g_button_controllers[i];
        }
    }
    return NULL;
}

static void reset_controller(button_handle_t button)
{
    if (!button) {
        return;
    }
    button->last_raw_level = BUTTON_INVALID_LEVEL;
    button->stable_level = BUTTON_INVALID_LEVEL;
    button->last_edge_us = 0;
    button->press_start_us = 0;
    button->last_release_us = 0;
    button->last_click_duration_ms = 0;
    button->last_event = BUTTON_EVENT_NONE;
    button->last_event_timestamp_us = 0;
    button->repeat_count = 0;
    button->click_count = 0;
    button->long_press_triggered = false;
    atomic_store(&button->current_state, BUTTON_STATE_IDLE);
    atomic_store(&button->is_pressed, false);
    atomic_store(&button->is_running, false);
    atomic_store(&button->is_initialized, false);
}

esp_err_t button_controller_init(void)
{
    if (atomic_load(&g_system_initialized)) {
        return ESP_OK;
    }

    memset(g_button_controllers, 0, sizeof(g_button_controllers));
    memset(g_gpio_to_controller, 0, sizeof(g_gpio_to_controller));
    g_controller_mutex = xSemaphoreCreateMutex();
    g_edge_queue = xQueueCreate(BUTTON_EDGE_QUEUE_SIZE, sizeof(button_edge_t));
    if (!g_controller_mutex || !g_edge_queue) {
        if (g_controller_mutex) {
            vSemaphoreDelete(g_controller_mutex);
            g_controller_mutex = NULL;
        }
        if (g_edge_queue) {
            vQueueDelete(g_edge_queue);
            g_edge_queue = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        vQueueDelete(g_edge_queue);
        g_edge_queue = NULL;
        vSemaphoreDelete(g_controller_mutex);
        g_controller_mutex = NULL;
        return ret;
    }

    if (xTaskCreate(button_task, "button_task", BUTTON_TASK_STACK_SIZE, NULL,
                    BUTTON_TASK_PRIORITY, &g_button_task) != pdPASS) {
        gpio_uninstall_isr_service();
        vQueueDelete(g_edge_queue);
        g_edge_queue = NULL;
        vSemaphoreDelete(g_controller_mutex);
        g_controller_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    atomic_store(&g_system_initialized, true);
    ESP_LOGI(BUTTON_TAG, "Shared button task initialized");
    return ESP_OK;
}

esp_err_t button_controller_deinit(void)
{
    if (!atomic_load(&g_system_initialized)) {
        return ESP_OK;
    }

    for (size_t i = 0; i < BUTTON_MAX_CONTROLLERS; ++i) {
        if (g_button_controllers[i].in_use) {
            button_controller_destroy(&g_button_controllers[i]);
        }
    }

    if (g_button_task) {
        TaskHandle_t task = g_button_task;
        g_button_task = NULL;
        task_watchdog_unregister_task(task);
        vTaskDelete(task);
    }
    if (g_edge_queue) {
        vQueueDelete(g_edge_queue);
        g_edge_queue = NULL;
    }
    gpio_uninstall_isr_service();
    if (g_controller_mutex) {
        vSemaphoreDelete(g_controller_mutex);
        g_controller_mutex = NULL;
    }
    atomic_store(&g_system_initialized, false);
    return ESP_OK;
}

esp_err_t button_controller_create(const button_config_t *config, button_handle_t *handle)
{
    if (!atomic_load(&g_system_initialized) || !config || !handle) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = button_controller_validate_config(config);
    if (ret != ESP_OK) {
        return ret;
    }
    if (xSemaphoreTake(g_controller_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (g_gpio_to_controller[config->gpio_pin]) {
        xSemaphoreGive(g_controller_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    button_handle_t button = allocate_controller();
    if (!button) {
        xSemaphoreGive(g_controller_mutex);
        return ESP_ERR_NO_MEM;
    }
    reset_controller(button);
    button->config = *config;
    if (button->config.button_id >= BTN_COUNT) {
        button->config.button_id = gpio_to_button_id(button->config.gpio_pin);
    }
    memset(&button->stats, 0, sizeof(button->stats));

    gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ULL << button->config.gpio_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = button->config.enable_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = button->config.enable_pullup ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ret = gpio_config(&gpio_cfg);
    if (ret != ESP_OK) {
        button->in_use = false;
        xSemaphoreGive(g_controller_mutex);
        return ret;
    }

    const int initial_level = gpio_get_level(button->config.gpio_pin);
    button->last_raw_level = initial_level;
    button->stable_level = initial_level;
    atomic_store(&button->is_initialized, true);
    g_gpio_to_controller[button->config.gpio_pin] = button;
    xSemaphoreGive(g_controller_mutex);
    *handle = button;
    return ESP_OK;
}

esp_err_t button_controller_destroy(button_handle_t handle)
{
    if (!handle || !handle->in_use) {
        return ESP_ERR_INVALID_ARG;
    }
    button_controller_stop(handle);
    if (xSemaphoreTake(g_controller_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    gpio_isr_handler_remove(handle->config.gpio_pin);
    if (handle->config.gpio_pin < GPIO_NUM_MAX &&
        g_gpio_to_controller[handle->config.gpio_pin] == handle) {
        g_gpio_to_controller[handle->config.gpio_pin] = NULL;
    }
    gpio_reset_pin(handle->config.gpio_pin);
    reset_controller(handle);
    handle->in_use = false;
    xSemaphoreGive(g_controller_mutex);
    return ESP_OK;
}

esp_err_t button_controller_start(button_handle_t handle)
{
    if (!handle || !atomic_load(&handle->is_initialized) || !g_button_task) {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load(&handle->is_running)) {
        return ESP_OK;
    }
    atomic_store(&handle->is_running, true);
    const esp_err_t ret = gpio_isr_handler_add(handle->config.gpio_pin,
                                                button_gpio_isr_handler,
                                                (void *)(uintptr_t)handle->config.gpio_pin);
    if (ret != ESP_OK) {
        atomic_store(&handle->is_running, false);
        return ret;
    }
    return ESP_OK;
}

esp_err_t button_controller_stop(button_handle_t handle)
{
    if (!handle || !atomic_load(&handle->is_initialized)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load(&handle->is_running)) {
        return ESP_OK;
    }
    gpio_isr_handler_remove(handle->config.gpio_pin);
    atomic_store(&handle->is_running, false);
    atomic_store(&handle->is_pressed, false);
    atomic_store(&handle->current_state, BUTTON_STATE_IDLE);
    handle->click_count = 0;
    return ESP_OK;
}

esp_err_t button_controller_register_event_callback(button_handle_t handle,
                                                     button_event_callback_t callback,
                                                     void *user_data)
{
    if (!handle || !atomic_load(&handle->is_initialized)) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->event_callback = callback;
    handle->user_data = user_data;
    return ESP_OK;
}

esp_err_t button_controller_register_error_callback(button_handle_t handle,
                                                     button_error_callback_t callback,
                                                     void *user_data)
{
    if (!handle || !atomic_load(&handle->is_initialized)) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->error_callback = callback;
    if (user_data) {
        handle->user_data = user_data;
    }
    return ESP_OK;
}

esp_err_t button_controller_get_stats(button_handle_t handle, button_stats_t *stats)
{
    if (!handle || !stats || !handle->in_use) {
        return ESP_ERR_INVALID_ARG;
    }
    stats->total_events = atomic_load(&handle->stats.total_events);
    stats->total_presses = atomic_load(&handle->stats.total_presses);
    stats->short_presses = atomic_load(&handle->stats.short_presses);
    stats->long_presses = atomic_load(&handle->stats.long_presses);
    stats->double_clicks = atomic_load(&handle->stats.double_clicks);
    stats->triple_clicks = atomic_load(&handle->stats.triple_clicks);
    stats->multi_clicks = atomic_load(&handle->stats.multi_clicks);
    stats->hold_repeats = atomic_load(&handle->stats.hold_repeats);
    stats->isr_calls = atomic_load(&handle->stats.isr_calls);
    stats->isr_queue_full = atomic_load(&handle->stats.isr_queue_full);
    return ESP_OK;
}

esp_err_t button_controller_get_diagnostic(button_handle_t handle,
                                            button_diagnostic_t *diagnostic)
{
    if (!handle || !diagnostic || !handle->in_use) {
        return ESP_ERR_INVALID_ARG;
    }
    diagnostic->gpio_pin = handle->config.gpio_pin;
    diagnostic->button_id = handle->config.button_id;
    diagnostic->raw_level = handle->last_raw_level;
    diagnostic->stable_level = handle->stable_level;
    diagnostic->is_pressed = atomic_load(&handle->is_pressed);
    diagnostic->state = atomic_load(&handle->current_state);
    diagnostic->isr_calls = atomic_load(&handle->stats.isr_calls);
    diagnostic->isr_queue_full = atomic_load(&handle->stats.isr_queue_full);
    diagnostic->total_events = atomic_load(&handle->stats.total_events);
    diagnostic->last_event = handle->last_event;
    diagnostic->last_event_timestamp_us = handle->last_event_timestamp_us;
    return ESP_OK;
}

esp_err_t button_controller_reset_stats(button_handle_t handle)
{
    if (!handle || !handle->in_use) {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_store(&handle->stats.total_events, 0);
    atomic_store(&handle->stats.total_presses, 0);
    atomic_store(&handle->stats.short_presses, 0);
    atomic_store(&handle->stats.long_presses, 0);
    atomic_store(&handle->stats.double_clicks, 0);
    atomic_store(&handle->stats.triple_clicks, 0);
    atomic_store(&handle->stats.multi_clicks, 0);
    atomic_store(&handle->stats.hold_repeats, 0);
    atomic_store(&handle->stats.isr_calls, 0);
    atomic_store(&handle->stats.isr_queue_full, 0);
    return ESP_OK;
}

esp_err_t button_controller_get_state(button_handle_t handle, button_state_t *state)
{
    if (!handle || !state || !handle->in_use) {
        return ESP_ERR_INVALID_ARG;
    }
    *state = atomic_load(&handle->current_state);
    return ESP_OK;
}

esp_err_t button_controller_is_pressed(button_handle_t handle, bool *is_pressed)
{
    if (!handle || !is_pressed || !handle->in_use) {
        return ESP_ERR_INVALID_ARG;
    }
    *is_pressed = atomic_load(&handle->is_pressed);
    return ESP_OK;
}
