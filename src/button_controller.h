/**
 * @file button_controller.h
 * @brief Advanced Button Controller for ESP32 using ESP-IDF Framework
 *
 * Features:
 * - Hardware debouncing with configurable timing
 * - Multiple press pattern detection (single, double, triple, long press)
 * - State machine-based event processing
 * - Thread-safe operations with proper resource management
 * - Comprehensive error handling and diagnostics
 * - Performance monitoring and statistics
 * - Power management integration
 * - Modular and extensible design
 */

#ifndef BUTTON_CONTROLLER_H
#define BUTTON_CONTROLLER_H

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_chip_info.h"

#ifdef __cplusplus
extern "C"
{
#endif

// Configuration constants
#define BUTTON_DEFAULT_DEBOUNCE_MS 50
#define BUTTON_DEFAULT_LONG_PRESS_MS 2000
#define BUTTON_DEFAULT_DOUBLE_CLICK_MS 300
#define BUTTON_DEFAULT_TRIPLE_CLICK_MS 500
#define BUTTON_MAX_CLICK_COUNT 5
#define BUTTON_EVENT_QUEUE_SIZE 32
#define BUTTON_TASK_STACK_SIZE 4096
#define BUTTON_TASK_PRIORITY 5
#define BUTTON_MAX_CONTROLLERS 8
#define BUTTON_TAG "BUTTON_CTRL"

// GPIO pin definitions for buttons
#define GPIO_BUTTON_POWER GPIO_NUM_16
#define GPIO_BUTTON_ENTER_MENU GPIO_NUM_19
#define GPIO_BUTTON_UP GPIO_NUM_17
#define GPIO_BUTTON_DOWN GPIO_NUM_5
#define GPIO_BUTTON_BACK GPIO_NUM_18

#define BUTTON_EVENT_STACK_SIZE 4096
#define BUTTON_EVENT_TASK_PRIORITY 5
#define BUTTON_EVENT_QUEUE_SIZE 32

    /**
     * @brief Button event types enumeration
     */
    typedef enum
    {
        BUTTON_EVENT_NONE = 0,
        BUTTON_EVENT_PRESS,
        BUTTON_EVENT_CLICK,
        BUTTON_EVENT_RELEASE,
        BUTTON_EVENT_REPEAT,
        BUTTON_EVENT_DOUBLE_CLICK,
        BUTTON_EVENT_TRIPLE_CLICK,
        BUTTON_EVENT_MULTI_CLICK,
        BUTTON_EVENT_LONG_PRESS,
        BUTTON_EVENT_VERY_LONG_PRESS,
        BUTTON_EVENT_MAX
    } button_raw_event_type_t;

    /**
     * @brief Button state enumeration for state machine
     */
    typedef enum
    {
        BUTTON_STATE_IDLE = 0,
        BUTTON_STATE_PRESSED,
        BUTTON_STATE_RELEASED,
        BUTTON_STATE_LONG_PRESS_ACTIVE,
        BUTTON_STATE_REPEAT_ACTIVE,
        BUTTON_STATE_MAX
    } button_state_t;

    // Button IDs
    typedef enum
    {
        BTN_POWER = 0,
        BTN_ENTER_MENU,
        BTN_UP,
        BTN_DOWN,
        BTN_BACK,
        BTN_COUNT
    } button_id_t;

    /**
     * @brief Button event information structure
     */
    typedef struct
    {
        button_raw_event_type_t event; /**< Event type */
        int64_t timestamp_us;          /**< Event timestamp in microseconds */
        uint32_t press_duration_ms;    /**< Event duration in milliseconds */
        uint8_t click_count;           /**< Number of consecutive clicks */
        gpio_num_t gpio_pin;           /**< GPIO pin that triggered the event */
        gpio_num_t controller_id;      /**< ID of the button controller instance */
        bool is_repeat;                /**< True if this is a repeat event */
        void *user_data;               /**< User-defined data pointer */
        button_state_t new_state;      /**< New state after event */
        int gpio_level;                /**< GPIO level at event time */
        char *controller_name;         /**< Name of the button controller */
        button_id_t button_id;         /**< ID of the button */
    } button_event_info_t;

    button_id_t gpio_to_button_id(gpio_num_t gpio);

    // Button mapping table
    typedef struct
    {
        gpio_num_t gpio_pin;
        button_id_t button_id;
        const char *name;
    } button_mapping_t;

    static const button_mapping_t button_mappings[] = {
        {GPIO_BUTTON_POWER, BTN_POWER, "Power"},
        {GPIO_BUTTON_ENTER_MENU, BTN_ENTER_MENU, "Enter/Menu"},
        {GPIO_BUTTON_UP, BTN_UP, "Up"},
        {GPIO_BUTTON_DOWN, BTN_DOWN, "Down"},
        {GPIO_BUTTON_BACK, BTN_BACK, "Back"}};

    // Convert GPIO pin to button ID
    button_id_t gpio_to_button_id(gpio_num_t gpio_pin)
    {
        for (int i = 0; i < sizeof(button_mappings) / sizeof(button_mappings[0]); i++)
        {
            if (button_mappings[i].gpio_pin == gpio_pin)
            {
                return button_mappings[i].button_id;
            }
        }
        return BTN_COUNT; // Invalid button
    }

    /**
     * @brief Button configuration structure
     */
    typedef struct
    {
        gpio_num_t gpio_pin; /**< GPIO pin number */
        button_id_t button_id;
        uint32_t debounce_ms;         /**< Debounce time in milliseconds */
        uint32_t long_press_ms;       /**< Long press threshold in milliseconds */
        uint32_t very_long_press_ms;  /** Very long press threshold in milliseconds */
        uint32_t double_click_ms;     /**< Double click timeout in milliseconds */
        uint32_t triple_click_ms;     /**< Triple click timeout in milliseconds */
        int active_level;             /**< Active level (HIGH/LOW) */
        char *controller_name;        /**< Name of the button controller */
        bool active_low;              /**< True if button is active low */
        bool enable_pullup;           /**< Enable internal pull-up resistor */
        bool enable_power_management; /**< Enable power management features */
        uint32_t hold_repeat_ms;      /**< Hold repeat interval in milliseconds */
    } button_config_t;

    /**
     * @brief Button statistics structure
     */
    typedef struct
    {
        uint32_t total_events;        /**< Total number of events processed */
        uint32_t short_presses;       /**< Number of short presses */
        uint32_t long_presses;        /**< Number of long presses */
        uint32_t double_clicks;       /**< Number of double clicks */
        uint32_t triple_clicks;       /**< Number of triple clicks */
        uint32_t multi_clicks;        /**< Number of multi-clicks (>3) */
        uint32_t debounce_triggers;   /**< Number of debounce events */
        uint32_t isr_calls;           /**< Total ISR invocations */
        int64_t last_event_time_us;   /**< Last event timestamp */
        uint32_t state_transitions;   /**< Number of state transitions */
        uint32_t error_count;         /**< Number of errors encountered */
        uint32_t total_releases;      /**< Total number of releases */
        uint32_t valid_state_changes; /**< Number of valid state changes */
        uint32_t bounce_events;       /**< Number of bounce events */
        uint32_t total_presses;       /**< Total number of presses */
        uint32_t hold_repeats;
        uint32_t total_interrupts;
        uint8_t click_count;
        // Click detection
        uint8_t consecutive_clicks;
        int64_t click_timestamps[BUTTON_MAX_CLICK_COUNT];
        int64_t first_click_time;
    } button_stats_t;

    /**
     * @brief Cleanup button system on failure
     */
    static void cleanup_button_system(void);

    /**
     * @brief Forward declaration of button handle (a pointer to internal structure)
     */
    typedef struct button_controller_t *button_handle_t;

    /**
     * @brief Button event callback function type
     *
     * @param event_info Pointer to event information structure
     * @param user_data User-defined data pointer passed during registration
     */
    typedef void (*button_event_callback_t)(const button_event_info_t *event_info, void *user_data);

    /**
     * @brief Error callback function type for handling controller errors
     *
     * @param error_code ESP error code
     * @param error_msg Error message string
     * @param user_data User-defined data pointer
     */

    typedef void (*button_error_callback_t)(esp_err_t error_code, const char *error_msg, void *user_data);

    typedef struct button_controller_t
    {
        // Configuration
        button_config_t config;

        // FreeRTOS resources
        QueueHandle_t event_queue;
        TaskHandle_t task_handle;

        // Debounce timer
        TimerHandle_t debounce_timer;
        int64_t debounce_time_ms;

        // Long press timer
        TimerHandle_t long_press_timer;
        int64_t long_press_time_ms;

        // Click timeout timer
        TimerHandle_t click_timeout_timer;
        int64_t double_click_time_ms;

        // Hold repeat timer
        TimerHandle_t hold_repeat_timer;
        int64_t repeat_start_ms;
        uint32_t repeat_interval_ms;

        // State mutex for thread-safe operations
        SemaphoreHandle_t state_mutex;

        // State flags
        volatile button_state_t current_state;
        volatile bool is_initialized;
        volatile bool is_running;
        volatile bool debounce_pending;
        bool repeat_press_time_ms;
        bool is_pressed;
        bool repeat_active;
        bool long_press_triggered;
        bool very_long_press_triggered;

        // Timing
        int64_t release_time_us;
        int64_t press_start_time_us;
        int64_t last_release_time_us;
        int64_t last_repeat_time;

        uint8_t consecutive_clicks;
        int64_t click_timestamps[5];

        // Previous GPIO state for edge detection
        int last_stable_level;
        bool waiting_for_timeout;

        // Statistics
        button_stats_t stats;

        // Callbacks and user data
        button_event_callback_t event_callback;
        button_error_callback_t error_callback;
        void *user_data;

        // Controller management
        uint8_t controller_id;
        bool in_use;
        int last_reported_level;

    } button_controller_t;

    typedef void (*button_error_callback_t)(esp_err_t error_code, const char *error_msg, void *user_data);
    static void button_queue_event(button_controller_t *btn, button_raw_event_type_t event, uint32_t duration_ms);
    static void button_determine_state_from_gpio(button_controller_t *btn);

    /**
     * @brief Initialize the button controller system
     *
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t button_controller_init(void);

    /**
     * @brief Deinitialize the button controller system
     *
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t button_controller_deinit(void);

    /**
     * @brief Create a new button controller instance
     *
     * @param config Pointer to button configuration structure
     * @param handle Pointer to store the button handle
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t button_controller_create(const button_config_t *config, button_handle_t *handle);

    /**
     * @brief Destroy a button controller instance
     *
     * @param handle Button handle to destroy
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t button_controller_destroy(button_handle_t handle);

    /**
     * @brief Start button monitoring
     *
     * @param handle Button handle
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t button_controller_start(button_handle_t handle);

    /**
     * @brief Stop button monitoring
     *
     * @param handle Button handle
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t button_controller_stop(button_handle_t handle);

    /**
     * @brief Register event callback function
     *
     * @param handle Button handle
     * @param callback Event callback function
     * @param user_data User-defined data pointer
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t button_controller_register_event_callback(button_handle_t handle,
                                                        button_event_callback_t callback,
                                                        void *user_data);

    /**
     * @brief Register error callback function
     *
     * @param handle Button handle
     * @param callback Error callback function
     * @param user_data User-defined data pointer
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t button_controller_register_error_callback(button_handle_t handle,
                                                        button_error_callback_t callback,
                                                        void *user_data);

    /**
     * @brief Get button controller statistics
     *
     * @param handle Button handle
     * @param stats Pointer to statistics structure to fill
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t button_controller_get_stats(button_handle_t handle, button_stats_t *stats);

    /**
     * @brief Reset button controller statistics
     *
     * @param handle Button handle
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t button_controller_reset_stats(button_handle_t handle);

    /**
     * @brief Get current button state
     *
     * @param handle Button handle
     * @param state Pointer to store current state
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t button_controller_get_state(button_handle_t handle, button_state_t *state);

    /**
     * @brief Check if button is currently pressed
     *
     * @param handle Button handle
     * @param is_pressed Pointer to store pressed state
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t button_controller_is_pressed(button_handle_t handle, bool *is_pressed);

    /**
     * @brief Get default button configuration
     *
     * @param config Pointer to configuration structure to fill with defaults
     */
    void button_controller_get_default_config(button_config_t *config);

    /**
     * @brief Validate button configuration
     *
     * @param config Pointer to configuration structure to validate
     * @return ESP_OK if valid, error code if invalid
     */
    esp_err_t button_controller_validate_config(const button_config_t *config);

    /**
     * @brief Convert event type to string
     *
     * @param event Event type
     * @return String representation of event type
     */
    static const char *button_event_to_string(button_raw_event_type_t event);

    /**
     * @brief Convert state type to string
     *
     * @param state State type
     * @return String representation of state type
     */
    static const char *button_state_to_string(button_state_t state);

    /**
     * @brief Timer callback for debounce handling
     * @param timer Timer handle
     */
    static void debounce_timer_callback(TimerHandle_t timer);
    /**
     * @brief Timer callback for long press detection
     * @param timer Timer handle
     */
    static void long_press_timer_callback(TimerHandle_t timer);
    /**
     * @brief Timer callback for hold repeat events
     * @param timer Timer handle
     */
    static void hold_repeat_timer_callback(TimerHandle_t timer);
    /**
     * @brief Timer callback for click timeout handling
     * @param timer Timer handle
     */
    static void click_timeout_timer_callback(TimerHandle_t timer);
#ifdef __cplusplus
}
#endif

