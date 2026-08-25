#include "lcd/lcd_event_receiver.h"

#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "events/event_dispatcher.h"
#include "lcd/lcd_flash_queue.h"
#include "lcd/lcd_config.h"
#include "lcd/lcd_writer.h"

#include "system/task_watchdog.h"
#define LCD_EVENT_RECEIVER_STACK_SIZE 3072
#define LCD_EVENT_RECEIVER_PRIORITY 3
#define LCD_EVENT_RECEIVER_WAIT_MS 250
#define LCD_EVENT_FLASH_DURATION_MS 1500

static const char *TAG = "LCD_EVENT_RX";

static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;

static flash_priority_t flash_priority_for_event(const system_event_t *event)
{
    if (event == NULL) {
        return FLASH_PRI_INFO;
    }

    switch (event->priority) {
    case EVENT_PRIORITY_CRITICAL:
        return FLASH_PRI_CRITICAL;
    case EVENT_PRIORITY_HIGH:
        return FLASH_PRI_FAULT;
    case EVENT_PRIORITY_NORMAL:
        return FLASH_PRI_WARNING;
    case EVENT_PRIORITY_LOW:
    default:
        return FLASH_PRI_INFO;
    }
}

static void display_event_notice(const system_event_t *event)
{
    if (event == NULL || event->category == EVENT_CATEGORY_BUTTON) {
        return;
    }

    /* Startup notices are intentionally quiet. Events remain available to
     * the protection/logger/relay consumers; only their LCD flash is
     * suppressed. AC and battery voltage notices are excluded for the whole
     * startup window, while unrelated critical protection events remain
     * visible. */
    const bool startup_active = lcd_is_startup_active();
    const bool is_protection = event->category == EVENT_CATEGORY_PROTECTION;
    const bool noncritical_protection =
        is_protection && event->priority < EVENT_PRIORITY_CRITICAL;
    const bool startup_voltage_protection =
        is_protection &&
        (event->quantity == PROT_QUANTITY_AC_VOLTAGE ||
         event->quantity == PROT_QUANTITY_BATTERY_VOLTAGE);
    const bool boot_complete_notice =
        event->category == EVENT_CATEGORY_SYSTEM &&
        event->action == EVENT_ACTION_STARTUP;
    if (startup_active &&
        (noncritical_protection || startup_voltage_protection ||
         boot_complete_notice)) {
        return;
    }

    const char *category = event_category_name(event->category);
    const char *action = event_action_name(event->action);
    const char *detail = action;

    if (event->category == EVENT_CATEGORY_PROTECTION) {
        detail = protection_quantity_name(event->quantity);
    }

    char row0[LCD_LINE_SIZE];
    char row1[LCD_LINE_SIZE];
    snprintf(row0, sizeof(row0), "%-*.*s", LCD_COLS, LCD_COLS,
             category ? category : "EVENT");
    snprintf(row1, sizeof(row1), "%-*.*s", LCD_COLS, LCD_COLS,
             detail ? detail : "NOTICE");

    lcd_flash_enqueue_to(row0,
                         row1,
                         LCD_EVENT_FLASH_DURATION_MS,
                         flash_priority_for_event(event),
                         LCD_FLASH_RETURN_AUTO);
}

static void lcd_event_receiver_task(void *arg)
{
    task_watchdog_register("lcd_event_receiver_task");
    (void)arg;

    while (s_running) {

        task_watchdog_feed();
        /* lcd_task creates the flash queue after it obtains its task handle. */
        if (!lcd_flash_is_initialized()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        system_event_t event = {0};
        if (event_dispatcher_receive(EVENT_SUB_LCD,
                                       &event,
                                       pdMS_TO_TICKS(LCD_EVENT_RECEIVER_WAIT_MS))) {
            /* The receiver drains the queue; only meaningful display events
             * are converted into flash messages. It never calls lcd_*(). */
            display_event_notice(&event);
        }
    }

    s_task = NULL;
    task_watchdog_unregister();
    vTaskDelete(NULL);
}

esp_err_t lcd_event_receiver_start(void)
{
    if (s_running || s_task != NULL) {
        return ESP_OK;
    }

    s_running = true;
    if (xTaskCreate(lcd_event_receiver_task,
                    "lcd_event_rx",
                    LCD_EVENT_RECEIVER_STACK_SIZE,
                    NULL,
                    LCD_EVENT_RECEIVER_PRIORITY,
                    &s_task) != pdPASS) {
        s_running = false;
        s_task = NULL;
        ESP_LOGE(TAG, "Failed to create LCD event receiver task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LCD subscriber receiver started");
    return ESP_OK;
}

esp_err_t lcd_event_receiver_stop(void)
{
    s_running = false;
    if (s_task == NULL) {
        return ESP_OK;
    }

    xTaskNotifyGive(s_task);
    for (int i = 0; s_task != NULL && i < 40; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return (s_task == NULL) ? ESP_OK : ESP_ERR_TIMEOUT;
}
