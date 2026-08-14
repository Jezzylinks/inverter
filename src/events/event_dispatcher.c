#include "stdint.h"
#include "stdarg.h"
#include "event_dispatcher.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "utility/led.h"
#include "utility/buzzer.h"
#include "string.h"
#include "fault_log.h"
#include "system_events.h"
#include "stdbool.h"
#include "nvs.h"

static const char *TAG = "EVENT_DISPATCHER";
#define MONITOR_STATS_NVS_NAMESPACE "monitor_statistics"
#define MONITOR_STATS_NVS_KEY "monitor_stats"

/******************************************************************************
 * Subscriber Queues
 ******************************************************************************/

bool event_dispatcher_send(event_subscriber_t subscriber,
                           const system_event_t *event);
extern QueueHandle_t g_event_subscriber_queue[EVENT_SUB_COUNT];

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
    protection_quantity_t last_event_quantity; // tracks warnings/derates too, not just shutdowns
    TickType_t last_event_time;
    TickType_t last_recovery_time;
} monitor_statistics_t;

static monitor_statistics_t stats;
static SemaphoreHandle_t stats_mutex = NULL;
static bool s_dispatcher_initialized = false;

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
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_BUZZER, EVENT_SUB_LOGGER}},

        /**********************************************************************
         * TEMPERATURE DERATE
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_DERATE,
            .quantity = PROT_QUANTITY_TEMPERATURE,
            .subscriber_count = 4,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_RELAY, EVENT_SUB_BUZZER, EVENT_SUB_LOGGER}},

        /**********************************************************************
         * TEMPERATURE SHUTDOWN
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_SHUTDOWN,
            .quantity = PROT_QUANTITY_TEMPERATURE,
            .subscriber_count = 5,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_RELAY, EVENT_SUB_BUZZER, EVENT_SUB_LOGGER, EVENT_SUB_WIFI}},

        /**********************************************************************
         * TEMPERATURE RECOVERED
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_RECOVERED,
            .quantity = PROT_QUANTITY_TEMPERATURE,
            .subscriber_count = 4,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_RELAY, EVENT_SUB_LOGGER, EVENT_SUB_WIFI}},

        /**********************************************************************
         * BATTERY WARNING
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_WARNING,
            .quantity = PROT_QUANTITY_BATTERY_VOLTAGE,
            .subscriber_count = 3,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_BUZZER, EVENT_SUB_LOGGER}},

        /**********************************************************************
         * BATTERY DERATE  <-- NEW
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_DERATE,
            .quantity = PROT_QUANTITY_BATTERY_VOLTAGE,
            .subscriber_count = 4,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_RELAY, EVENT_SUB_BUZZER, EVENT_SUB_LOGGER}},

        /**********************************************************************
         * BATTERY SHUTDOWN
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_SHUTDOWN,
            .quantity = PROT_QUANTITY_BATTERY_VOLTAGE,
            .subscriber_count = 5,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_RELAY, EVENT_SUB_BUZZER, EVENT_SUB_LOGGER, EVENT_SUB_WIFI}},

        /**********************************************************************
         * BATTERY RECOVERED  <-- NEW
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_RECOVERED,
            .quantity = PROT_QUANTITY_BATTERY_VOLTAGE,
            .subscriber_count = 4,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_RELAY, EVENT_SUB_LOGGER, EVENT_SUB_WIFI}},

        /**********************************************************************
         * AC VOLTAGE WARNING  <-- NEW
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_WARNING,
            .quantity = PROT_QUANTITY_AC_VOLTAGE,
            .subscriber_count = 3,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_BUZZER, EVENT_SUB_LOGGER}},

        /**********************************************************************
         * AC VOLTAGE DERATE  <-- NEW
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_DERATE,
            .quantity = PROT_QUANTITY_AC_VOLTAGE,
            .subscriber_count = 4,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_RELAY, EVENT_SUB_BUZZER, EVENT_SUB_LOGGER}},

        /**********************************************************************
         * AC VOLTAGE SHUTDOWN
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_SHUTDOWN,
            .quantity = PROT_QUANTITY_AC_VOLTAGE,
            .subscriber_count = 5,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_RELAY, EVENT_SUB_BUZZER, EVENT_SUB_LOGGER, EVENT_SUB_WIFI}},

        /**********************************************************************
         * AC VOLTAGE RECOVERED  <-- NEW
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_RECOVERED,
            .quantity = PROT_QUANTITY_AC_VOLTAGE,
            .subscriber_count = 4,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_RELAY, EVENT_SUB_LOGGER, EVENT_SUB_WIFI}},

        /**********************************************************************
         * OUTPUT CURRENT WARNING  <-- NEW
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_WARNING,
            .quantity = PROT_QUANTITY_OUTPUT_CURRENT,
            .subscriber_count = 3,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_BUZZER, EVENT_SUB_LOGGER}},

        /**********************************************************************
         * OUTPUT CURRENT DERATE  <-- NEW
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_DERATE,
            .quantity = PROT_QUANTITY_OUTPUT_CURRENT,
            .subscriber_count = 4,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_RELAY, EVENT_SUB_BUZZER, EVENT_SUB_LOGGER}},

        /**********************************************************************
         * OUTPUT CURRENT SHUTDOWN
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_SHUTDOWN,
            .quantity = PROT_QUANTITY_OUTPUT_CURRENT,
            .subscriber_count = 5,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_RELAY, EVENT_SUB_BUZZER, EVENT_SUB_LOGGER, EVENT_SUB_WIFI}},

        /**********************************************************************
         * OUTPUT CURRENT RECOVERED  <-- NEW
         **********************************************************************/
        {
            .category = EVENT_CATEGORY_PROTECTION,
            .action = EVENT_ACTION_RECOVERED,
            .quantity = PROT_QUANTITY_OUTPUT_CURRENT,
            .subscriber_count = 4,
            .subscribers = {EVENT_SUB_LCD, EVENT_SUB_RELAY, EVENT_SUB_LOGGER, EVENT_SUB_WIFI}}};

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
        [EVENT_SUB_ENFORCER] = 10,
        [EVENT_SUB_FAULT_LOG] = 10};

