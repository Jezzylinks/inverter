#include "diagnostics/stack_report.h"

#include <stddef.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "STACK_REPORT";

/* Application tasks plus the ESP-IDF system tasks that also live in the heap.
 * Tasks that do not currently exist (one-shot or on-demand) are skipped. */
static const char *const kTaskNames[] = {
    "main", "lcd_task", "lcd_event_rx", "dispatcher", "buzzer_evt", "led_evt",
    "logger_evt", "monitor_evt", "prot_evt", "button_task", "adc_task",
    "adc_ui", "Display Timeout", "watchdog_supervisor", "settings_save",
    "ota_check", "ota_manifest_check", "ota_task", "wifi_toggle",
    "wifi_scan_ui", "wifi_prereq", "wifi_op_watch", "wifi_bg_start",
    "wifi_menu_refresh", "wifi_monitor", "wifi_reconnect", "wifi_saved",
    "dns_server", "net_services_sync", "cloud_report", "httpd", "mdns",
    "sys_evt", "wifi", "tiT", "Tmr Svc", "esp_timer", "ipc0", "ipc1",
};

void stack_report_log(const char *stage)
{
    ESP_LOGI(TAG, "---- stack report (%s) ----", stage ? stage : "?");
    uint32_t total_unused = 0U;
    unsigned tasks_seen = 0U;
    for (size_t i = 0U; i < sizeof(kTaskNames) / sizeof(kTaskNames[0]); ++i) {
        /* xTaskGetHandle asserts when the name length reaches the configured
         * maximum (which includes the NUL terminator). Some descriptive
         * labels in this list cannot be legal FreeRTOS task names. */
        if (strlen(kTaskNames[i]) >= configMAX_TASK_NAME_LEN) {
            ESP_LOGW(TAG, "Skipping stack lookup for overlong task name: %s",
                     kTaskNames[i]);
            continue;
        }

        const TaskHandle_t handle = xTaskGetHandle(kTaskNames[i]);
        if (handle == NULL) {
            continue;
        }
        /* Bytes on ESP-IDF. Smallest amount of stack that was ever free. */
        const uint32_t min_free = (uint32_t)uxTaskGetStackHighWaterMark(handle);
        total_unused += min_free;
        ++tasks_seen;
        ESP_LOGI(TAG, "STACK %-20s never_used=%u bytes", kTaskNames[i],
                 (unsigned)min_free);
    }
    ESP_LOGI(TAG,
             "tasks=%u total_never_used=%u bytes | heap free=%u largest=%u "
             "min_ever=%u",
             tasks_seen, (unsigned)total_unused,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)esp_get_minimum_free_heap_size());
}
