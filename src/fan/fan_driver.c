#include "fan_driver.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "hardware_config.h"
#include "esp_err.h"

static const char *TAG = "FAN_DRIVER";

/*----------------------------------------------------------
 * PWM (speed control, GPIO_FAN / Green wire)
 *---------------------------------------------------------*/
#define FAN_LEDC_MODE LEDC_LOW_SPEED_MODE
#define FAN_LEDC_TIMER LEDC_TIMER_1 /* deliberately NOT LEDC_TIMER_0 --  \
                                     * buzzer and the status/error LEDs  \
                                     * already share that timer, and the \
                                     * fan needs a stable, independent   \
                                     * PWM frequency that doesn't shift  \
                                     * every time a tone or brightness   \
                                     * changes elsewhere. */
#define FAN_LEDC_CHANNEL LEDC_CHANNEL_3
#define FAN_LEDC_RES LEDC_TIMER_10_BIT

/*----------------------------------------------------------
 * Tachometer (feedback, GPIO_FAN_TACH / Yellow wire)
 *---------------------------------------------------------*/

/* Reject pulses closer together than this -- contact bounce / electrical
 * noise, not a real edge. Corresponds to ~20,000 RPM at 2 PPR, well
 * above anything a real cooling fan spins at. */
#define FAN_TACH_MIN_PERIOD_US 1500

/* If no pulse has arrived in this long, treat the fan as stopped/stalled
 * rather than reporting a stale RPM from whenever it was last spinning. */
#define FAN_TACH_STALE_US 500000 /* 500 ms */

static volatile int64_t s_last_edge_time_us = 0;
static volatile uint32_t s_last_period_us = 0;

static void IRAM_ATTR fan_tach_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    int64_t period = now - s_last_edge_time_us;

    if (period >= FAN_TACH_MIN_PERIOD_US)
    {
        s_last_period_us = (uint32_t)period;
    }
    /* else: bounce/noise -- keep the previous period, just don't update
     * the edge timestamp from this spurious edge either, so a burst of
     * bounce doesn't repeatedly reset the window. */
    else
    {
        return;
    }

    s_last_edge_time_us = now;
}

/*----------------------------------------------------------
 * Public functions
 *---------------------------------------------------------*/

void fan_driver_init(void)
{
    /* PWM output */
    ledc_timer_config_t timer = {
        .speed_mode = FAN_LEDC_MODE,
        .timer_num = FAN_LEDC_TIMER,
        .duty_resolution = FAN_LEDC_RES,
        .freq_hz = FAN_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .gpio_num = GPIO_FAN,
        .speed_mode = FAN_LEDC_MODE,
        .channel = FAN_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = FAN_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));

    /* Tachometer input */
    gpio_config_t tach_io = {
        .pin_bit_mask = 1ULL << GPIO_FAN_TACH,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, /* belt-and-braces alongside the
                                           * external 4.7k pull-up the
                                           * open-collector tach output
                                           * needs */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&tach_io));

    /* gpio_install_isr_service() is shared/global across the whole
     * firmware -- if button_controller.c (or anything else) already
     * installed it, this returns ESP_ERR_INVALID_STATE, which is fine
     * and expected, not a real failure. */
    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(isr_svc_err);
    }

    ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO_FAN_TACH, fan_tach_isr, NULL));

    ESP_LOGI(TAG, "Fan driver ready: PWM on GPIO%d @ %dHz, tach on GPIO%d",
             GPIO_FAN, FAN_PWM_FREQ_HZ, GPIO_FAN_TACH);
}

esp_err_t fan_set_speed_percent(uint8_t percent)
{
    if (percent > 100)
    {
        percent = 100;
    }

    uint32_t max_duty = (1 << FAN_LEDC_RES) - 1;
    uint32_t duty = (max_duty * percent) / 100;

    esp_err_t err = ledc_set_duty(FAN_LEDC_MODE, FAN_LEDC_CHANNEL, duty);
    if (err != ESP_OK)
    {
        return err;
    }
    return ledc_update_duty(FAN_LEDC_MODE, FAN_LEDC_CHANNEL);
}

uint32_t fan_get_rpm(void)
{
    /* Snapshot both volatiles once -- a 32/64-bit read is atomic on this
     * architecture, but the ISR could still update between the two
     * reads if we read them separately, and that's fine here (worst
     * case is a couple of microseconds of skew between the timestamp
     * and the period it's paired with, immaterial at fan-speed
     * timescales). */
    int64_t last_edge = s_last_edge_time_us;
    uint32_t period_us = s_last_period_us;

    if (period_us == 0)
    {
        return 0; /* no pulse seen yet since boot */
    }

    if (esp_timer_get_time() - last_edge > FAN_TACH_STALE_US)
    {
        return 0; /* fan stopped/stalled -- last reading is too old to trust */
    }

    /* RPM = (1 / period_seconds) * 60 / pulses_per_rev
     *     = 60,000,000 / (period_us * PPR) */
    return (uint32_t)(60000000ULL / ((uint64_t)period_us * FAN_TACH_PULSES_PER_REV));
}
