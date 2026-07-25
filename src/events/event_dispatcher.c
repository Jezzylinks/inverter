#include "stdint.h"
#include "stdarg.h"
#include "event_dispatcher.h"
#include "esp_log.h"
#include "utility/led.h"
#include "utility/buzzer.h"
#include "string.h"
#include "fault_log.h"
#include "system_events.h"
#include "stdbool.h"

static const char *TAG = "EVENT_DISPATCHER";

/******************************************************************************
 * Subscriber Queues
 ******************************************************************************/

bool event_dispatcher_send(event_subscriber_t subscriber,
                           const system_event_t *event);
extern QueueHandle_t g_event_subscriber_queue[EVENT_SUB_COUNT];

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

static const uint8_t queue_sizes[EVENT_SUB_COUNT] =
    {
        [EVENT_SUB_LCD] = 10,
        [EVENT_SUB_RELAY] = 10,
        [EVENT_SUB_LED] = 10,
        [EVENT_SUB_BUZZER] = 10,
        [EVENT_SUB_LOGGER] = 20,
        [EVENT_SUB_WIFI] = 10,
        [EVENT_SUB_MONITOR] = 10,
        [EVENT_SUB_BUTTON] = 10,
        [EVENT_SUB_SYSTEM] = 10,
};

bool event_dispatcher_init(void)
{

    ESP_LOGI(TAG,
             "Free heap = %lu",
             esp_get_free_heap_size());
    for (int i = 0; i < EVENT_SUB_COUNT; i++)
    {
        configASSERT(queue_sizes[i] > 0);

        g_event_subscriber_queue[i] =
            xQueueCreate(queue_sizes[i], sizeof(system_event_t));

        configASSERT(g_event_subscriber_queue[i] != NULL);
    }

    ESP_LOGI(TAG, "Event dispatcher initialized.");

    return true;
}

/******************************************************************************
 * Dispatcher Task
 ******************************************************************************/
void event_route_dispatch(const system_event_t *evt)
{
    if (evt == NULL)
    {
        return;
    }

    switch (evt->category)
    {
    case EVENT_CATEGORY_PROTECTION:

        event_dispatcher_send(EVENT_SUB_LCD, evt);
        event_dispatcher_send(EVENT_SUB_LED, evt);
        event_dispatcher_send(EVENT_SUB_BUZZER, evt);
        event_dispatcher_send(EVENT_SUB_RELAY, evt);
        event_dispatcher_send(EVENT_SUB_LOGGER, evt);

        break;

    case EVENT_CATEGORY_BUTTON:

        event_dispatcher_send(EVENT_SUB_LCD, evt);
        break;

    default:
        break;
    }
}

/******************************************************************************
 * Send Directly To One Subscriber
 ******************************************************************************/

bool event_dispatcher_send(event_subscriber_t subscriber,
                           const system_event_t *event)
{
    if (subscriber >= EVENT_SUB_COUNT)
        return false;

    if (event == NULL)
        return false;

    if (g_event_subscriber_queue[subscriber] == NULL)
        return false;

    return xQueueSend(g_event_subscriber_queue[subscriber],
                      event,
                      0) == pdPASS;
}

/******************************************************************************
 * Receive Event
 ******************************************************************************/

bool event_dispatcher_receive(event_subscriber_t subscriber,
                              system_event_t *event,
                              TickType_t timeout)
{
    if (subscriber >= EVENT_SUB_COUNT)
        return false;

    if (event == NULL)
        return false;

    if (g_event_subscriber_queue[subscriber] == NULL)
        return false;

    return xQueueReceive(g_event_subscriber_queue[subscriber],
                         event,
                         timeout) == pdPASS;
}

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

void event_dispatcher_task(void *pv)
{
    system_event_t evt;

    while (1)
    {
        if (system_event_receive(&evt, portMAX_DELAY))
        {
            event_route_dispatch(&evt);
        }
    }
}

typedef struct
{
    uint32_t warnings;

    uint32_t derates;

    uint32_t shutdowns;

    uint32_t recoveries;

    uint32_t ac_faults;

    uint32_t overload_faults;

    uint32_t temperature_faults;

    uint32_t battery_faults;

    protection_quantity_t last_fault;

    TickType_t last_fault_time;

} monitor_statistics_t;

static monitor_statistics_t stats;

void monitor_statistics_init(void)
{
    memset(&stats, 0, sizeof(stats));
}

const monitor_statistics_t *monitor_statistics_get(void)
{
    return &stats;
}

void monitor_statistics_update(const system_event_t *evt)
{
    if (evt == NULL)
        return;

    switch (evt->action)
    {
    case EVENT_ACTION_WARNING:
        stats.warnings++;
        break;

    case EVENT_ACTION_DERATE:
        stats.derates++;
        break;

    case EVENT_ACTION_SHUTDOWN:

        stats.shutdowns++;

        stats.last_fault = evt->quantity;

        stats.last_fault_time = evt->timestamp;

        switch (evt->quantity)
        {
        case PROT_QUANTITY_AC_VOLTAGE:
            stats.ac_faults++;
            break;

        case PROT_QUANTITY_OUTPUT_CURRENT:
            stats.overload_faults++;
            break;

        case PROT_QUANTITY_TEMPERATURE:
            stats.temperature_faults++;
            break;

        case PROT_QUANTITY_BATTERY_VOLTAGE:
            stats.battery_faults++;
            break;

        default:
            break;
        }

        break;

    case EVENT_ACTION_RECOVERED:

        stats.recoveries++;

        break;

    default:

        break;
    }
}

void monitor_event_task(void *pv)
{
    system_event_t evt;
    monitor_statistics_init();
    while (1)
    {
        if (!event_dispatcher_receive(EVENT_SUB_MONITOR, &evt, portMAX_DELAY))
        {
            continue;
        }

        monitor_statistics_update(&evt);
    }
}

void logger_event_task(void *pv)
{
    system_event_t evt;

    while (1)
    {
        if (!event_dispatcher_receive(EVENT_SUB_LOGGER,
                                      &evt,
                                      portMAX_DELAY))
        {
            continue;
        }

        fault_log_add_event(&evt);
    }
}

void buzzer_event_task(void *pv)
{
    system_event_t evt;

    while (1)
    {
        if (!event_dispatcher_receive(EVENT_SUB_BUZZER,
                                      &evt,
                                      portMAX_DELAY))
        {
            continue;
        }

        switch (evt.action)
        {
        case EVENT_ACTION_WARNING:

            buzzer_beep(100, 80, 1500);

            break;

        case EVENT_ACTION_DERATE:

            buzzer_beep(100, 80, 1500);

            break;

        case EVENT_ACTION_SHUTDOWN:

            buzzer_beep(100, 80, 1500);

            break;

        case EVENT_ACTION_RECOVERED:

            buzzer_beep(100, 80, 1500);

            break;

        default:
            break;
        }
    }
}