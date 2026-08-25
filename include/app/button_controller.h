/**
 * @file button_controller.h
 * @brief Shared-task button controller for ESP32.
 *
 * GPIO ISRs only enqueue edge observations. A single controller task owns all
 * debounce, click classification, long-press, repeat, and callback dispatch.
 * This keeps timing deterministic and avoids one task/timer set per button.
 */
#ifndef BUTTON_CONTROLLER_H
#define BUTTON_CONTROLLER_H

#include "driver/gpio.h"
#include "hardware/hardware_config.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BUTTON_DEFAULT_DEBOUNCE_MS 50U
#define BUTTON_DEFAULT_LONG_PRESS_MS 2000U
#define BUTTON_DEFAULT_DOUBLE_CLICK_MS 300U
#define BUTTON_DEFAULT_HOLD_REPEAT_MS 500U
#define BUTTON_MAX_CLICK_COUNT 5U
#define BUTTON_MAX_CONTROLLERS 8U
#define BUTTON_EDGE_QUEUE_SIZE 32U
#define BUTTON_TASK_STACK_SIZE 4096U
#define BUTTON_TASK_PRIORITY 5U
#define BUTTON_TASK_POLL_INTERVAL_MS 10U
#define BUTTON_TAG "BUTTON"

typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_PRESS,
    BUTTON_EVENT_CLICK,
    BUTTON_EVENT_RELEASE,
    BUTTON_EVENT_REPEAT,
    BUTTON_EVENT_DOUBLE_CLICK,
    BUTTON_EVENT_TRIPLE_CLICK,
    BUTTON_EVENT_MULTI_CLICK,
    BUTTON_EVENT_LONG_PRESS,
    BUTTON_EVENT_MAX
} button_raw_event_type_t;

typedef enum {
    BUTTON_STATE_IDLE = 0,
    BUTTON_STATE_PRESSED,
    BUTTON_STATE_RELEASED,
    BUTTON_STATE_LONG_PRESS_ACTIVE,
    BUTTON_STATE_REPEAT_ACTIVE,
    BUTTON_STATE_MAX
} button_state_t;

typedef enum {
    BTN_POWER = 0,
    BTN_ENTER,
    BTN_UP,
    BTN_DOWN,
    BTN_BACK,
    BTN_COUNT
} button_id_t;

typedef struct {
    gpio_num_t gpio_pin;
    button_id_t button_id;
    const char *name;
} button_mapping_t;

typedef struct {
    button_raw_event_type_t event;
    button_id_t button_id;
    uint8_t click_count;
    uint8_t reserved;
    int64_t timestamp_us;
    uint32_t press_duration_ms;
} button_event_info_t;

typedef struct {
    gpio_num_t gpio_pin;
    button_id_t button_id;
    uint32_t debounce_ms;
    uint32_t long_press_ms;
    uint32_t double_click_ms;
    uint32_t hold_repeat_ms;
    bool active_low;
    bool enable_pullup;
    const char *controller_name;
    bool enable_multi_click;
} button_config_t;

typedef struct {
    _Atomic uint32_t total_events;
    _Atomic uint32_t total_presses;
    _Atomic uint32_t short_presses;
    _Atomic uint32_t long_presses;
    _Atomic uint32_t double_clicks;
    _Atomic uint32_t triple_clicks;
    _Atomic uint32_t multi_clicks;
    _Atomic uint32_t hold_repeats;
    _Atomic uint32_t isr_calls;
    _Atomic uint32_t isr_queue_full;
} button_stats_t;

typedef struct button_controller_t *button_handle_t;
typedef void (*button_event_callback_t)(button_event_info_t *event_info, void *user_data);
typedef void (*button_error_callback_t)(esp_err_t error_code, const char *error_msg, void *user_data);

esp_err_t button_controller_init(void);
esp_err_t button_controller_deinit(void);
esp_err_t button_controller_create(const button_config_t *config, button_handle_t *handle);
esp_err_t button_controller_destroy(button_handle_t handle);
esp_err_t button_controller_start(button_handle_t handle);
esp_err_t button_controller_stop(button_handle_t handle);
esp_err_t button_controller_register_event_callback(button_handle_t handle,
                                                       button_event_callback_t callback,
                                                       void *user_data);
esp_err_t button_controller_register_error_callback(button_handle_t handle,
                                                       button_error_callback_t callback,
                                                       void *user_data);
esp_err_t button_controller_get_stats(button_handle_t handle, button_stats_t *stats);
esp_err_t button_controller_reset_stats(button_handle_t handle);
esp_err_t button_controller_get_state(button_handle_t handle, button_state_t *state);
esp_err_t button_controller_is_pressed(button_handle_t handle, bool *is_pressed);
void button_controller_get_default_config(button_config_t *config);
esp_err_t button_controller_validate_config(const button_config_t *config);
const char *button_event_to_string(button_raw_event_type_t event);
const char *button_state_to_string(button_state_t state);
button_id_t gpio_to_button_id(gpio_num_t gpio_pin);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_CONTROLLER_H */
