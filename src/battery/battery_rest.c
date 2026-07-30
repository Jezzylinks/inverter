#include "battery_rest.h"

#include <math.h>

void battery_rest_init(battery_rest_t *detector,
                       float current_threshold,
                       uint32_t rest_time_seconds)
{
    if (detector == NULL)
    {
        return;
    }

    detector->current_threshold = fabsf(current_threshold);
    detector->rest_time_seconds = rest_time_seconds;

    detector->accumulated_seconds = 0;
    detector->resting = false;
}

void battery_rest_reset(battery_rest_t *detector)
{
    if (detector == NULL)
    {
        return;
    }

    detector->accumulated_seconds = 0;
    detector->resting = false;
}

bool battery_rest_update(battery_rest_t *detector,
                         float battery_current,
                         uint32_t dt_seconds)
{
    if (detector == NULL)
    {
        return false;
    }

    /* Battery is considered active if charging or discharging above threshold */
    if (fabsf(battery_current) <= detector->current_threshold)
    {
        if (detector->accumulated_seconds < detector->rest_time_seconds)
        {
            detector->accumulated_seconds += dt_seconds;

            if (detector->accumulated_seconds > detector->rest_time_seconds)
            {
                detector->accumulated_seconds =
                    detector->rest_time_seconds;
            }
        }
    }
    else
    {
        detector->accumulated_seconds = 0;
        detector->resting = false;
        return false;
    }

    detector->resting =
        (detector->accumulated_seconds >= detector->rest_time_seconds);

    return detector->resting;
}

bool battery_is_resting(const battery_rest_t *detector)
{
    if (detector == NULL)
    {
        return false;
    }

    return detector->resting;
}