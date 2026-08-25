#include "post/post_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "post/post_adc.h"
#include "post/post_fan.h"
#include "post/post_lcd.h"

static const char *TAG = "POST_MANAGER";
static post_result_t s_last_result;

post_result_t post_run_all(void)
{
    post_result_t result = {0};
    const int64_t start_us = esp_timer_get_time();

    ESP_LOGI(TAG, "=== Running Power-On Self-Test ===");
    result.lcd_ok = post_lcd_test();
    if (!result.lcd_ok) {
        result.failure_mask |= POST_FAILURE_LCD;
    }
    ESP_LOGI(TAG, "LCD: %s", result.lcd_ok ? "PASS" : "FAIL");

    result.adc_ok = post_adc_test();
    if (!result.adc_ok) {
        result.failure_mask |= POST_FAILURE_ADC;
    }
    ESP_LOGI(TAG, "ADC: %s", result.adc_ok ? "PASS" : "FAIL");

    result.fan_ok = (post_fan_test() == POST_FAN_RESULT_PASS);
    if (!result.fan_ok) {
        result.failure_mask |= POST_FAILURE_FAN;
    }
    ESP_LOGI(TAG, "Fan: %s", result.fan_ok ? "PASS" : "FAIL");

    result.all_passed = result.failure_mask == 0U;
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    result.duration_ms = (elapsed_us > 0) ? (uint32_t)(elapsed_us / 1000) : 0U;
    s_last_result = result;

    ESP_LOGI(TAG, "=== POST %s (failures=0x%02lx, %lums) ===",
             result.all_passed ? "PASSED" : "FAILED",
             (unsigned long)result.failure_mask,
             (unsigned long)result.duration_ms);
    return result;
}

post_result_t post_get_last_result(void)
{
    return s_last_result;
}
