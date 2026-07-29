#include "post_manager.h"

#include "post_lcd.h"
#include "post_fan.h"
#include "post_adc.h"
#include "esp_log.h"

static const char *TAG = "POST_MANAGER";

static post_result_t s_last_result = {0};

post_result_t post_run_all(void)
{
    post_result_t result = {0};

    ESP_LOGI(TAG, "=== Running Power-On Self-Test ===");

    /* LCD first -- if this fails, nothing downstream can be shown on
     * screen, only logged. Still run the rest so a technician gets the
     * full picture from the serial log even with a dead display. */
    result.lcd_ok = post_lcd_test();
    ESP_LOGI(TAG, "LCD:  %s", result.lcd_ok ? "PASS" : "FAIL");

    result.adc_ok = post_adc_test();
    ESP_LOGI(TAG, "ADC:  %s", result.adc_ok ? "PASS" : "FAIL");

    result.fan_ok = post_fan_test();
    ESP_LOGI(TAG, "Fan:  %s", result.fan_ok ? "PASS" : "FAIL");

    result.all_passed = result.lcd_ok && result.adc_ok && result.fan_ok;

    ESP_LOGI(TAG, "=== POST %s ===", result.all_passed ? "PASSED" : "FAILED");

    s_last_result = result;
    return result;
}

post_result_t post_get_last_result(void)
{
    return s_last_result;
}
