#include "task_watchdog.h"

#include "esp_log.h"
#include "esp_task_wdt.h"

static const char *TAG = "TASK_WDT";

void task_watchdog_register(const char *task_name)
{
    const esp_err_t status = esp_task_wdt_status(NULL);
    if (status == ESP_OK) {
        return;
    }

    const esp_err_t err = esp_task_wdt_add(NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Could not register %s: %s",
                 task_name ? task_name : "task", esp_err_to_name(err));
    }
}

void task_watchdog_feed(void)
{
    const esp_err_t err = esp_task_wdt_reset();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "Task watchdog feed failed: %s", esp_err_to_name(err));
    }
}
