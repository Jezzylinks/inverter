#include "post_lcd.h"

#include "lcd.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "POST_LCD";

bool post_lcd_test(void)
{
    esp_err_t err = lcd_i2c_probe();

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD did not ACK its I2C address: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}