bool event_dispatcher_init(void)
{
    if (s_dispatcher_initialized) {
        return true;
    }

    memset(g_event_subscriber_queue, 0, sizeof(g_event_subscriber_queue));
    for (int i = 0; i < EVENT_SUB_COUNT; i++) {
        if (queue_sizes[i] == 0U) {
            return false;
        }
        g_event_subscriber_queue[i] = xQueueCreate(queue_sizes[i], sizeof(system_event_t));
        if (!g_event_subscriber_queue[i]) {
            event_dispatcher_deinit();
            ESP_LOGE(TAG, "Failed to allocate subscriber queue %d", i);
            return false;
        }
    }
    s_dispatcher_initialized = true;
    ESP_LOGI(TAG, "Event dispatcher initialized (free heap=%lu)",
             (unsigned long)esp_get_free_heap_size());
    return true;
}

void event_dispatcher_deinit(void)
{
    for (int i = 0; i < EVENT_SUB_COUNT; ++i) {
        if (g_event_subscriber_queue[i]) {
            vQueueDelete(g_event_subscriber_queue[i]);
            g_event_subscriber_queue[i] = NULL;
        }
    }
    if (stats_mutex) {
        vSemaphoreDelete(stats_mutex);
        stats_mutex = NULL;
    }
    memset(&stats, 0, sizeof(stats));
    s_dispatcher_initialized = false;
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
    {
        const event_route_t *route = event_route_find(evt);
        if (route != NULL)
        {
            for (uint8_t i = 0; i < route->subscriber_count; i++)
                event_dispatcher_send(route->subscribers[i], evt);
        }
        else
        {
            event_dispatcher_send(EVENT_SUB_LCD, evt);
            event_dispatcher_send(EVENT_SUB_LOGGER, evt);
            event_dispatcher_send(EVENT_SUB_FAULT_LOG, evt);
        }

        event_dispatcher_send(EVENT_SUB_MONITOR, evt);
        event_dispatcher_send(EVENT_SUB_ENFORCER, evt); // <-- always, not just fallback
        break;
    }

    case EVENT_CATEGORY_SYSTEM:
        event_dispatcher_send(EVENT_SUB_LCD, evt);
        event_dispatcher_send(EVENT_SUB_LED, evt);
        event_dispatcher_send(EVENT_SUB_BUZZER, evt);
        event_dispatcher_send(EVENT_SUB_LOGGER, evt);
        event_dispatcher_send(EVENT_SUB_FAULT_LOG, evt);
        break;

    case EVENT_CATEGORY_BUTTON:

        event_dispatcher_send(EVENT_SUB_LCD, evt);
        event_dispatcher_send(EVENT_SUB_BUZZER, evt);
        break;

    case EVENT_CATEGORY_FACTORY_RESET:
        /* Factory reset outcomes are rare and significant -- worth a
         * persistent record, not just a beep/LED and a log line. */
        event_dispatcher_send(EVENT_SUB_LCD, evt);
        event_dispatcher_send(EVENT_SUB_LED, evt);
        event_dispatcher_send(EVENT_SUB_BUZZER, evt);
        event_dispatcher_send(EVENT_SUB_LOGGER, evt);
        event_dispatcher_send(EVENT_SUB_FAULT_LOG, evt);
        break;

    case EVENT_CATEGORY_WIFI:
        event_dispatcher_send(EVENT_SUB_LCD, evt);
        event_dispatcher_send(EVENT_SUB_LED, evt);
        event_dispatcher_send(EVENT_SUB_BUZZER, evt);
        event_dispatcher_send(EVENT_SUB_LOGGER, evt);
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

    BaseType_t sent;
    if ((subscriber == EVENT_SUB_LED || subscriber == EVENT_SUB_BUZZER) &&
        event->priority == EVENT_PRIORITY_CRITICAL) {
        /* Critical alarm output supersedes queued lower-priority indications. */
        if (subscriber == EVENT_SUB_BUZZER) {
            buzzer_request_critical_preemption();
        }
        system_event_t discarded;
        while (xQueueReceive(g_event_subscriber_queue[subscriber],
                             &discarded,
                             0) == pdPASS) {
        }
        sent = xQueueSendToFront(g_event_subscriber_queue[subscriber], event, 0);
    } else {
        sent = xQueueSend(g_event_subscriber_queue[subscriber], event, 0);
    }

    if (sent != pdPASS) {
        ESP_LOGW(TAG, "Subscriber queue %d is full; event dropped", subscriber);
        return false;
    }
    return true;
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
    ESP_LOGI(TAG, "Dispatcher task started");

    system_event_t evt = {0};

    while (1)
    {

        bool ok = system_event_receive(&evt, pdMS_TO_TICKS(1000));

        if (ok)
        {
            event_route_dispatch(&evt);
        }
    }
}

