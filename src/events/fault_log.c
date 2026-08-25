#include "events/fault_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "events/system_events.h"
#include "system/system_state.h"
#include "events/event_dispatcher.h"

#include "system/task_watchdog.h"
static const char *TAG = "fault_log";
extern system_state_t sys_state;
static fault_log_entry_t s_entries[FAULT_LOG_CAPACITY];
static uint32_t s_head;  // index of next write slot
static uint32_t s_count; // number of valid entries (<= CAPACITY)
static SemaphoreHandle_t s_mutex;
static volatile bool s_dirty;

/* Small ISR-safe staging queue so protection.c's ISR-context callers
 * (if any land there in the future - e.g. a hardware overcurrent
 * comparator interrupt) never block on the main mutex. */
static RingbufHandle_t s_isr_ringbuf;

typedef struct
{
    fault_severity_t severity;
    fault_source_t source;
    float value;
} isr_staged_entry_t;

/* On-disk layout: raw array + count + crc32 trailer. Simple and
 * sufficient for 32 small structs; no need for NVS blob chunking. */
typedef struct
{
    uint32_t count;
    fault_log_entry_t entries[FAULT_LOG_CAPACITY];
    uint32_t crc32;
} fault_log_nvs_blob_t;

static uint32_t compute_crc(const fault_log_nvs_blob_t *blob)
{
    return esp_crc32_le(0, (const uint8_t *)blob,
                        offsetof(fault_log_nvs_blob_t, crc32));
}

bool fault_log_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex)
        return false;

    s_isr_ringbuf = xRingbufferCreate(sizeof(isr_staged_entry_t) * 16, RINGBUF_TYPE_NOSPLIT);
    if (!s_isr_ringbuf)
    {
        ESP_LOGW(TAG, "isr ringbuf alloc failed, ISR logging disabled");
    }

    s_head = 0;
    s_count = 0;
    s_dirty = false;

    nvs_handle_t h;
    esp_err_t err = nvs_open(FAULT_LOG_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        ESP_LOGI(TAG, "no prior fault log in NVS (err=0x%x), starting fresh", err);
        return true;
    }

    fault_log_nvs_blob_t blob;
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, FAULT_LOG_NVS_KEY, &blob, &len);
    nvs_close(h);

    if (err == ESP_OK && len == sizeof(blob) && compute_crc(&blob) == blob.crc32 && blob.count <= FAULT_LOG_CAPACITY)
    {
        memcpy(s_entries, blob.entries, sizeof(s_entries));
        s_count = blob.count;
        s_head = s_count % FAULT_LOG_CAPACITY;
        ESP_LOGI(TAG, "restored %u fault log entries from NVS", (unsigned)s_count);
    }
    else
    {
        ESP_LOGW(TAG, "fault log NVS blob missing or corrupt, starting fresh");
    }
    return true;
}

static void add_locked(fault_severity_t severity, fault_source_t source,
                       float value, const char *message)
{
    fault_log_entry_t *slot = &s_entries[s_head];
    slot->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    slot->severity = severity;
    slot->source = source;
    slot->value = value;
    if (message)
    {
        strncpy(slot->message, message, sizeof(slot->message) - 1);
        slot->message[sizeof(slot->message) - 1] = '\0';
    }
    else
    {
        slot->message[0] = '\0';
    }

    s_head = (s_head + 1) % FAULT_LOG_CAPACITY;
    if (s_count < FAULT_LOG_CAPACITY)
        s_count++;
    s_dirty = true;
}

void fault_log_add(fault_severity_t severity, fault_source_t source, float value, const char *message)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    add_locked(severity, source, value, message);
    xSemaphoreGive(s_mutex);

    if (severity >= FAULT_SEV_FAULT)
    {
        /* Faults and critical events are important enough to persist
         * promptly rather than waiting for the periodic flush task. */
        fault_log_flush_to_nvs();
    }
}

