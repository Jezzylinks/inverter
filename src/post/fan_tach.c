#include "fan_tach.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

/*----------------------------------------------------------
 * Private Definitions
 *---------------------------------------------------------*/

#define TAG "fan_tach"

/*----------------------------------------------------------
 * Driver Context
 *---------------------------------------------------------*/

typedef struct
{
    fan_tach_config_t config;

    gptimer_handle_t timer;

    fan_tach_state_t state;

    volatile uint64_t timestamp[FAN_TACH_HISTORY_SIZE];

    volatile uint8_t sample_count;

    portMUX_TYPE lock;

} fan_tach_ctx_t;

static fan_tach_ctx_t s_ctx =
    {
        .lock = portMUX_INITIALIZER_UNLOCKED,
};

/*----------------------------------------------------------
 * Private Functions
 *---------------------------------------------------------*/

static void fan_tach_buffer_push(uint64_t value)
{
    portENTER_CRITICAL_ISR(&s_ctx.lock);

    if (s_ctx.sample_count < FAN_TACH_HISTORY_SIZE)
    {
        s_ctx.timestamp[s_ctx.sample_count++] = value;
    }
    else
    {
        /*
         * Buffer full.
         * Remove the oldest timestamp.
         */

        memmove(
            (void *)&s_ctx.timestamp[0],
            (const void *)&s_ctx.timestamp[1],
            sizeof(uint64_t) *
                (FAN_TACH_HISTORY_SIZE - 1));

        s_ctx.timestamp[FAN_TACH_HISTORY_SIZE - 1] = value;
    }

    portEXIT_CRITICAL_ISR(&s_ctx.lock);
}

/*----------------------------------------------------------
 * GPIO Interrupt
 *---------------------------------------------------------*/

static void IRAM_ATTR fan_tach_gpio_isr(void *arg)
{
    uint64_t counter = 0;

    /*
     * Read current GPTimer count.
     */

    gptimer_get_raw_count(
        s_ctx.timer,
        &counter);

    fan_tach_buffer_push(counter);
}

/*----------------------------------------------------------
 * GPTimer Initialization
 *---------------------------------------------------------*/

static esp_err_t fan_tach_timer_init(void)
{
    gptimer_config_t timer_cfg =
        {
            .clk_src =
                GPTIMER_CLK_SRC_DEFAULT,

            .direction =
                GPTIMER_COUNT_UP,

            .resolution_hz =
                s_ctx.config.timer_resolution_hz,
        };

    ESP_RETURN_ON_ERROR(
        gptimer_new_timer(
            &timer_cfg,
            &s_ctx.timer),
        TAG,
        "Failed to create GPTimer");

    ESP_RETURN_ON_ERROR(
        gptimer_enable(
            s_ctx.timer),
        TAG,
        "Failed to enable GPTimer");

    return ESP_OK;
}

/*----------------------------------------------------------
 * GPIO Initialization
 *---------------------------------------------------------*/

static esp_err_t fan_tach_gpio_init(void)
{
    gpio_config_t io =
        {
            .pin_bit_mask =
                (1ULL << s_ctx.config.tach_gpio),

            .mode =
                GPIO_MODE_INPUT,

            .pull_up_en =
                s_ctx.config.pullup_enable,

            .pull_down_en =
                s_ctx.config.pulldown_enable,

            .intr_type =
                s_ctx.config.interrupt_type,
        };

    ESP_RETURN_ON_ERROR(
        gpio_config(&io),
        TAG,
        "GPIO configuration failed");

    esp_err_t err =
        gpio_install_isr_service(
            ESP_INTR_FLAG_IRAM);

    /*
     * Ignore if already installed.
     */

    if ((err != ESP_OK) &&
        (err != ESP_ERR_INVALID_STATE))
    {
        return err;
    }

    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            s_ctx.config.tach_gpio,
            fan_tach_gpio_isr,
            NULL),
        TAG,
        "Failed to register ISR");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Public Functions
 *---------------------------------------------------------*/

