#ifndef EVENT_ROUTES_H
#define EVENT_ROUTES_H

#include <stdbool.h>

#include "system_events.h"
#include "event_dispatcher.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /******************************************************************************
     * Maximum subscribers per route
     ******************************************************************************/

#define EVENT_ROUTE_MAX_SUBSCRIBERS 8

    /******************************************************************************
     * Event Route
     ******************************************************************************/

    typedef struct
    {
        event_category_t category;

        event_action_t action;

        protection_quantity_t quantity;

        uint8_t subscriber_count;

        event_subscriber_t subscribers[EVENT_ROUTE_MAX_SUBSCRIBERS];

    } event_route_t;

    /******************************************************************************
     * API
     ******************************************************************************/

    const event_route_t *event_route_find(const system_event_t *event);

    void event_route_dispatch(const system_event_t *event);

#ifdef __cplusplus
}
#endif

#endif