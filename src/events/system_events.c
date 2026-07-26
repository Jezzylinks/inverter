#include "system_events.h"
#include "string.h"
#include "esp_log.h"

static const char *TAG = "SYSTEM_EVENTS";

/******************************************************************************
 * Global Queue
 ******************************************************************************/

QueueHandle_t g_system_event_queue = NULL;

/******************************************************************************
 * Initialization
 ******************************************************************************/

bool system_events_init(void)
{
    if (g_system_event_queue != NULL)
    {
        return true;
    }

    g_system_event_queue =
        xQueueCreate(SYSTEM_EVENT_QUEUE_LENGTH,
                     sizeof(system_event_t));

    if (g_system_event_queue == NULL)
    {
        ESP_LOGE(TAG,
                 "Failed to create event queue.");

        return false;
    }

    ESP_LOGI(TAG,
             "System event queue created.");

    return true;
}

/******************************************************************************
 * Deinitialization
 ******************************************************************************/

void system_events_deinit(void)
{
    if (g_system_event_queue != NULL)
    {
        vQueueDelete(g_system_event_queue);

        g_system_event_queue = NULL;
    }
}

/******************************************************************************
 * Post Event
 ******************************************************************************/

bool system_event_post(const system_event_t *event)
{
    if ((event == NULL) ||
        (g_system_event_queue == NULL))
    {
        return false;
    }

    return xQueueSend(g_system_event_queue,
                      event,
                      0) == pdTRUE;
}

/******************************************************************************
 * Receive Event
 ******************************************************************************/

bool system_event_receive(system_event_t *event,
                          TickType_t timeout)
{
    if ((event == NULL) ||
        (g_system_event_queue == NULL))
    {
        return false;
    }

    return xQueueReceive(g_system_event_queue,
                         event,
                         timeout) == pdTRUE;
}

/******************************************************************************
 * Event Post
 ******************************************************************************/

bool system_event_post_protection(protection_quantity_t quantity,
                                  protection_action_t action,
                                  float value)
{
    system_event_t evt = {0};

    evt.category = EVENT_CATEGORY_PROTECTION;
    evt.source = EVENT_SOURCE_PROTECTION;
    evt.quantity = quantity;
    evt.value = value;
    evt.timestamp = xTaskGetTickCount();

    switch (action)
    {
    case PROT_ACTION_WARN:

        evt.action = EVENT_ACTION_WARNING;
        evt.priority = EVENT_PRIORITY_LOW;
        break;

    case PROT_ACTION_DERATE:

        evt.action = EVENT_ACTION_DERATE;
        evt.priority = EVENT_PRIORITY_NORMAL;
        break;

    case PROT_ACTION_SHUTDOWN:

        evt.action = EVENT_ACTION_SHUTDOWN;
        evt.priority = EVENT_PRIORITY_CRITICAL;
        break;

    case PROT_ACTION_RECOVERED:

        evt.action = EVENT_ACTION_RECOVERED;
        evt.priority = EVENT_PRIORITY_NORMAL;
        break;

    case PROT_ACTION_NONE:

    default:
        return false;
    }

    return system_event_post(&evt);
}

/******************************************************************************
 * String Helpers
 ******************************************************************************/

const char *event_category_name(event_category_t category)
{
    switch (category)
    {
    case EVENT_CATEGORY_PROTECTION:
        return "Protection";
    case EVENT_CATEGORY_BUTTON:
        return "Button";
    case EVENT_CATEGORY_LCD:
        return "LCD";
    case EVENT_CATEGORY_RELAY:
        return "Relay";
    case EVENT_CATEGORY_WIFI:
        return "WiFi";
    case EVENT_CATEGORY_SYSTEM:
        return "System";
    case EVENT_CATEGORY_LOG:
        return "Logger";
    case EVENT_CATEGORY_USER:
        return "User";

    default:
        return "Unknown";
    }
}

const char *event_action_name(event_action_t action)
{
    switch (action)
    {
    case EVENT_ACTION_WARNING:
        return "Warning";
    case EVENT_ACTION_DERATE:
        return "Derate";
    case EVENT_ACTION_SHUTDOWN:
        return "Shutdown";
    case EVENT_ACTION_RECOVERED:
        return "Recovered";
    case EVENT_ACTION_START:
        return "Start";
    case EVENT_ACTION_STOP:
        return "Stop";
    case EVENT_ACTION_ON:
        return "On";
    case EVENT_ACTION_OFF:
        return "Off";
    case EVENT_ACTION_PRESSED:
        return "Pressed";
    case EVENT_ACTION_RELEASED:
        return "Released";
    case EVENT_ACTION_LONG_PRESS:
        return "Long Press";
    case EVENT_ACTION_DOUBLE_PRESS:
        return "Double Press";
    case EVENT_ACTION_CHANGED:
        return "Changed";

    default:
        return "Unknown";
    }
}

const char *event_source_name(event_source_t source)
{
    switch (source)
    {
    case EVENT_SOURCE_PROTECTION:
        return "Protection";
    case EVENT_SOURCE_ADC:
        return "ADC";
    case EVENT_SOURCE_LCD:
        return "LCD";
    case EVENT_SOURCE_BUTTON:
        return "Button";
    case EVENT_SOURCE_RELAY:
        return "Relay";
    case EVENT_SOURCE_WIFI:
        return "WiFi";
    case EVENT_SOURCE_SYSTEM:
        return "System";
    case EVENT_SOURCE_NVS:
        return "NVS";

    default:
        return "Unknown";
    }
}

const char *event_priority_name(event_priority_t priority)
{
    switch (priority)
    {
    case EVENT_PRIORITY_LOW:
        return "Low";
    case EVENT_PRIORITY_NORMAL:
        return "Normal";
    case EVENT_PRIORITY_HIGH:
        return "High";
    case EVENT_PRIORITY_CRITICAL:
        return "Critical";

    default:
        return "Unknown";
    }
}