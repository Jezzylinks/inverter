#ifndef SYSTEM_EVENTS_H
#define SYSTEM_EVENTS_H

#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "security/protection.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /******************************************************************************
     * Configuration
     ******************************************************************************/

#define SYSTEM_EVENT_QUEUE_LENGTH 20

    /******************************************************************************
     * Event Categories
     ******************************************************************************/

    typedef enum
    {
        EVENT_CATEGORY_NONE = 0,

        EVENT_CATEGORY_PROTECTION,

        EVENT_CATEGORY_BUTTON,

        EVENT_CATEGORY_LCD,

        EVENT_CATEGORY_RELAY,

        EVENT_CATEGORY_WIFI,

        EVENT_CATEGORY_SYSTEM,

        EVENT_CATEGORY_LOG,

        EVENT_CATEGORY_USER

    } event_category_t;

    /******************************************************************************
     * Event Actions
     ******************************************************************************/

    typedef enum
    {
        EVENT_ACTION_NONE = 0,

        EVENT_ACTION_WARNING,

        EVENT_ACTION_DERATE,

        EVENT_ACTION_SHUTDOWN,

        EVENT_ACTION_RECOVERED,

        EVENT_ACTION_START,

        EVENT_ACTION_STOP,

        EVENT_ACTION_ON,

        EVENT_ACTION_OFF,

        EVENT_ACTION_PRESSED,

        EVENT_ACTION_RELEASED,

        EVENT_ACTION_LONG_PRESS,

        EVENT_ACTION_DOUBLE_PRESS,

        EVENT_ACTION_CHANGED

    } event_action_t;

    /******************************************************************************
     * Event Source
     ******************************************************************************/

    typedef enum
    {
        EVENT_SOURCE_UNKNOWN = 0,

        EVENT_SOURCE_PROTECTION,

        EVENT_SOURCE_ADC,

        EVENT_SOURCE_LCD,

        EVENT_SOURCE_BUTTON,

        EVENT_SOURCE_RELAY,

        EVENT_SOURCE_WIFI,

        EVENT_SOURCE_SYSTEM,

        EVENT_SOURCE_NVS

    } event_source_t;

    /******************************************************************************
     * Event Priority
     ******************************************************************************/

    typedef enum
    {
        EVENT_PRIORITY_LOW = 0,

        EVENT_PRIORITY_NORMAL,

        EVENT_PRIORITY_HIGH,

        EVENT_PRIORITY_CRITICAL

    } event_priority_t;

    /******************************************************************************
     * Event Structure
     ******************************************************************************/

    typedef struct
    {
        event_category_t category;

        event_action_t action;

        event_source_t source;

        event_priority_t priority;

        protection_quantity_t quantity;

        float value;

        uint32_t timestamp;

        uint32_t data;

    } system_event_t;

    /******************************************************************************
     * Global Dispatcher Queue
     ******************************************************************************/

    extern QueueHandle_t g_system_event_queue;

    /******************************************************************************
     * API
     ******************************************************************************/

    /* Initialize event system */
    bool system_events_init(void);

    /* Destroy queue */
    void system_events_deinit(void);

    /* Post event */
    bool system_event_post(const system_event_t *event);

    /* Receive event */
    bool system_event_receive(system_event_t *event,
                              TickType_t timeout);

    /******************************************************************************
     * Helper Functions
     ******************************************************************************/

    const char *event_category_name(event_category_t category);

    const char *event_action_name(event_action_t action);

    const char *event_source_name(event_source_t source);

    const char *event_priority_name(event_priority_t priority);

#ifdef __cplusplus
}
#endif

#endif