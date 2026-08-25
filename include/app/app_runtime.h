#ifndef APP_RUNTIME_H
#define APP_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "post/post_manager.h"
#include "system/system_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared synchronization and startup state used by existing subsystems. */
extern EventGroupHandle_t sys_event_group;
extern SemaphoreHandle_t sys_state_mutex;
extern SemaphoreHandle_t change_pin_mutex;
extern TaskHandle_t lcd_task_handle;
extern system_state_t sys_state;

/* ADC warm-up notification consumed by the startup coordinator. */
#define APP_EVENT_ADC_READY (1U << 0)

/* Existing lifecycle entry points retained behind one application boundary. */
void init_watchdog(bool enable_task_wdt, bool panic_on_hang);
void nvs_init(bool erase_on_fail);
bool nvs_is_initialized(void);
void nvs_print_stats(void);
void init_system_state(void);
void init_menu_system(void);
void init_hardware(void);
void restore_from_deep_sleep(void);
void lcd_power_init(void);
void LCD_power(bool enable);
void lcd_set_brightness(uint8_t brightness);
void adc_task(void *arg);
void lcd_task(void *arg);
void inverter_emergency_shutdown(void);
void log_all_error_flags(uint32_t flags);
void update_lcd_activity_state(void);
void handle_menu_timeout(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_RUNTIME_H */
