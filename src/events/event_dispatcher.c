#include "event_dispatcher.h"

#include "esp_log.h"

static const char *TAG = "EVENT_DISPATCHER";

/******************************************************************************
 * Subscriber Queues
 ******************************************************************************/

QueueHandle_t g_event_subscriber_queue[EVENT_SUB_COUNT] = {0};

/******************************************************************************
 * Initialize Dispatcher
 ******************************************************************************/

bool event_dispatcher_init(void)
{
    for (int i = 0; i < EVENT_SUB_COUNT; i++)
    {
        g_event_subscriber_queue[i] =
            xQueueCreate(EVENT_SUBSCRIBER_QUEUE_LENGTH,
                         sizeof(system_event_t));

        if (g_event_subscriber_queue[i] == NULL)
        {
            ESP_LOGE(TAG,
                     "Failed to create subscriber queue %d",
                     i);

            return false;
        }
    }

    ESP_LOGI(TAG,
             "Event dispatcher initialized.");

    return true;
}

/******************************************************************************
 * Dispatcher Task
 ******************************************************************************/

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