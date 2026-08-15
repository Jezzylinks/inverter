#include "system_diagnostics.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "system_state.h"

#define DIAGNOSTICS_NAMESPACE "diagnostics"
#define DIAGNOSTICS_KEY "snapshot_v1"
#define DIAGNOSTICS_MAGIC 0x44494147UL
#define DIAGNOSTICS_VERSION 1U

static const char *TAG = "DIAGNOSTICS";
static system_diagnostics_snapshot_t s_snapshot;
static bool s_initialized;

static uint32_t checksum(const system_diagnostics_snapshot_t *snapshot)
{
    const uint8_t *bytes = (const uint8_t *)snapshot;
    uint32_t hash = 2166136261UL;
    for (size_t i = 0U; i < sizeof(*snapshot); ++i) {
        hash ^= bytes[i];
        hash *= 16777619UL;
    }
    return hash;
}

typedef struct {
    uint32_t magic;
    uint32_t version;
    system_diagnostics_snapshot_t snapshot;
    uint32_t checksum;
} diagnostics_record_t;

static bool persist(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DIAGNOSTICS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to open diagnostics NVS: %s", esp_err_to_name(err));
        return false;
    }

    diagnostics_record_t record = {
        .magic = DIAGNOSTICS_MAGIC,
        .version = DIAGNOSTICS_VERSION,
        .snapshot = s_snapshot,
    };
    record.checksum = checksum(&record.snapshot) ^ record.magic ^ record.version;
    err = nvs_set_blob(handle, DIAGNOSTICS_KEY, &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to persist diagnostics: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool system_diagnostics_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    nvs_handle_t handle;
    diagnostics_record_t record;
    size_t length = sizeof(record);
    esp_err_t err = nvs_open(DIAGNOSTICS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, DIAGNOSTICS_KEY, &record, &length);
        nvs_close(handle);
    }

    if (err == ESP_OK && length == sizeof(record) &&
        record.magic == DIAGNOSTICS_MAGIC &&
        record.version == DIAGNOSTICS_VERSION &&
        record.checksum == (checksum(&record.snapshot) ^ record.magic ^ record.version)) {
        s_snapshot = record.snapshot;
    }

    s_snapshot.boot_count++;
    s_snapshot.last_reset_reason = esp_reset_reason();
    s_initialized = persist();
    ESP_LOGI(TAG, "Boot #%lu, reset reason: %s",
             (unsigned long)s_snapshot.boot_count,
             system_diagnostics_reset_reason_name(s_snapshot.last_reset_reason));
    return s_initialized;
}

void system_diagnostics_record_fault(uint32_t fault_flags,
                                     float battery_voltage,
                                     uint32_t timestamp_ms)
{
    if (!s_initialized) {
        return;
    }
    s_snapshot.last_fault_flags = fault_flags;
    s_snapshot.last_battery_voltage = battery_voltage;
    s_snapshot.last_fault_timestamp_ms = timestamp_ms;
    (void)persist();
}

void system_diagnostics_record_uptime(uint32_t uptime_seconds)
{
    if (!s_initialized) {
        return;
    }
    s_snapshot.last_uptime_seconds = uptime_seconds;
}

bool system_diagnostics_get_snapshot(system_diagnostics_snapshot_t *out)
{
    if (!out || !s_initialized) {
        return false;
    }
    *out = s_snapshot;
    return true;
}

const char *system_diagnostics_reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
    }
}
