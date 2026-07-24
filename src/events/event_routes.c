#include "event_routes.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "EVENT_ROUTES";

/******************************************************************************
 * Route Table
 ******************************************************************************/

static const event_route_t routes[] =
    {
        /**********************************************************************
         * TEMPERATURE WARNING
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,

            .action = EVENT_ACTION_WARNING,

            .quantity = PROT_QUANTITY_TEMPERATURE,

            .subscriber_count = 3,

            .subscribers =
                {
                    EVENT_SUB_LCD,
                    EVENT_SUB_BUZZER,
                    EVENT_SUB_LOGGER}},

        /**********************************************************************
         * TEMPERATURE DERATE
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,

            .action = EVENT_ACTION_DERATE,

            .quantity = PROT_QUANTITY_TEMPERATURE,

            .subscriber_count = 4,

            .subscribers =
                {
                    EVENT_SUB_LCD,
                    EVENT_SUB_RELAY,
                    EVENT_SUB_BUZZER,
                    EVENT_SUB_LOGGER}},

        /**********************************************************************
         * TEMPERATURE SHUTDOWN
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,

            .action = EVENT_ACTION_SHUTDOWN,

            .quantity = PROT_QUANTITY_TEMPERATURE,

            .subscriber_count = 5,

            .subscribers =
                {
                    EVENT_SUB_LCD,
                    EVENT_SUB_RELAY,
                    EVENT_SUB_BUZZER,
                    EVENT_SUB_LOGGER,
                    EVENT_SUB_WIFI}},

        /**********************************************************************
         * TEMPERATURE RECOVERED
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,

            .action = EVENT_ACTION_RECOVERED,

            .quantity = PROT_QUANTITY_TEMPERATURE,

            .subscriber_count = 4,

            .subscribers =
                {
                    EVENT_SUB_LCD,
                    EVENT_SUB_RELAY,
                    EVENT_SUB_LOGGER,
                    EVENT_SUB_WIFI}},

        /**********************************************************************
         * BATTERY WARNING
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,

            .action = EVENT_ACTION_WARNING,

            .quantity = PROT_QUANTITY_BATTERY_VOLTAGE,

            .subscriber_count = 3,

            .subscribers =
                {
                    EVENT_SUB_LCD,
                    EVENT_SUB_BUZZER,
                    EVENT_SUB_LOGGER}},

        /**********************************************************************
         * BATTERY SHUTDOWN
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,

            .action = EVENT_ACTION_SHUTDOWN,

            .quantity = PROT_QUANTITY_BATTERY_VOLTAGE,

            .subscriber_count = 5,

            .subscribers =
                {
                    EVENT_SUB_LCD,
                    EVENT_SUB_RELAY,
                    EVENT_SUB_BUZZER,
                    EVENT_SUB_LOGGER,
                    EVENT_SUB_WIFI}},

        /**********************************************************************
         * AC VOLTAGE SHUTDOWN
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,

            .action = EVENT_ACTION_SHUTDOWN,

            .quantity = PROT_QUANTITY_AC_VOLTAGE,

            .subscriber_count = 5,

            .subscribers =
                {
                    EVENT_SUB_LCD,
                    EVENT_SUB_RELAY,
                    EVENT_SUB_BUZZER,
                    EVENT_SUB_LOGGER,
                    EVENT_SUB_WIFI}},

        /**********************************************************************
         * OUTPUT CURRENT SHUTDOWN
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,

            .action = EVENT_ACTION_SHUTDOWN,

            .quantity = PROT_QUANTITY_OUTPUT_CURRENT,

            .subscriber_count = 5,

            .subscribers =
                {
                    EVENT_SUB_LCD,
                    EVENT_SUB_RELAY,
                    EVENT_SUB_BUZZER,
                    EVENT_SUB_LOGGER,
                    EVENT_SUB_WIFI}}};

static const size_t route_count =
    sizeof(routes) / sizeof(routes[0]);

/******************************************************************************
 * Find Route
 ******************************************************************************/

const event_route_t *event_route_find(
    const system_event_t *event)
{
    if (event == NULL)
        return NULL;

    for (size_t i = 0; i < route_count; i++)
    {
        const event_route_t *r = &routes[i];

        if (r->category != event->category)
            continue;

        if (r->action != event->action)
            continue;

        if (r->quantity != event->quantity)
            continue;

        return r;
    }

    return NULL;
}

/******************************************************************************
 * Dispatch
 ******************************************************************************/

void event_route_dispatch(
    const system_event_t *event)
{
    const event_route_t *route =
        event_route_find(event);

    if (route == NULL)
    {
        ESP_LOGW(TAG,
                 "No route found.");
        return;
    }

    for (uint8_t i = 0;
         i < route->subscriber_count;
         i++)
    {
        event_dispatcher_send(
            route->subscribers[i],
            event);
    }
}