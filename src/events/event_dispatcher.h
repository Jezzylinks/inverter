#ifndef EVENT_DISPATCHER_H
#define EVENT_DISPATCHER_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "system_events.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /******************************************************************************
     * Configuration
     ******************************************************************************/

#define EVENT_SUBSCRIBER_QUEUE_LENGTH 10

    /******************************************************************************
     * Subscribers
     ******************************************************************************/

    typedef enum
    {
        EVENT_SUB_LCD = 0,

        EVENT_SUB_RELAY,

        EVENT_SUB_BUZZER,

        EVENT_SUB_LOGGER,

        EVENT_SUB_WIFI,

        EVENT_SUB_MONITOR,

        EVENT_SUB_BUTTON,

        EVENT_SUB_SYSTEM,

        EVENT_SUB_COUNT

    } event_subscriber_t;

    /******************************************************************************
     * Queue Handles
     ******************************************************************************/

    extern QueueHandle_t g_event_subscriber_queue[EVENT_SUB_COUNT];

    /******************************************************************************
     * API
     ******************************************************************************/

    bool event_dispatcher_init(void);

    void event_dispatcher_task(void *pvParameters);

    bool event_dispatcher_send(event_subscriber_t subscriber,
                               const system_event_t *event);

    bool event_dispatcher_receive(event_subscriber_t subscriber,
                                  system_event_t *event,
                                  TickType_t timeout);

#ifdef __cplusplus
}
#endif

#endif