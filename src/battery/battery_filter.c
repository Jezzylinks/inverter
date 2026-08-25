#include "battery/battery_filter.h"
#include "stdlib.h"

#define BATTERY_FILTER_MIN_ALPHA (0.01f)
#define BATTERY_FILTER_MAX_ALPHA (1.00f)

void battery_filter_init(battery_filter_t *filter,
                         float alpha)
{
    if (filter == NULL)
    {
        return;
    }

    if (alpha < BATTERY_FILTER_MIN_ALPHA)
    {
        alpha = BATTERY_FILTER_MIN_ALPHA;
    }

    if (alpha > BATTERY_FILTER_MAX_ALPHA)
    {
        alpha = BATTERY_FILTER_MAX_ALPHA;
    }

    filter->alpha = alpha;
    filter->filtered = 0.0f;
    filter->initialized = false;
}

void battery_filter_reset(battery_filter_t *filter)
{
    if (filter == NULL)
    {
        return;
    }

    filter->filtered = 0.0f;
    filter->initialized = false;
}

float battery_filter_update(battery_filter_t *filter,
                            float sample)
{
    if (filter == NULL)
    {
        return sample;
    }

    /* First sample initializes the filter */
    if (!filter->initialized)
    {
        filter->filtered = sample;
        filter->initialized = true;

        return sample;
    }

    filter->filtered +=
        filter->alpha *
        (sample - filter->filtered);

    return filter->filtered;
}

float battery_filter_get(const battery_filter_t *filter)
{
    if (filter == NULL)
    {
        return 0.0f;
    }

    return filter->filtered;
}