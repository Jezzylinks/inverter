#include "telemetry/telemetry_health.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define TELEMETRY_REQUIRED_DEFAULT_MASK \
    (1UL << TELEMETRY_CHANNEL_BATTERY_VOLTAGE)

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static telemetry_health_snapshot_t s_health;
static uint32_t s_required_mask = TELEMETRY_REQUIRED_DEFAULT_MASK;

static bool channel_valid(telemetry_channel_t channel)
{
    return channel >= 0 && channel < TELEMETRY_CHANNEL_COUNT;
}

static bool age_is_fresh(uint32_t last_update_ms,
                         uint32_t now_ms,
                         uint32_t max_age_ms)
{
    if (last_update_ms == 0U) {
        return false;
    }
    return (uint32_t)(now_ms - last_update_ms) <= max_age_ms;
}

void telemetry_health_init(void)
{
    taskENTER_CRITICAL(&s_lock);
    memset(&s_health, 0, sizeof(s_health));
    s_health.initialized = true;
    s_required_mask = TELEMETRY_REQUIRED_DEFAULT_MASK;
    taskEXIT_CRITICAL(&s_lock);
}

bool telemetry_health_record(telemetry_channel_t channel,
                             float value,
                             float minimum,
                             float maximum,
                             uint32_t now_ms)
{
    if (!channel_valid(channel) || !isfinite(value) ||
        !isfinite(minimum) || !isfinite(maximum) || minimum > maximum) {
        telemetry_health_record_invalid(channel, now_ms);
        return false;
    }

    const bool valid = value >= minimum && value <= maximum;
    taskENTER_CRITICAL(&s_lock);
    telemetry_channel_snapshot_t *sample = &s_health.channel[channel];
    sample->last_value = value;
    sample->last_update_ms = now_ms;
    s_health.last_update_ms = now_ms;
    if (valid) {
        sample->valid = true;
        sample->ever_valid = true;
        sample->total_valid_samples++;
        sample->consecutive_invalid_samples = 0U;
    } else {
        sample->valid = false;
        sample->total_invalid_samples++;
        sample->consecutive_invalid_samples++;
    }
    taskEXIT_CRITICAL(&s_lock);
    return valid;
}

void telemetry_health_record_invalid(telemetry_channel_t channel,
                                     uint32_t now_ms)
{
    if (!channel_valid(channel)) {
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    telemetry_channel_snapshot_t *sample = &s_health.channel[channel];
    sample->valid = false;
    sample->last_update_ms = now_ms;
    s_health.last_update_ms = now_ms;
    sample->total_invalid_samples++;
    sample->consecutive_invalid_samples++;
    taskEXIT_CRITICAL(&s_lock);
}

void telemetry_health_set_required_mask(uint32_t required_mask)
{
    taskENTER_CRITICAL(&s_lock);
    s_required_mask = required_mask & ((1UL << TELEMETRY_CHANNEL_COUNT) - 1UL);
    taskEXIT_CRITICAL(&s_lock);
}

bool telemetry_health_channel_valid(telemetry_channel_t channel,
                                    uint32_t now_ms,
                                    uint32_t max_age_ms)
{
    if (!channel_valid(channel)) {
        return false;
    }

    bool valid;
    taskENTER_CRITICAL(&s_lock);
    const telemetry_channel_snapshot_t *sample = &s_health.channel[channel];
    valid = sample->valid && sample->ever_valid &&
            age_is_fresh(sample->last_update_ms, now_ms, max_age_ms);
    taskEXIT_CRITICAL(&s_lock);
    return valid;
}

bool telemetry_health_required_ready(uint32_t now_ms,
                                    uint32_t max_age_ms)
{
    bool ready = true;
    taskENTER_CRITICAL(&s_lock);
    if (!s_health.initialized) {
        ready = false;
    } else {
        for (uint32_t i = 0U; i < TELEMETRY_CHANNEL_COUNT; ++i) {
            if ((s_required_mask & (1UL << i)) == 0U) {
                continue;
            }
            const telemetry_channel_snapshot_t *sample = &s_health.channel[i];
            if (!sample->valid || !sample->ever_valid ||
                !age_is_fresh(sample->last_update_ms, now_ms, max_age_ms)) {
                ready = false;
                break;
            }
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return ready;
}

void telemetry_health_get_snapshot(telemetry_health_snapshot_t *out)
{
    if (!out) {
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    memcpy(out, &s_health, sizeof(*out));
    out->battery_ready = s_health.channel[TELEMETRY_CHANNEL_BATTERY_VOLTAGE].valid;
    out->required_ready = true;
    for (uint32_t i = 0U; i < TELEMETRY_CHANNEL_COUNT; ++i) {
        if ((s_required_mask & (1UL << i)) == 0U) {
            continue;
        }
        if (!s_health.channel[i].valid || !s_health.channel[i].ever_valid) {
            out->required_ready = false;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
}
