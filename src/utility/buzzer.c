#include "buzzer.h"

#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "system_state.h"

void buzzer_init(void)
{
    ledc_timer_config_t timer =
        {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_13_BIT,
            .timer_num = LEDC_TIMER_1,
            .freq_hz = 2000,
            .clk_cfg = LEDC_AUTO_CLK};

    ledc_timer_config(&timer);

    ledc_channel_config_t channel =
        {
            .gpio_num = GPIO_BUZZER,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_1,
            .duty = 0,
            .hpoint = 0};

    ledc_channel_config(&channel);
}

#include "system_state.h"

extern system_state_t sys_state;

void update_buzzer(uint16_t freq_hz,
                   uint8_t volume_percent)
{
    static uint16_t last_freq = 0;

    if (!sys_state.sound_enabled)
    {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        return;
    }

    if (freq_hz == 0 || volume_percent == 0)
    {
        ledc_stop(
            LEDC_LOW_SPEED_MODE,
            LEDC_CHANNEL_0,
            0);

        return;
    }

    if (freq_hz != last_freq)
    {
        ledc_set_freq(
            LEDC_LOW_SPEED_MODE,
            LEDC_TIMER_1,
            freq_hz);

        last_freq = freq_hz;
    }

    if (volume_percent > 100)
        volume_percent = 100;

    uint32_t duty =
        ((1 << 13) - 1) * volume_percent / 100;

    ledc_set_duty(
        LEDC_LOW_SPEED_MODE,
        LEDC_CHANNEL_0,
        duty);

    ledc_update_duty(
        LEDC_LOW_SPEED_MODE,
        LEDC_CHANNEL_0);
}

void buzzer_off(void)
{
    update_buzzer(0, 0);
}

void buzzer_beep(uint16_t freq,
                 uint8_t volume,
                 uint32_t duration)
{
    update_buzzer(freq, volume);

    vTaskDelay(pdMS_TO_TICKS(duration));

    buzzer_off();
}

void buzzer_alert(void)
{
    buzzer_beep(2000, 50, 100);

    vTaskDelay(pdMS_TO_TICKS(100));

    buzzer_beep(2000, 50, 100);
}

void buzzer_error(void)
{
    buzzer_beep(500, 70, 500);
}

void buzzer_success(void)
{
    buzzer_beep(2500, 40, 200);
}