void fault_log_add_event(const system_event_t *evt)
{
    if (evt == NULL)
    {
        return;
    }

    fault_severity_t severity = FAULT_SEV_INFO;
    fault_source_t source = FAULT_SRC_POWER_METER;
    const char *message = "Unknown event";

    /*--------------------------------------------------------------
     * Convert Event Action -> Fault Severity
     *-------------------------------------------------------------*/
    switch (evt->action)
    {
    case EVENT_ACTION_WARNING:
        severity = FAULT_SEV_WARNING;
        break;

    case EVENT_ACTION_DERATE:
        severity = FAULT_SEV_DERATED;
        break;

    case EVENT_ACTION_SHUTDOWN:
        severity = FAULT_SEV_FAULT;
        break;

    case EVENT_ACTION_RECOVERED:
        severity = FAULT_SEV_INFO;
        break;

    default:
        severity = FAULT_SEV_INFO;
        break;
    }

    /*--------------------------------------------------------------
     * Convert Event Source
     *-------------------------------------------------------------*/
    switch (evt->category)
    {
    case EVENT_CATEGORY_PROTECTION:

        switch (evt->quantity)
        {
        case PROT_QUANTITY_AC_VOLTAGE:

            source = FAULT_SRC_PROTECTION_AC_VOLTAGE;

            switch (evt->action)
            {
            case EVENT_ACTION_WARNING:
                message = "AC voltage warning";
                break;

            case EVENT_ACTION_DERATE:
                message = "AC voltage derated";
                break;

            case EVENT_ACTION_SHUTDOWN:
                message = "AC voltage shutdown";
                break;

            case EVENT_ACTION_RECOVERED:
                message = "AC voltage recovered";
                break;

            default:
                break;
            }

            break;

        case PROT_QUANTITY_OUTPUT_CURRENT:

            source = FAULT_SRC_PROTECTION_CURRENT;

            switch (evt->action)
            {
            case EVENT_ACTION_WARNING:
                message = "Output current warning";
                break;

            case EVENT_ACTION_DERATE:
                message = "Current derated";
                break;

            case EVENT_ACTION_SHUTDOWN:
                message = "Output overload";
                break;

            case EVENT_ACTION_RECOVERED:
                message = "Current recovered";
                break;

            default:
                break;
            }

            break;

        case PROT_QUANTITY_TEMPERATURE:

            source = FAULT_SRC_PROTECTION_TEMPERATURE;

            switch (evt->action)
            {
            case EVENT_ACTION_WARNING:
                message = "Temperature warning";
                break;

            case EVENT_ACTION_DERATE:
                message = "Temperature derated";
                break;

            case EVENT_ACTION_SHUTDOWN:
                message = "Over temperature";
                break;

            case EVENT_ACTION_RECOVERED:
                message = "Temperature recovered";
                break;

            default:
                break;
            }

            break;

        case PROT_QUANTITY_BATTERY_VOLTAGE:

            source = FAULT_SRC_PROTECTION_BATTERY;

            switch (evt->action)
            {
            case EVENT_ACTION_WARNING:
                message = "Battery warning";
                break;

            case EVENT_ACTION_DERATE:
                message = "Battery derated";
                break;

            case EVENT_ACTION_SHUTDOWN:

                if (evt->value <
                    sys_state.battery_profile.cutoff_voltage_12v)
                {
                    message = "Low battery shutdown";
                }
                else
                {
                    message = "Battery overvoltage";
                }

                break;

            case EVENT_ACTION_RECOVERED:
                message = "Battery recovered";
                break;

            default:
                break;
            }

            break;

        default:

            source = FAULT_SRC_POWER_METER;
            message = "Unknown protection";

            break;
        }

        break;

    case EVENT_CATEGORY_SYSTEM:

        source = FAULT_SRC_RESTART_POLICY;
        message = "System event";
        break;

    case EVENT_CATEGORY_FACTORY_RESET:

        source = FAULT_SRC_NVS;
        message = "Factory reset";
        break;

    case EVENT_CATEGORY_BUTTON:

        source = FAULT_SRC_BUTTON;
        message = "Button event";
        break;

    case EVENT_CATEGORY_WIFI:

        source = FAULT_SRC_POWER_METER;
        message = "WiFi event";
        break;

    default:

        source = FAULT_SRC_POWER_METER;
        message = "Unknown event";

        break;
    }

    ESP_LOGI(TAG,
             "[%s] %s (%.2f) : %s",
             fault_log_severity_name(severity),
             fault_log_source_name(source),
             evt->value,
             message);

    fault_log_add(severity, source, evt->value, message);
}