void monitor_statistics_init(void)
{
    if (stats_mutex == NULL)
    {
        stats_mutex = xSemaphoreCreateMutex();
    }
    if (stats_mutex && xSemaphoreTake(stats_mutex, pdMS_TO_TICKS(100)))
    {
        memset(&stats, 0, sizeof(stats));
        xSemaphoreGive(stats_mutex);
    }
}

bool monitor_statistics_get(monitor_statistics_t *out)
{
    if (out == NULL || stats_mutex == NULL)
        return false;

    if (!xSemaphoreTake(stats_mutex, pdMS_TO_TICKS(100)))
        return false;

    *out = stats;
    xSemaphoreGive(stats_mutex);
    return true;
}

bool monitor_statistics_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MONITOR_STATS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        /* Namespace doesn't exist yet (first boot) -- not an error. */
        return false;
    }

    monitor_statistics_t loaded;
    size_t required_size = sizeof(loaded);
    err = nvs_get_blob(handle, MONITOR_STATS_NVS_KEY, &loaded, &required_size);
    nvs_close(handle);

    if (err != ESP_OK || required_size != sizeof(loaded))
    {
        ESP_LOGW("MON_STATS", "No saved stats found, starting fresh: %s",
                 esp_err_to_name(err));
        return false;
    }

    stats = loaded;
    ESP_LOGI("MON_STATS", "Loaded lifetime stats: %lu shutdowns, %lu warnings",
             (unsigned long)stats.shutdowns, (unsigned long)stats.warnings);
    return true;
}

bool monitor_statistics_save(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MONITOR_STATS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGW("MON_STATS", "Failed to open NVS for stats save: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(handle, MONITOR_STATS_NVS_KEY, &stats, sizeof(stats));
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK)
    {
        ESP_LOGW("MON_STATS", "Failed to save stats: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void monitor_statistics_update(const system_event_t *evt)
{
    if (evt == NULL || stats_mutex == NULL)
        return;

    if (!xSemaphoreTake(stats_mutex, pdMS_TO_TICKS(100)))
        return;

    stats.last_event_quantity = evt->quantity;
    stats.last_event_time = evt->timestamp;

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
        monitor_statistics_save();
        break;

    case EVENT_ACTION_RECOVERED:
        stats.recoveries++;
        stats.last_recovery_time = evt->timestamp;
        monitor_statistics_save();
        break;

    default:
        break;
    }

    xSemaphoreGive(stats_mutex);
}

void monitor_event_task(void *pv)
{
    system_event_t evt;
    monitor_statistics_init();
    monitor_statistics_load(); /* no-op if nothing saved yet (first boot) */

    while (1)
    {
        if (!event_dispatcher_receive(EVENT_SUB_MONITOR, &evt, portMAX_DELAY))
        {
            continue;
        }

        monitor_statistics_update(&evt);
    }
}