esp_err_t fan_tach_init(
    const fan_tach_config_t *config)
{
    ESP_RETURN_ON_FALSE(
        config != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Invalid configuration");

    memset(&s_ctx, 0, sizeof(s_ctx));

    s_ctx.lock =
        (portMUX_TYPE)
            portMUX_INITIALIZER_UNLOCKED;

    memcpy(
        &s_ctx.config,
        config,
        sizeof(fan_tach_config_t));

    ESP_RETURN_ON_ERROR(
        fan_tach_timer_init(),
        TAG,
        "Timer initialization failed");

    ESP_RETURN_ON_ERROR(
        fan_tach_gpio_init(),
        TAG,
        "GPIO initialization failed");

    s_ctx.state =
        FAN_TACH_STATE_STOPPED;

    ESP_LOGI(
        TAG,
        "Fan tach initialized");

    return ESP_OK;
}

esp_err_t fan_tach_start(void)
{
    ESP_RETURN_ON_FALSE(
        s_ctx.state ==
            FAN_TACH_STATE_STOPPED,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Driver not stopped");

    ESP_RETURN_ON_ERROR(
        gptimer_start(
            s_ctx.timer),
        TAG,
        "Failed to start GPTimer");

    s_ctx.state =
        FAN_TACH_STATE_RUNNING;

    return ESP_OK;
}

esp_err_t fan_tach_stop(void)
{
    ESP_RETURN_ON_FALSE(
        s_ctx.state ==
            FAN_TACH_STATE_RUNNING,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Driver not running");

    ESP_RETURN_ON_ERROR(
        gptimer_stop(
            s_ctx.timer),
        TAG,
        "Failed to stop GPTimer");

    s_ctx.state =
        FAN_TACH_STATE_STOPPED;

    return ESP_OK;
}

void fan_tach_reset(void)
{
    portENTER_CRITICAL(&s_ctx.lock);

    memset(
        (void *)s_ctx.timestamp,
        0,
        sizeof(s_ctx.timestamp));

    s_ctx.sample_count = 0;

    portEXIT_CRITICAL(&s_ctx.lock);
}

/*----------------------------------------------------------
 * Public Functions
 *---------------------------------------------------------*/

bool fan_tach_is_ready(void)
{
    bool ready;

    portENTER_CRITICAL(&s_ctx.lock);

    ready = (s_ctx.sample_count >= 2U);

    portEXIT_CRITICAL(&s_ctx.lock);

    return ready;
}

uint8_t fan_tach_get_sample_count(void)
{
    uint8_t count;

    portENTER_CRITICAL(&s_ctx.lock);

    count = s_ctx.sample_count;

    portEXIT_CRITICAL(&s_ctx.lock);

    return count;
}

uint32_t fan_tach_get_timer_resolution(void)
{
    return s_ctx.config.timer_resolution_hz;
}

uint32_t fan_tach_get_ppr(void)
{
    return s_ctx.config.pulses_per_revolution;
}

fan_tach_state_t fan_tach_get_state(void)
{
    return s_ctx.state;
}

uint64_t fan_tach_get_period_us(void)
{
    uint64_t samples[FAN_TACH_HISTORY_SIZE];
    uint8_t count;

    portENTER_CRITICAL(&s_ctx.lock);

    count = s_ctx.sample_count;

    memcpy(samples,
           (const void *)s_ctx.timestamp,
           sizeof(samples));

    portEXIT_CRITICAL(&s_ctx.lock);

    if (count < 2U)
    {
        return 0;
    }

    uint64_t total_period = 0;

    for (uint8_t i = 1; i < count; i++)
    {
        total_period +=
            (samples[i] - samples[i - 1]);
    }

    return total_period / (count - 1U);
}

uint32_t fan_tach_get_rpm(void)
{
    uint64_t period;

    period = fan_tach_get_period_us();

    if (period == 0)
    {
        return 0;
    }

    return (uint32_t)((60ULL *
                       s_ctx.config.timer_resolution_hz) /

                      (period *
                       s_ctx.config.pulses_per_revolution));
}

bool fan_tach_is_alive(void)
{
    uint64_t now;
    uint64_t last;

    portENTER_CRITICAL(&s_ctx.lock);

    if (s_ctx.sample_count == 0U)
    {
        portEXIT_CRITICAL(&s_ctx.lock);

        return false;
    }

    last =
        s_ctx.timestamp[s_ctx.sample_count - 1U];

    portEXIT_CRITICAL(&s_ctx.lock);

    if (gptimer_get_raw_count(
            s_ctx.timer,
            &now) != ESP_OK)
    {
        return false;
    }

    return ((now - last) <=
            s_ctx.config.timeout_us);
}

esp_err_t fan_tach_deinit(void)
{
    if (s_ctx.timer != NULL)
    {
        if (s_ctx.state ==
            FAN_TACH_STATE_RUNNING)
        {
            ESP_RETURN_ON_ERROR(
                gptimer_stop(
                    s_ctx.timer),
                TAG,
                "Failed to stop timer");
        }

        ESP_RETURN_ON_ERROR(
            gptimer_disable(
                s_ctx.timer),
            TAG,
            "Failed to disable timer");

        ESP_RETURN_ON_ERROR(
            gptimer_del_timer(
                s_ctx.timer),
            TAG,
            "Failed to delete timer");

        s_ctx.timer = NULL;
    }

    gpio_isr_handler_remove(
        s_ctx.config.tach_gpio);

    fan_tach_reset();

    s_ctx.state =
        FAN_TACH_STATE_UNINITIALIZED;

    ESP_LOGI(TAG,
             "Fan tach deinitialized");

    return ESP_OK;
}