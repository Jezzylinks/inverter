#include "app_buttons.h"

#include <string.h>

#include "app_input.h"
#include "button_controller.h"
#include "esp_log.h"
#include "hardware_config.h"

#define APP_BUTTONS_TAG "APP_BUTTONS"
#define APP_BUTTON_DEBOUNCE_MS 50U
#define APP_BUTTON_LONG_PRESS_MS 1000U
#define APP_BUTTON_DOUBLE_CLICK_MS 400U
#define APP_BUTTON_REPEAT_MS 100U

typedef struct {
    gpio_num_t gpio;
    const char *name;
    button_event_callback_t callback;
    bool multi_click;
    button_handle_t handle;
} app_button_binding_t;

static app_button_binding_t s_bindings[] = {
    {GPIO_BUTTON_POWER, "Power", handle_power_button_event, true, NULL},
    {GPIO_BUTTON_ENTER_MENU, "Enter/Menu", handle_enter_menu_button_event, true, NULL},
    {GPIO_BUTTON_UP, "Up", handle_up_button_event, false, NULL},
    {GPIO_BUTTON_DOWN, "Down", handle_down_button_event, false, NULL},
    {GPIO_BUTTON_BACK, "Back", handle_back_button_event, false, NULL},
};

static void destroy_binding(app_button_binding_t *binding)
{
    if (!binding || !binding->handle) {
        return;
    }
    (void)button_controller_stop(binding->handle);
    (void)button_controller_destroy(binding->handle);
    binding->handle = NULL;
}

esp_err_t app_buttons_init(void)
{
    esp_err_t err = button_controller_init();
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < sizeof(s_bindings) / sizeof(s_bindings[0]); ++i) {
        app_button_binding_t *binding = &s_bindings[i];
        button_config_t config;
        button_controller_get_default_config(&config);
        config.gpio_pin = binding->gpio;
        config.debounce_ms = APP_BUTTON_DEBOUNCE_MS;
        config.long_press_ms = APP_BUTTON_LONG_PRESS_MS;
        config.double_click_ms = APP_BUTTON_DOUBLE_CLICK_MS;
        config.hold_repeat_ms = APP_BUTTON_REPEAT_MS;
        config.active_low = true;
        config.enable_pullup = true;
        config.enable_multi_click = binding->multi_click;
        config.controller_name = binding->name;

        err = button_controller_create(&config, &binding->handle);
        if (err != ESP_OK) {
            ESP_LOGE(APP_BUTTONS_TAG, "%s create failed: %s", binding->name,
                     esp_err_to_name(err));
            goto fail;
        }
        err = button_controller_register_event_callback(binding->handle,
                                                        binding->callback,
                                                        NULL);
        if (err != ESP_OK) {
            ESP_LOGE(APP_BUTTONS_TAG, "%s callback failed: %s", binding->name,
                     esp_err_to_name(err));
            goto fail;
        }
        err = button_controller_start(binding->handle);
        if (err != ESP_OK) {
            ESP_LOGE(APP_BUTTONS_TAG, "%s start failed: %s", binding->name,
                     esp_err_to_name(err));
            goto fail;
        }
    }

    ESP_LOGI(APP_BUTTONS_TAG, "All five buttons registered on one shared task");
    return ESP_OK;

fail:
    app_buttons_deinit();
    return err;
}

void app_buttons_deinit(void)
{
    for (size_t i = 0; i < sizeof(s_bindings) / sizeof(s_bindings[0]); ++i) {
        destroy_binding(&s_bindings[i]);
    }
    (void)button_controller_deinit();
}

void app_buttons_reset_statistics(void)
{
    for (size_t i = 0; i < sizeof(s_bindings) / sizeof(s_bindings[0]); ++i) {
        if (s_bindings[i].handle) {
            (void)button_controller_reset_stats(s_bindings[i].handle);
        }
    }
}