void fault_log_add_from_isr(fault_severity_t severity, fault_source_t source, float value)
{
    if (!s_isr_ringbuf)
        return;
    isr_staged_entry_t staged = {.severity = severity, .source = source, .value = value};
    BaseType_t higher_prio_task_woken = pdFALSE;
    xRingbufferSendFromISR(s_isr_ringbuf, &staged, sizeof(staged), &higher_prio_task_woken);
    if (higher_prio_task_woken)
        portYIELD_FROM_ISR();
}

/* Call this periodically (e.g. every 500ms) from a low-priority
 * housekeeping task to drain anything staged from ISR context. */
void fault_log_drain_isr_staged(void)
{
    if (!s_isr_ringbuf)
        return;
    size_t item_size;
    isr_staged_entry_t *staged;
    while ((staged = (isr_staged_entry_t *)xRingbufferReceive(s_isr_ringbuf, &item_size, 0)) != NULL)
    {
        fault_log_add(staged->severity, staged->source, staged->value, "isr-event");
        vRingbufferReturnItem(s_isr_ringbuf, (void *)staged);
    }
}

uint32_t fault_log_get_entries(fault_log_entry_t *out_entries, uint32_t max_entries)
{
    if (!out_entries || max_entries == 0)
        return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t n = s_count < max_entries ? s_count : max_entries;
    for (uint32_t i = 0; i < n; i++)
    {
        /* most-recent-first: walk backwards from (s_head - 1) */
        uint32_t idx = (s_head + FAULT_LOG_CAPACITY - 1 - i) % FAULT_LOG_CAPACITY;
        out_entries[i] = s_entries[idx];
    }
    xSemaphoreGive(s_mutex);
    return n;
}

bool fault_log_flush_to_nvs(void)
{
    if (!s_dirty)
        return true;

    fault_log_nvs_blob_t blob;
    memset(&blob, 0, sizeof(blob));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    blob.count = s_count;
    memcpy(blob.entries, s_entries, sizeof(s_entries));
    s_dirty = false;
    xSemaphoreGive(s_mutex);

    blob.crc32 = compute_crc(&blob);

    nvs_handle_t h;
    esp_err_t err = nvs_open(FAULT_LOG_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open failed: 0x%x", err);
        return false;
    }
    err = nvs_set_blob(h, FAULT_LOG_NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "flush failed: 0x%x", err);
        return false;
    }
    return true;
}

bool fault_log_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_entries, 0, sizeof(s_entries));
    s_head = 0;
    s_count = 0;
    s_dirty = true;
    xSemaphoreGive(s_mutex);
    return fault_log_flush_to_nvs();
}

const char *fault_log_severity_name(fault_severity_t s)
{
    switch (s)
    {
    case FAULT_SEV_INFO:
        return "INFO";
    case FAULT_SEV_WARNING:
        return "WARNING";
    case FAULT_SEV_DERATED:
        return "DERATED";
    case FAULT_SEV_FAULT:
        return "FAULT";
    case FAULT_SEV_CRITICAL:
        return "CRITICAL";
    default:
        return "?";
    }
}

const char *fault_log_source_name(fault_source_t s)
{
    switch (s)
    {
    case FAULT_SRC_PROTECTION_AC_VOLTAGE:
        return "AC Voltage";
    case FAULT_SRC_PROTECTION_CURRENT:
        return "Current";
    case FAULT_SRC_PROTECTION_TEMPERATURE:
        return "Temperature";
    case FAULT_SRC_PROTECTION_BATTERY:
        return "Battery";
    case FAULT_SRC_WATCHDOG:
        return "Watchdog";
    case FAULT_SRC_BROWNOUT:
        return "Brownout";
    case FAULT_SRC_NVS:
        return "NVS";
    case FAULT_SRC_LCD:
        return "LCD";
    case FAULT_SRC_BUTTON:
        return "Button";
    case FAULT_SRC_RESTART_POLICY:
        return "Restart Policy";
    case FAULT_SRC_FAN:
        return "Fan";
    case FAULT_SRC_POWER_METER:
        return "Power Meter";
    default:
        return "Other";
    }
}

void fault_log_event_task(void *pv)
{
    task_watchdog_register("fault_log_event_task");
    system_event_t evt;

    while (1)
    {

        task_watchdog_feed();
        if (!event_dispatcher_receive(EVENT_SUB_FAULT_LOG,
                                      &evt,
                                      pdMS_TO_TICKS(1000U)))
        {
            continue;
        }

        fault_log_add_event(&evt);
    }
}