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

#define EVENT_SUBSCRIBER_QUEUE_LENGTH 10
#define EVENT_ROUTE_MAX_SUBSCRIBERS 8

    typedef enum
    {
        EVENT_SUB_LCD = 0,
        EVENT_SUB_RELAY,
        EVENT_SUB_LED,
        EVENT_SUB_BUZZER,
        EVENT_SUB_LOGGER,
        EVENT_SUB_FAULT_LOG,
        EVENT_SUB_WIFI,
        EVENT_SUB_MONITOR,
        EVENT_SUB_BUTTON,
        EVENT_SUB_SYSTEM,
        EVENT_SUB_COUNT
    } event_subscriber_t;

    typedef struct
    {
        event_category_t category;

        event_action_t action;

        protection_quantity_t quantity;

        uint8_t subscriber_count;

        event_subscriber_t subscribers[EVENT_ROUTE_MAX_SUBSCRIBERS];

    } event_route_t;

    extern QueueHandle_t g_event_subscriber_queue[EVENT_SUB_COUNT];

    /******************************************************************************
     * API
     ******************************************************************************/

    const event_route_t *event_route_find(const system_event_t *event);

    void event_route_dispatch(const system_event_t *event);
    bool event_dispatcher_init(void);

    void event_dispatcher_task(void *pvParameters);

    bool event_dispatcher_send(event_subscriber_t subscriber,
                               const system_event_t *event);

    bool event_dispatcher_receive(event_subscriber_t subscriber,
                                  system_event_t *event,
                                  TickType_t timeout);
    void event_dispatcher_task(void *pv);
    void buzzer_event_task(void *pv);
    void led_event_task(void *pv);
    void monitor_event_task(void *pv);
    void fault_log_event_task(void *pv);

#ifdef __cplusplus
}
#endif

#endif