#endif // BUTTON_CONTROLLER_H

/*
 * =============================================================================
 * IMPLEMENTATION
 * =============================================================================
 */

// Global controller pool
static button_controller_t g_button_controllers[BUTTON_MAX_CONTROLLERS];
static SemaphoreHandle_t g_controller_mutex = NULL;
static bool g_system_initialized = false;

// ISR handler lookup table for fast GPIO to controller mapping
static button_handle_t g_gpio_to_controller[GPIO_NUM_MAX] = {NULL};

/**
 * @brief Get default button configuration
 */
void button_controller_get_default_config(button_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    config->gpio_pin = GPIO_NUM_0;
    config->debounce_ms = BUTTON_DEFAULT_DEBOUNCE_MS;
    config->long_press_ms = BUTTON_DEFAULT_LONG_PRESS_MS;
    config->double_click_ms = BUTTON_DEFAULT_DOUBLE_CLICK_MS;
    config->triple_click_ms = BUTTON_DEFAULT_TRIPLE_CLICK_MS;
    config->active_low = true;
    config->enable_pullup = true;
    config->enable_power_management = false;
    config->hold_repeat_ms = 500;
}

/**
 * @brief Validate button configuration
 */
esp_err_t button_controller_validate_config(const button_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Validate GPIO pin
    if (config->gpio_pin < GPIO_NUM_0 || config->gpio_pin >= GPIO_NUM_MAX)
    {
        ESP_LOGE(BUTTON_TAG, "Invalid GPIO pin: %d\n", config->gpio_pin);
        return ESP_ERR_INVALID_ARG;
    }

    // Validate timing parameters
    if (config->debounce_ms > 1000)
    {
        ESP_LOGE(BUTTON_TAG, "Debounce time too large: %lu ms\n", config->debounce_ms);
        return ESP_ERR_INVALID_ARG;
    }

    if (config->long_press_ms > 30000)
    {
        ESP_LOGE(BUTTON_TAG, "Long press time too large: %lu ms\n", config->long_press_ms);
        return ESP_ERR_INVALID_ARG;
    }

    if (config->double_click_ms > 2000)
    {
        ESP_LOGE(BUTTON_TAG, "Double click timeout too large: %lu ms\n", config->double_click_ms);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/**
 * @brief Get event name as string
 */
static const char *button_event_to_string(button_raw_event_type_t event)
{
    switch (event)
    {
    case BUTTON_EVENT_PRESS:
        return "PRESS";
    case BUTTON_EVENT_RELEASE:
        return "RELEASE";
    case BUTTON_EVENT_CLICK:
        return "CLICK";
    case BUTTON_EVENT_DOUBLE_CLICK:
        return "DOUBLE_CLICK";
    case BUTTON_EVENT_TRIPLE_CLICK:
        return "TRIPLE_CLICK";
    case BUTTON_EVENT_MULTI_CLICK:
        return "MULTI_CLICK";
    case BUTTON_EVENT_LONG_PRESS:
        return "LONG_PRESS";
    case BUTTON_EVENT_VERY_LONG_PRESS:
        return "VERY_LONG_PRESS";
    case BUTTON_EVENT_REPEAT:
        return "REPEAT";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief Get state name as string
 */
static const char *button_state_to_string(button_state_t state)
{
    switch (state)
    {
    case BUTTON_STATE_IDLE:
        return "IDLE";
    case BUTTON_STATE_PRESSED:
        return "PRESSED";
    case BUTTON_STATE_RELEASED:
        return "RELEASED";
    case BUTTON_STATE_LONG_PRESS_ACTIVE:
        return "LONG_PRESS_ACTIVE";
    case BUTTON_STATE_REPEAT_ACTIVE:
        return "REPEAT_ACTIVE";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief Find available controller slot
 */
static button_handle_t find_available_controller(void)
{
    for (int i = 0; i < BUTTON_MAX_CONTROLLERS; i++)
    {
        if (!g_button_controllers[i].in_use)
        {
            g_button_controllers[i].in_use = true;
            g_button_controllers[i].controller_id = i;
            return &g_button_controllers[i];
        }
    }
    return NULL;
}

/**
 * @brief Release controller slot
 */
static void release_controller(button_handle_t handle)
{
    if (handle != NULL)
    {
        handle->in_use = false;
        handle->controller_id = 0xFF;
    }
}

/**
 * @brief ISR handler for button GPIO interrupts
 */
static void IRAM_ATTR button_gpio_isr_handler(void *arg)
{
    gpio_num_t gpio_pin = (gpio_num_t)(uintptr_t)arg;
    button_handle_t btn = g_gpio_to_controller[gpio_pin];
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (btn == NULL)
    {
        ets_printf("Ignoring ISR for GPIO %d (not running)\n", gpio_pin);
        return;
    }

    // Record ISR hit
    btn->stats.isr_calls++;
    btn->stats.total_interrupts++;

    // Disable further interrupts until debounce finishes
    gpio_intr_disable(btn->config.gpio_pin);
    btn->debounce_pending = true;
    // Reset debounce timer
    xTimerResetFromISR(btn->debounce_timer, &higher_priority_task_woken);

    // Yield if needed
    if (higher_priority_task_woken)
    {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Button processing task
 * This task processes button events from the queue and calls the appropriate handlers.
 */

// ============================================================================
// CONSOLIDATED EVENT PROCESSING TASK
// ============================================================================

/**
 * @brief Consolidated button event processing task
 *
 * This task efficiently processes button events from the queue and dispatches
 * them to registered callbacks. All event logic is centralized here.
 */
static void button_event_processing_task(void *pvParameters)
{
    button_handle_t btn = (button_handle_t)pvParameters;
    button_event_info_t event_info;

    if (!btn)
    {
        ESP_LOGE(BUTTON_TAG, "Task started with NULL button handle!");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(BUTTON_TAG, "✓ Event task started for %s (GPIO %d)",
             btn->config.controller_name, btn->config.gpio_pin);

    // Verify controller is marked as running
    if (!btn->is_running)
    {
        ESP_LOGE(BUTTON_TAG, "❌ Controller not marked as running for %s!",
                 btn->config.controller_name);
        vTaskDelete(NULL);
        return;
    }

    // Main event loop - runs until controller is stopped
    while (btn->is_running)
    {
        // Wait for events from queue (blocking with 100ms timeout)
        // This blocks efficiently - no CPU waste when idle
        if (xQueueReceive(btn->event_queue, &event_info, pdMS_TO_TICKS(100)) == pdPASS)
        {
            // Update statistics counter
            btn->stats.total_events++;

            // Log event for debugging
            ESP_LOGI(BUTTON_TAG, "[%s] Event: %s, Duration: %lums",
                     btn->config.controller_name,
                     button_event_to_string(event_info.event),
                     event_info.press_duration_ms);

            // ========================================
            // BUZZER + LED FEEDBACK INTEGRATION
            // ========================================

            switch (event_info.event)
            {
            case BUTTON_EVENT_CLICK:
                ESP_LOGI(BUTTON_TAG, "[%s] ✓ Single click", btn->config.controller_name);
                break;

            case BUTTON_EVENT_DOUBLE_CLICK:
                ESP_LOGI(BUTTON_TAG, "[%s] ✓✓ Double click", btn->config.controller_name);
                break;

            case BUTTON_EVENT_TRIPLE_CLICK:
                ESP_LOGI(BUTTON_TAG, "[%s] ✓✓✓ Triple click", btn->config.controller_name);
                break;

            case BUTTON_EVENT_LONG_PRESS:

                ESP_LOGI(BUTTON_TAG, "[%s] ⏱ Long press", btn->config.controller_name);
                break;

            case BUTTON_EVENT_REPEAT:
                // Handle repeat acceleration for UP/DOWN buttons
                if ((btn->config.button_id == BTN_UP || btn->config.button_id == BTN_DOWN) &&
                    btn->config.hold_repeat_ms > 0)
                {
                    if (btn->stats.hold_repeats > 3)
                    {
                        uint32_t fast_repeat_ms = btn->config.hold_repeat_ms / 2;
                        if (fast_repeat_ms < 50)
                            fast_repeat_ms = 50;
                        xTimerChangePeriod(btn->hold_repeat_timer,
                                           pdMS_TO_TICKS(fast_repeat_ms), 0);
                    }
                }
                ESP_LOGD(BUTTON_TAG, "[%s] ⟳ Repeat", btn->config.controller_name);
                break;

            case BUTTON_EVENT_RELEASE:
                // Reset repeat rate to default on release
                if (btn->config.hold_repeat_ms > 0)
                {
                    xTimerChangePeriod(btn->hold_repeat_timer,
                                       pdMS_TO_TICKS(btn->config.hold_repeat_ms), 0);
                }
                ESP_LOGD(BUTTON_TAG, "[%s] ↑ Released", btn->config.controller_name);
                break;

            default:
                ESP_LOGW(BUTTON_TAG, "[%s] ⚠ Unhandled event: %d",
                         btn->config.controller_name, event_info.event);
                break;
            }

            // Dispatch event to user callback if registered
            if (btn->event_callback)
            {
                btn->event_callback(&event_info, btn->user_data);
            }

            // Periodic statistics logging (every 50 events for more frequent feedback)
            if (btn->stats.total_events % 50 == 0 && btn->stats.total_events > 0)
            {
                ESP_LOGI(BUTTON_TAG,
                         "[%s] 📊 Stats - Events: %lu, Clicks: %lu, Double: %lu, Triple: %lu, Long: %lu, Repeats: %lu",
                         btn->config.controller_name,
                         btn->stats.total_events,
                         btn->stats.short_presses,
                         btn->stats.double_clicks,
                         btn->stats.triple_clicks,
                         btn->stats.long_presses,
                         btn->stats.hold_repeats);
            }
        }
        // xQueueReceive handles blocking efficiently - no need for additional delays
        // When no events, task blocks at xQueueReceive for up to 100ms
        // This is CPU-efficient and responsive
    }

    // Task cleanup - only reached when is_running becomes false
    ESP_LOGI(BUTTON_TAG, "🛑 Event task stopping for %s (GPIO %d)",
             btn->config.controller_name, btn->config.gpio_pin);

    // Drain any remaining events in queue
    uint32_t drained = 0;
    while (xQueueReceive(btn->event_queue, &event_info, 0) == pdPASS)
    {
        drained++;
        ESP_LOGD(BUTTON_TAG, "Draining event: %s", button_event_to_string(event_info.event));
    }

    if (drained > 0)
    {
        ESP_LOGI(BUTTON_TAG, "Drained %lu events from queue", drained);
    }

    ESP_LOGI(BUTTON_TAG, "✓ Task deleted for %s", btn->config.controller_name);

    // Task deletes itself
    vTaskDelete(NULL);
}

// ============================================================================
// FAST EVENT QUEUING (Called from Timer Callbacks)
// ============================================================================

/**
 * @brief Queue button event (FAST - just sends to queue, no processing)
 *
 * This is called from timer callbacks.
 * It creates the event and sends it to the queue immediately.
 * The actual handler is called later by the event processing task.
 */
static void button_queue_event(button_controller_t *btn, button_raw_event_type_t event, uint32_t duration_ms)
{
    // Create event structure
    button_event_info_t event_info = {
        .button_id = btn->config.button_id,
        .event = event,
        .timestamp_us = esp_timer_get_time(),
        .press_duration_ms = duration_ms,
        .gpio_pin = btn->config.gpio_pin,
        .controller_id = btn->controller_id,
        .is_repeat = (event == BUTTON_EVENT_REPEAT),
        .user_data = NULL};

    // Send to queue (non-blocking from timer context)
    BaseType_t result = xQueueSend(btn->event_queue, &event_info, 0);

    if (result != pdPASS)
    {
        // Queue full - event dropped (should rarely happen)
        printf("[QUEUE] Event dropped! GPIO %d, Event %d\n", btn->config.gpio_pin, event);
    }

    btn->last_release_time_us = event_info.timestamp_us;

    // That's it! Very fast - just queued the event
}

/**
 * @brief Calculate press duration in milliseconds
 */
static inline uint32_t calculate_press_duration(button_controller_t *btn)
{
    return (esp_timer_get_time() - btn->press_start_time_us) / 1000;
}

/**
 * @brief Calculate release duration in milliseconds
 */
static inline uint32_t calculate_release_duration(button_controller_t *btn)
{
    return (btn->release_time_us - btn->press_start_time_us) / 1000;
}

/**
 * @brief Stop all active timers
 */
static void stop_all_timers(button_controller_t *btn)
{
    xTimerStop(btn->long_press_timer, 0);
    xTimerStop(btn->hold_repeat_timer, 0);
}

/**
 * @brief Determine button state based on GPIO level after debounce
 *
 * OPTIMIZED: Minimal processing, just reads GPIO, updates state, queues event
 */
/**
 * @brief Determine state from GPIO (called from timer callback)
 */
// ============================================================================
// COMPLETE STATE MACHINE WITH CLICK DETECTION
// ============================================================================

static void button_determine_state_from_gpio(button_controller_t *btn)
{
    if (!btn)
    {
        ESP_LOGE(BUTTON_TAG, "NULL button controller!");
        return;
    }

    // Acquire mutex for thread-safe state machine
    if (xSemaphoreTake(btn->state_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        btn->stats.error_count++;
        ESP_LOGW(BUTTON_TAG, "Failed to acquire mutex in debounce callback");
        gpio_intr_enable(btn->config.gpio_pin);
        return;
    }

    // Read GPIO level
    int gpio_level = gpio_get_level(btn->config.gpio_pin);
    bool is_pressed = btn->config.active_low ? (gpio_level == 0) : (gpio_level == 1);

    uint32_t duration_ms;
    int64_t current_time = esp_timer_get_time();

    ESP_LOGD(BUTTON_TAG, "GPIO %d: level=%d, pressed=%d, state=%s",
             btn->config.gpio_pin, gpio_level, is_pressed,
             button_state_to_string(btn->current_state));

    // State machine processing
    switch (btn->current_state)
    {
    case BUTTON_STATE_IDLE:
        if (!is_pressed)
        {
            xSemaphoreGive(btn->state_mutex);
            return;
        }

        // Button pressed from IDLE
        ESP_LOGI(BUTTON_TAG, "GPIO %d: IDLE → PRESSED", btn->config.gpio_pin);
        ets_printf("Button pressed on GPIO %d\n", btn->config.gpio_pin);
        btn->current_state = BUTTON_STATE_PRESSED;
        btn->press_start_time_us = current_time;
        btn->long_press_triggered = false;
        btn->is_pressed = true;
        btn->stats.total_presses++;

        // Stop click timeout if running (shouldn't be, but safety check)
        xTimerStop(btn->click_timeout_timer, 0);

        // Start long press timer
        xTimerStart(btn->long_press_timer, 0);
        break;

    case BUTTON_STATE_PRESSED:
        if (!is_pressed)
        {
            // Button released - check if it's a short press (potential click)
            duration_ms = (current_time - btn->press_start_time_us) / 1000;

            // ESP_LOGE(BUTTON_TAG, "GPIO %d: PRESSED → RELEASED (duration: %lums)",
            //          btn->config.gpio_pin, duration_ms);
            ets_printf("Button released on GPIO %d after %lums\n",
                       btn->config.gpio_pin, duration_ms);

            btn->release_time_us = current_time;
            btn->last_release_time_us = current_time;
            btn->is_pressed = false;
            btn->stats.total_releases++;

            // Stop long press timer
            xTimerStop(btn->long_press_timer, 0);
            xTimerStop(btn->hold_repeat_timer, 0);

            // Check if this qualifies as a click (short press)
            if (duration_ms < btn->config.long_press_ms)
            {
                // This is a potential click
                btn->consecutive_clicks++;

                ets_printf("Click #%d detected on GPIO %d\n",
                           btn->consecutive_clicks, btn->config.gpio_pin);
                // Transition to RELEASED state to wait for more clicks
                btn->current_state = BUTTON_STATE_RELEASED;
                btn->waiting_for_timeout = true;

                // Start/restart click timeout timer
                // Use double_click_ms as the window for additional clicks
                xTimerChangePeriod(btn->click_timeout_timer,
                                   pdMS_TO_TICKS(btn->config.double_click_ms), 0);
                xTimerReset(btn->click_timeout_timer, 0);
            }
            else
            {
                // Long press was already triggered, just send release
                btn->current_state = BUTTON_STATE_IDLE;
                btn->consecutive_clicks = 0;
                btn->waiting_for_timeout = false;
                button_queue_event(btn, BUTTON_EVENT_RELEASE, duration_ms);
            }
        }
        break;

    case BUTTON_STATE_LONG_PRESS_ACTIVE:
    case BUTTON_STATE_REPEAT_ACTIVE:
        if (!is_pressed)
        {
            // Long press or repeat released
            // ESP_LOGI(BUTTON_TAG, "GPIO %d: LONG_PRESS → IDLE", btn->config.gpio_pin);
            ets_printf("Button released from long press on GPIO %d\n",
                       btn->config.gpio_pin);

            btn->release_time_us = current_time;
            btn->last_release_time_us = current_time;
            btn->is_pressed = false;
            btn->stats.total_releases++;

            // Stop timers
            stop_all_timers(btn);

            // Reset to IDLE, no click counting for long presses
            btn->current_state = BUTTON_STATE_IDLE;
            btn->consecutive_clicks = 0;
            btn->waiting_for_timeout = false;

            duration_ms = (current_time - btn->press_start_time_us) / 1000;
            button_queue_event(btn, BUTTON_EVENT_RELEASE, duration_ms);
        }
        break;

    case BUTTON_STATE_RELEASED:
        if (is_pressed)
        {
            // Another press detected - this is a multi-click
            int64_t time_since_release = current_time - btn->last_release_time_us;
            uint32_t time_since_release_ms = time_since_release / 1000;

            // ESP_LOGI(BUTTON_TAG, "GPIO %d: New press after %lums (click #%d)",
            //          btn->config.gpio_pin, time_since_release_ms,
            //          btn->consecutive_clicks + 1);
            ets_printf("New press detected on GPIO %d after %lums (click #%d)\n",
                       btn->config.gpio_pin, time_since_release_ms,
                       btn->consecutive_clicks + 1);

            // Check if within multi-click window
            if (time_since_release_ms <= btn->config.double_click_ms)
            {
                // Valid multi-click continuation
                btn->current_state = BUTTON_STATE_PRESSED;
                btn->press_start_time_us = current_time;

                // Stop the click timeout timer (we're still clicking)
                xTimerStop(btn->click_timeout_timer, 0);

                // Start long press timer for this press
                xTimerStart(btn->long_press_timer, 0);
            }
            else
            {
                // Too long since last click - this is a new sequence
                // Finalize previous clicks first
                if (btn->consecutive_clicks > 0)
                {
                    button_raw_event_type_t event_type;

                    switch (btn->consecutive_clicks)
                    {
                    case 1:
                        event_type = BUTTON_EVENT_CLICK;
                        btn->stats.short_presses++;
                        break;
                    case 2:
                        event_type = BUTTON_EVENT_DOUBLE_CLICK;
                        btn->stats.double_clicks++;
                        break;
                    case 3:
                        event_type = BUTTON_EVENT_TRIPLE_CLICK;
                        btn->stats.triple_clicks++;
                        break;
                    default:
                        event_type = BUTTON_EVENT_MULTI_CLICK;
                        btn->stats.multi_clicks++;
                        break;
                    }

                    button_queue_event(btn, event_type, 0);
                }

                // Start new click sequence
                btn->consecutive_clicks = 0;
                btn->current_state = BUTTON_STATE_PRESSED;
                btn->press_start_time_us = current_time;
                xTimerStart(btn->long_press_timer, 0);
            }
        }
        // If still released, click timeout timer will handle finalization
        break;

    default:
        ESP_LOGW(BUTTON_TAG, "GPIO %d: Unknown state, resetting to IDLE",
                 btn->config.gpio_pin);
        btn->current_state = BUTTON_STATE_IDLE;
        btn->consecutive_clicks = 0;
        btn->waiting_for_timeout = false;
        xTimerStop(btn->click_timeout_timer, 0);
        break;
    }

    btn->stats.valid_state_changes++;
    xSemaphoreGive(btn->state_mutex);
}

/**
 * @brief Initialize button controller system
 */
esp_err_t button_controller_init(void)
{
    if (g_system_initialized)
    {
        // ESP_LOGW(BUTTON_TAG, "Button controller system already initialized");
        return ESP_OK;
    }

    // Create global controller mutex
    g_controller_mutex = xSemaphoreCreateMutex();
    if (g_controller_mutex == NULL)
    {
        ESP_LOGE(BUTTON_TAG, "Failed to create controller mutex\n");
        return ESP_ERR_NO_MEM;
    }

    // Initialize GPIO ISR service
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(BUTTON_TAG, "Failed to install GPIO ISR service: %s\n", esp_err_to_name(ret));
        for (int i = 0; i < BUTTON_MAX_CONTROLLERS; i++)
            if (g_button_controllers[i].task_handle)
            {
                vTaskDelete(g_button_controllers[i].task_handle);
                g_button_controllers[i].task_handle = NULL;
                vSemaphoreDelete(g_button_controllers[i].state_mutex);
                g_button_controllers[i].state_mutex = NULL;
            }
        vSemaphoreDelete(g_controller_mutex);
        g_controller_mutex = NULL;
        return ret;
    }
    ESP_LOGI(BUTTON_TAG, "GPIO ISR service installed\n");
    // Initialize controller pool
    memset(g_button_controllers, 0, sizeof(g_button_controllers));
    memset(g_gpio_to_controller, 0, sizeof(g_gpio_to_controller));

    // Initialize controller IDs
    for (int i = 0; i < BUTTON_MAX_CONTROLLERS; i++)
    {
        g_button_controllers[i].controller_id = i;
        g_button_controllers[i].is_initialized = false;
    }
    g_system_initialized = true;
    ESP_LOGI(BUTTON_TAG, "Button controller system initialized\n");
    ESP_LOGI(BUTTON_TAG, " - Event queue size: %d\n", BUTTON_EVENT_QUEUE_SIZE);
    ESP_LOGI(BUTTON_TAG, " - Max controllers: %d\n", BUTTON_MAX_CONTROLLERS);

    return ESP_OK;
}

/**
 * @brief Deinitialize button controller system
 */
esp_err_t button_controller_deinit(void)
{
    if (!g_system_initialized)
    {
        return ESP_OK;
    }

    // Stop all active controllers
    for (int i = 0; i < BUTTON_MAX_CONTROLLERS; i++)
    {
        if (g_button_controllers[i].in_use)
        {
            button_controller_destroy(&g_button_controllers[i]);
        }
    }

    // Delete global mutex
    if (g_controller_mutex)
    {
        vSemaphoreDelete(g_controller_mutex);
        g_controller_mutex = NULL;
    }
    // Uninstall GPIO ISR service
    gpio_uninstall_isr_service();
    g_system_initialized = false;
    ESP_LOGI(BUTTON_TAG, "Button controller system deinitialized\n");

    return ESP_OK;
}

/**
 * @brief Configure GPIO for button input
 */
static esp_err_t configure_button_gpio(button_handle_t controller)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << controller->config.gpio_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = controller->config.enable_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE};

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(BUTTON_TAG, "GPIO configuration failed for pin %d: %s\n",
                 controller->config.gpio_pin, esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

/**
 * @brief Cleanup controller resources
 */
static void cleanup_controller_resources(button_handle_t controller)
{
    if (controller == NULL)
        return;

    // Remove from GPIO mapping
    if (controller->config.gpio_pin < GPIO_NUM_MAX)
    {
        g_gpio_to_controller[controller->config.gpio_pin] = NULL;
    }

    // Remove ISR handler
    gpio_isr_handler_remove(controller->config.gpio_pin);

    // Stop and delete timers
    if (controller->debounce_timer)
    {
        xTimerStop(controller->debounce_timer, 0);
        xTimerDelete(controller->debounce_timer, 0);
        controller->debounce_timer = NULL;
    }

    if (controller->long_press_timer)
    {
        xTimerStop(controller->long_press_timer, 0);
        xTimerDelete(controller->long_press_timer, 0);
        controller->long_press_timer = NULL;
    }

    if (controller->hold_repeat_timer)
    {
        xTimerStop(controller->hold_repeat_timer, 0);
        xTimerDelete(controller->hold_repeat_timer, 0);
        controller->hold_repeat_timer = NULL;
    }

    if (controller->click_timeout_timer)
    {
        xTimerStop(controller->click_timeout_timer, 0);
        xTimerDelete(controller->click_timeout_timer, 0);
        controller->click_timeout_timer = NULL;
    }

    // Delete task
    if (controller->task_handle)
    {
        controller->is_running = false;
        vTaskDelay(pdMS_TO_TICKS(100)); // Allow task to exit gracefully
        // Task deletes itself
        controller->task_handle = NULL;
    }

    // Delete queue
    if (controller->event_queue)
    {
        vQueueDelete(controller->event_queue);
        controller->event_queue = NULL;
    }

    // Delete mutex
    if (controller->state_mutex)
    {
        vSemaphoreDelete(controller->state_mutex);
        controller->state_mutex = NULL;
    }

    // Clear controller state
    controller->is_initialized = false;
    controller->is_running = false;
}

// ============================================================================
// UPDATED BUTTON CONTROLLER STRUCTURE (ADD TO HEADER)
// ============================================================================

/*
 * Add to button_controller_t structure:
 *
 * TimerHandle_t click_timeout_timer;  // Timer for multi-click detection
 * uint8_t consecutive_clicks;          // Click counter (already exists)
 * bool waiting_for_timeout;            // Waiting for click timeout (already exists)
 */

// ============================================================================
// UPDATED TIMER CREATION (ADD CLICK TIMEOUT TIMER)
// ============================================================================

static esp_err_t create_button_timers(button_handle_t controller)
{
    if (!controller)
    {
        ESP_LOGE(BUTTON_TAG, "Invalid controller for timer creation\n");
        return ESP_ERR_INVALID_ARG;
    }

    static char timer_name_buffer[48];

    // Initialize all timer handles to NULL
    controller->debounce_timer = NULL;
    controller->long_press_timer = NULL;
    controller->hold_repeat_timer = NULL;
    controller->click_timeout_timer = NULL; // NEW

    ESP_LOGI(BUTTON_TAG, "Creating timers for controller ID %d, GPIO %d\n",
             controller->controller_id, controller->config.gpio_pin);

    // Create debounce timer (one-shot)
    memset(timer_name_buffer, 0, sizeof(timer_name_buffer));
    snprintf(timer_name_buffer, sizeof(timer_name_buffer), "btn_deb_%d_%d",
             controller->controller_id, controller->config.gpio_pin);

    controller->debounce_timer = xTimerCreate(
        timer_name_buffer,
        pdMS_TO_TICKS(controller->config.debounce_ms),
        pdFALSE,
        (void *)controller,
        debounce_timer_callback);

    if (!controller->debounce_timer)
    {
        ESP_LOGE(BUTTON_TAG, "Failed to create debounce timer\n");
        goto cleanup_timers;
    }

    // Create long press timer (one-shot)
    memset(timer_name_buffer, 0, sizeof(timer_name_buffer));
    snprintf(timer_name_buffer, sizeof(timer_name_buffer), "btn_lp_%d_%d",
             controller->controller_id, controller->config.gpio_pin);

    controller->long_press_timer = xTimerCreate(
        timer_name_buffer,
        pdMS_TO_TICKS(controller->config.long_press_ms),
        pdFALSE,
        (void *)controller,
        long_press_timer_callback);

    if (!controller->long_press_timer)
    {
        ESP_LOGE(BUTTON_TAG, "Failed to create long press timer\n");
        goto cleanup_timers;
    }

    // Create hold repeat timer (auto-reload)
    memset(timer_name_buffer, 0, sizeof(timer_name_buffer));
    snprintf(timer_name_buffer, sizeof(timer_name_buffer), "btn_hd_%d_%d",
             controller->controller_id, controller->config.gpio_pin);

    controller->hold_repeat_timer = xTimerCreate(
        timer_name_buffer,
        pdMS_TO_TICKS(controller->config.hold_repeat_ms),
        pdTRUE,
        (void *)controller,
        hold_repeat_timer_callback);

    if (!controller->hold_repeat_timer)
    {
        ESP_LOGE(BUTTON_TAG, "Failed to create hold repeat timer\n");
        goto cleanup_timers;
    }

    // Create click timeout timer (one-shot) - NEW
    memset(timer_name_buffer, 0, sizeof(timer_name_buffer));
    snprintf(timer_name_buffer, sizeof(timer_name_buffer), "btn_clk_%d_%d",
             controller->controller_id, controller->config.gpio_pin);

    controller->click_timeout_timer = xTimerCreate(
        timer_name_buffer,
        pdMS_TO_TICKS(controller->config.double_click_ms),
        pdFALSE,
        (void *)controller,
        click_timeout_timer_callback);

    if (!controller->click_timeout_timer)
    {
        ESP_LOGE(BUTTON_TAG, "Failed to create click timeout timer\n");
        goto cleanup_timers;
    }

    ESP_LOGI(BUTTON_TAG, "All timers created successfully for GPIO %d\n",
             controller->config.gpio_pin);

    return ESP_OK;

cleanup_timers:
    if (controller->debounce_timer)
    {
        xTimerDelete(controller->debounce_timer, portMAX_DELAY);
        controller->debounce_timer = NULL;
    }
    if (controller->long_press_timer)
    {
        xTimerDelete(controller->long_press_timer, portMAX_DELAY);
        controller->long_press_timer = NULL;
    }
    if (controller->hold_repeat_timer)
    {
        xTimerDelete(controller->hold_repeat_timer, portMAX_DELAY);
        controller->hold_repeat_timer = NULL;
    }
    if (controller->click_timeout_timer)
    {
        xTimerDelete(controller->click_timeout_timer, portMAX_DELAY);
        controller->click_timeout_timer = NULL;
    }

    return ESP_ERR_NO_MEM;
}

// Safe timer cleanup function
static void cleanup_button_timers(button_handle_t controller)
{
    if (!controller)
    {
        return;
    }

    ESP_LOGI(BUTTON_TAG, "Cleaning up timers for controller ID %d\n", controller->controller_id);

    // Stop and delete timers with proper error checking
    if (controller->debounce_timer)
    {
        if (xTimerIsTimerActive(controller->debounce_timer))
        {
            xTimerStop(controller->debounce_timer, portMAX_DELAY);
        }
        xTimerDelete(controller->debounce_timer, portMAX_DELAY);
        controller->debounce_timer = NULL;
    }

    if (controller->long_press_timer)
    {
        if (xTimerIsTimerActive(controller->long_press_timer))
        {
            xTimerStop(controller->long_press_timer, portMAX_DELAY);
        }
        xTimerDelete(controller->long_press_timer, portMAX_DELAY);
        controller->long_press_timer = NULL;
    }

    if (controller->hold_repeat_timer)
    {
        if (xTimerIsTimerActive(controller->hold_repeat_timer))
        {
            xTimerStop(controller->hold_repeat_timer, portMAX_DELAY);
        }
        xTimerDelete(controller->hold_repeat_timer, portMAX_DELAY);
        controller->hold_repeat_timer = NULL;
    }

    if (controller->click_timeout_timer)
    {
        if (xTimerIsTimerActive(controller->click_timeout_timer))
        {
            xTimerStop(controller->click_timeout_timer, portMAX_DELAY);
        }
        xTimerDelete(controller->click_timeout_timer, portMAX_DELAY);
        controller->click_timeout_timer = NULL;
    }
}

// Integration point - add this after "ESP_LOGE(TAG, "Configured GPIO %d for button input", config->gpio_pin);"
esp_err_t button_controller_create(const button_config_t *config, button_handle_t *handle)
{
    const char *TAG = "button_controller_create";

    if (!g_system_initialized)
    {
        ESP_LOGE(TAG, "Button controller system not initialized\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL || handle == NULL)
    {
        ESP_LOGE(TAG, "Invalid arguments to button_controller_create\n");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Creating button controller for GPIO %d\n", config->gpio_pin);

    // Validate configuration
    esp_err_t ret = button_controller_validate_config(config);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Check if GPIO is already in use
    if (g_gpio_to_controller[config->gpio_pin] != NULL)
    {
        ESP_LOGE(TAG, "GPIO %d already in use by another controller\n", config->gpio_pin);
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_controller_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to acquire controller mutex\n");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Acquired controller mutex\n");

    // Find available controller slot
    button_handle_t controller = find_available_controller();
    if (controller == NULL)
    {
        ESP_LOGE(TAG, "No available controller slots (max: %d)\n", BUTTON_MAX_CONTROLLERS);
        xSemaphoreGive(g_controller_mutex);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Found available controller slot ID: %d\n", controller->controller_id);

    // Initialize controller structure
    controller->config = *config;
    controller->current_state = BUTTON_STATE_IDLE;
    controller->is_initialized = false;
    controller->is_running = false;
    controller->press_start_time_us = 0;
    controller->last_release_time_us = 0;
    controller->consecutive_clicks = 0;
    memset(&controller->stats, 0, sizeof(controller->stats));
    memset(controller->click_timestamps, 0, sizeof(controller->click_timestamps));
    controller->event_callback = NULL;
    controller->error_callback = NULL;
    controller->user_data = NULL;
    controller->last_reported_level = gpio_get_level(config->gpio_pin);
    controller->last_stable_level = controller->last_reported_level;
    controller->is_pressed = false;

    // Create synchronization primitives
    controller->state_mutex = xSemaphoreCreateMutex();
    if (controller->state_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create state mutex\n");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    ESP_LOGI(TAG, "Created state mutex\n");

    // Create event queue
    controller->event_queue = xQueueCreate(BUTTON_EVENT_QUEUE_SIZE, sizeof(button_event_info_t));
    if (controller->event_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create event queue\n");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    ESP_LOGI(TAG, "Created event queue for button ID %d\n", controller->controller_id);

    // Configure GPIO
    ret = configure_button_gpio(controller);
    if (ret != ESP_OK)
    {
        goto cleanup;
    }
    ESP_LOGI(TAG, "Configured GPIO %d for button input\n", config->gpio_pin);

    // CREATE TIMERS - This is where the refactored code integrates
    ret = create_button_timers(controller);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create timers for controller ID %d: %s\n",
                 controller->controller_id, esp_err_to_name(ret));
        goto cleanup;
    }

    // Mark controller as initialized
    controller->is_initialized = true;

    // Register controller in global mapping
    g_gpio_to_controller[config->gpio_pin] = controller;

    // Release mutex
    xSemaphoreGive(g_controller_mutex);

    // Set output handle
    *handle = controller;

    ESP_LOGI(TAG, "Button controller created successfully for GPIO %d (controller ID: %d)\n",
             config->gpio_pin, controller->controller_id);

    return ESP_OK;

cleanup:
    ESP_LOGE(TAG, "Cleanup required for controller ID %d\n", controller->controller_id);

    // Clean up timers
    cleanup_button_timers(controller);

    // Clean up synchronization primitives
    if (controller->state_mutex)
    {
        vSemaphoreDelete(controller->state_mutex);
        controller->state_mutex = NULL;
    }

    if (controller->event_queue)
    {
        vQueueDelete(controller->event_queue);
        controller->event_queue = NULL;
    }

    // Reset controller slot (assuming your system has a way to mark it as available)
    controller->is_initialized = false;
    controller->is_running = false;

    // Remove from GPIO mapping if it was set
    if (g_gpio_to_controller[config->gpio_pin] == controller)
    {
        g_gpio_to_controller[config->gpio_pin] = NULL;
    }

    xSemaphoreGive(g_controller_mutex);

    return ret;
}

// Enhanced timer callbacks with better safety checks
// ========================================================================
// Streamlined Timer Callbacks - No user callbacks, minimal processing
// ========================================================================

static void debounce_timer_callback(TimerHandle_t xTimer)
{
    button_handle_t btn = (button_handle_t)pvTimerGetTimerID(xTimer);

    if (btn == NULL)
    {
        ets_printf("Debounce timer callback with NULL button handle\n");
        return;
    }
    if (!btn->debounce_pending)
    {
        // Spurious callback, ignore
        return;
    }
    // CRITICAL FIX: Actually process the button state
    button_determine_state_from_gpio(btn);

    // CRITICAL FIX: Re-enable GPIO interrupt after debounce
    gpio_intr_enable(btn->config.gpio_pin);

    // Clear debounce pending flag
    btn->debounce_pending = false;
}

// Called by long_press_timer when long press duration is reached
// ============================================================================
// LONG PRESS TIMER CALLBACK
// ============================================================================

static void long_press_timer_callback(TimerHandle_t xTimer)
{
    button_handle_t btn = (button_handle_t)pvTimerGetTimerID(xTimer);

    if (!btn)
    {
        return;
    }

    // Verify button is still pressed
    int gpio_level = gpio_get_level(btn->config.gpio_pin);
    bool is_pressed = btn->config.active_low ? (gpio_level == 0) : (gpio_level == 1);

    if (!is_pressed)
    {
        return;
    }

    if (xSemaphoreTake(btn->state_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        if (is_pressed && btn->current_state == BUTTON_STATE_PRESSED)
        {
            // Long press detected - cancel any click counting
            btn->current_state = BUTTON_STATE_LONG_PRESS_ACTIVE;
            btn->long_press_triggered = true;
            btn->stats.long_presses++;

            // Cancel click detection
            btn->consecutive_clicks = 0;
            btn->waiting_for_timeout = false;
            xTimerStop(btn->click_timeout_timer, 0);

            uint32_t duration_ms = (esp_timer_get_time() - btn->press_start_time_us) / 1000;
            button_queue_event(btn, BUTTON_EVENT_LONG_PRESS, duration_ms);

            // Start hold repeat if configured
            if (btn->config.hold_repeat_ms > 0)
            {
                xTimerStart(btn->hold_repeat_timer, 0);
            }
        }
        xSemaphoreGive(btn->state_mutex);
    }
}

// ============================================================================
// HOLD REPEAT TIMER CALLBACK
// ============================================================================

static void hold_repeat_timer_callback(TimerHandle_t xTimer)
{
    button_handle_t btn = (button_handle_t)pvTimerGetTimerID(xTimer);

    if (!btn)
    {
        return;
    }

    // Verify button is still pressed
    int gpio_level = gpio_get_level(btn->config.gpio_pin);
    bool is_pressed = btn->config.active_low ? (gpio_level == 0) : (gpio_level == 1);

    if (!is_pressed)
    {
        xTimerStop(xTimer, 0);
        return;
    }

    if (btn->current_state == BUTTON_STATE_LONG_PRESS_ACTIVE)
    {
        btn->current_state = BUTTON_STATE_REPEAT_ACTIVE;
    }

    uint32_t duration_ms = (esp_timer_get_time() - btn->press_start_time_us) / 1000;
    button_queue_event(btn, BUTTON_EVENT_REPEAT, duration_ms);
    btn->stats.hold_repeats++;
}

// ============================================================================
// CLICK DETECTION TIMER CALLBACK (NEW)
// ============================================================================

/**
 * @brief Click timeout timer callback
 * This fires after the multi-click window expires to finalize click count
 */
static void click_timeout_timer_callback(TimerHandle_t xTimer)
{
    button_handle_t btn = (button_handle_t)pvTimerGetTimerID(xTimer);

    if (!btn)
    {
        return;
    }

    if (xSemaphoreTake(btn->state_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        // Click timeout expired - finalize the click event
        if (btn->consecutive_clicks > 0)
        {
            button_raw_event_type_t event_type;
            uint32_t duration_ms = (btn->last_release_time_us - btn->press_start_time_us) / 1000;

            switch (btn->consecutive_clicks)
            {
            case 1:
                event_type = BUTTON_EVENT_CLICK;
                btn->stats.short_presses++;
                ESP_LOGI(BUTTON_TAG, "GPIO %d: Single click confirmed", btn->config.gpio_pin);
                break;

            case 2:
                event_type = BUTTON_EVENT_DOUBLE_CLICK;
                btn->stats.double_clicks++;
                ESP_LOGI(BUTTON_TAG, "GPIO %d: Double click confirmed", btn->config.gpio_pin);
                break;

            case 3:
                event_type = BUTTON_EVENT_TRIPLE_CLICK;
                btn->stats.triple_clicks++;
                ESP_LOGI(BUTTON_TAG, "GPIO %d: Triple click confirmed", btn->config.gpio_pin);
                break;

            default:
                event_type = BUTTON_EVENT_MULTI_CLICK;
                btn->stats.multi_clicks++;
                ESP_LOGI(BUTTON_TAG, "GPIO %d: Multi-click (%d) confirmed",
                         btn->config.gpio_pin, btn->consecutive_clicks);
                break;
            }

            // Queue the final click event
            button_queue_event(btn, event_type, duration_ms);

            // Reset click tracking
            btn->consecutive_clicks = 0;
            btn->waiting_for_timeout = false;
        }

        // Return to IDLE state
        btn->current_state = BUTTON_STATE_IDLE;

        xSemaphoreGive(btn->state_mutex);
    }
}

/**
 * @brief Destroy button controller instance
 */
esp_err_t button_controller_destroy(button_handle_t handle)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!handle->in_use)
    {
        ESP_LOGW(BUTTON_TAG, "Attempting to destroy inactive controller\n");
        return ESP_OK;
    }

    ESP_LOGI(BUTTON_TAG, "Destroying %s\n", handle->config.controller_name);

    // Stop the controller first
    button_controller_stop(handle);

    if (xSemaphoreTake(g_controller_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(BUTTON_TAG, "Failed to acquire controller mutex for destroy\n");
        return ESP_ERR_TIMEOUT;
    }

    cleanup_controller_resources(handle);
    release_controller(handle);

    xSemaphoreGive(g_controller_mutex);

    ESP_LOGI(BUTTON_TAG, "Button controller destroyed\n");
    return ESP_OK;
}

/**
 * @brief Start button monitoring
 */
esp_err_t button_controller_start(button_handle_t handle)
{
    if (handle == NULL || !handle->is_initialized)
    {
        ESP_LOGE(BUTTON_TAG, "Invalid or uninitialized controller handle\n");
        return ESP_ERR_INVALID_ARG;
    }

    if (handle->is_running)
    {
        ESP_LOGE(BUTTON_TAG, "Controller already running for GPIO %d\n", handle->config.gpio_pin);
        return ESP_OK;
    }

    // Create processing task
    char task_name[32];
    snprintf(task_name, sizeof(task_name), "btn_task_%d", handle->config.gpio_pin);

    handle->is_running = true;
    BaseType_t task_create_ret = xTaskCreate(button_event_processing_task, task_name,
                                             BUTTON_TASK_STACK_SIZE, (void *)handle,
                                             BUTTON_TASK_PRIORITY, &handle->task_handle);

    if (task_create_ret != pdPASS)
    {
        ESP_LOGE(BUTTON_TAG, "Failed to create button task for GPIO %d\n", handle->config.gpio_pin);
        vQueueDelete(handle->event_queue);
        vSemaphoreDelete(handle->state_mutex);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(task_name, "Created button task for %s with handle %p\n",
             handle->config.controller_name, handle->task_handle);

    // Add ISR handler
    esp_err_t ret = gpio_isr_handler_add(handle->config.gpio_pin,
                                         button_gpio_isr_handler,
                                         (void *)(uintptr_t)handle->config.gpio_pin);
    if (ret != ESP_OK)
    {
        ESP_LOGE(BUTTON_TAG, "Failed to add ISR handler for GPIO %d: %s\n",
                 handle->config.gpio_pin, esp_err_to_name(ret));
        vTaskDelete(handle->task_handle);
        handle->task_handle = NULL;
        return ret;
    }

    // Reset statistics
    memset(&handle->stats, 0, sizeof(button_stats_t));

    ESP_LOGI(BUTTON_TAG, "Button controller started for %s\n", handle->config.controller_name);
    return ESP_OK;
}

/**
 * @brief Stop button monitoring - UPDATED VERSION
 * Properly stops all timers before stopping the controller
 */
esp_err_t button_controller_stop(button_handle_t handle)
{
    if (handle == NULL || !handle->is_initialized)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!handle->is_running)
    {
        ESP_LOGW(BUTTON_TAG, "Controller already stopped for GPIO %d\n", handle->config.gpio_pin);
        return ESP_OK;
    }

    ESP_LOGI(BUTTON_TAG, "Stopping button controller for GPIO %d\n", handle->config.gpio_pin);

    // Remove ISR handler first to stop new interrupts
    gpio_isr_handler_remove(handle->config.gpio_pin);

    // Stop ALL timers (debounce, long press, hold repeat, AND click timeout)
    if (handle->debounce_timer && xTimerIsTimerActive(handle->debounce_timer))
    {
        xTimerStop(handle->debounce_timer, portMAX_DELAY);
    }
    if (handle->long_press_timer && xTimerIsTimerActive(handle->long_press_timer))
    {
        xTimerStop(handle->long_press_timer, portMAX_DELAY);
    }
    if (handle->hold_repeat_timer && xTimerIsTimerActive(handle->hold_repeat_timer))
    {
        xTimerStop(handle->hold_repeat_timer, portMAX_DELAY);
    }
    if (handle->click_timeout_timer && xTimerIsTimerActive(handle->click_timeout_timer))
    {
        xTimerStop(handle->click_timeout_timer, portMAX_DELAY);
    }

    // Signal task to stop
    handle->is_running = false;

    // Wait for task to finish (it deletes itself)
    if (handle->task_handle)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        handle->task_handle = NULL;
    }

    // Reset state
    handle->current_state = BUTTON_STATE_IDLE;
    handle->consecutive_clicks = 0;
    handle->waiting_for_timeout = false;

    ESP_LOGI(BUTTON_TAG, "Button controller stopped for GPIO %d\n", handle->config.gpio_pin);
    return ESP_OK;
}

/**
 * @brief Register event callback
 */
esp_err_t button_controller_register_event_callback(button_handle_t handle,
                                                    button_event_callback_t callback,
                                                    void *user_data)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(handle->state_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        handle->event_callback = callback;
        handle->user_data = user_data;
        xSemaphoreGive(handle->state_mutex);

        ESP_LOGI(BUTTON_TAG, "Event callback registered for GPIO %d\n", handle->config.gpio_pin);
        return ESP_OK;
    }

    ESP_LOGE(BUTTON_TAG, "Failed to acquire mutex for callback registration\n");
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Register error callback
 */
esp_err_t button_controller_register_error_callback(button_handle_t handle,
                                                    button_error_callback_t callback,
                                                    void *user_data)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(handle->state_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        handle->error_callback = callback;
        if (user_data != NULL)
        {
            handle->user_data = user_data;
        }
        xSemaphoreGive(handle->state_mutex);

        ESP_LOGI(BUTTON_TAG, "Error callback registered for GPIO %d\n", handle->config.gpio_pin);
        return ESP_OK;
    }

    ESP_LOGE(BUTTON_TAG, "Failed to acquire mutex for error callback registration\n");
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Get controller statistics
 */
esp_err_t button_controller_get_stats(button_handle_t handle, button_stats_t *stats)
{
    if (handle == NULL || stats == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *stats = handle->stats;
    return ESP_OK;
}

/**
 * @brief Reset controller statistics
 */
esp_err_t button_controller_reset_stats(button_handle_t handle)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&handle->stats, 0, sizeof(button_stats_t));
    ESP_LOGI(BUTTON_TAG, "Statistics reset for GPIO %d\n", handle->config.gpio_pin);
    return ESP_OK;
}

/**
 * @brief Get current button state
 */
esp_err_t button_controller_get_state(button_handle_t handle, button_state_t *state)
{
    if (handle == NULL || state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *state = handle->current_state;
    return ESP_OK;
}

/**
 * @brief Check if button is currently pressed
 */
esp_err_t button_controller_is_pressed(button_handle_t handle, bool *is_pressed)
{
    if (handle == NULL || is_pressed == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!handle->is_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    int gpio_level = gpio_get_level(handle->config.gpio_pin);
    *is_pressed = handle->config.active_low ? (gpio_level == 0) : (gpio_level == 1);

    return ESP_OK;
}

/**
 * @brief Example error handler
 */
static void button_error_handler(esp_err_t error_code, const char *error_msg, void *user_data)
{
    const char *button_name = (const char *)user_data;
    const char *APP_TAG = "ButtonError";
    ESP_LOGE(APP_TAG, "❌ BUTTON ERROR [%s]: %s (Code: %s)\n",
             button_name ? button_name : "UNKNOWN",
             error_msg,
             esp_err_to_name(error_code));

    // Error recovery strategies could be implemented here
    // - Restart specific controller
    // - Log error to NVS
    // - Trigger system reset if critical
    // - Notify user via LED/display
    // Handle error - restart controller, notify user, log to flash, etc.
    // This is where you could implement error recovery strategies
}