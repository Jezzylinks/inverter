#ifndef STORAGE_NVS_MANAGER_H
#define STORAGE_NVS_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STORAGE_NVS_STATE_UNINITIALIZED = 0,
    STORAGE_NVS_STATE_READY,
    STORAGE_NVS_STATE_RECOVERED,
    STORAGE_NVS_STATE_FAILED
} storage_nvs_state_t;

/* Idempotent, process-wide initialization. Only this module may initialize NVS. */
esp_err_t storage_nvs_init(void);
storage_nvs_state_t storage_nvs_state(void);
bool storage_nvs_is_ready(void);
esp_err_t storage_nvs_last_error(void);
unsigned storage_nvs_recovery_count(void);
uint32_t storage_nvs_schema_version(void);

/* Serialize complete open/read-or-write/commit/close transactions. */
esp_err_t storage_nvs_open(const char *namespace_name, nvs_open_mode_t mode,
                           nvs_handle_t *out_handle);
esp_err_t storage_nvs_close(nvs_handle_t handle);
esp_err_t storage_nvs_commit_close(nvs_handle_t handle);

/* Safe diagnostics and deliberately explicit destructive operations. */
esp_err_t storage_nvs_get_stats(nvs_stats_t *out_stats);
esp_err_t storage_nvs_erase_namespace(const char *namespace_name);
esp_err_t storage_nvs_factory_reset(void);

/* Flash-cache exclusion lock.
 *
 * Any code path that calls esp_wifi_start() or esp_wifi_stop() must
 * hold this lock for the duration of that call.  nvs_commit() disables
 * the ESP32 flash cache while it erases and writes a flash page; if
 * esp_wifi_start/stop() is executing on another core or task at the
 * same instant it will attempt to fetch instructions from flash through
 * the (now-disabled) cache and trigger a "Cache disabled but cached
 * memory region accessed" Guru Meditation panic.
 *
 * Acquiring this lock before esp_wifi_start/stop() and releasing it
 * afterwards ensures that no nvs_commit() can run concurrently, and
 * that if a commit is already in progress the Wi-Fi call waits until
 * the cache is safely re-enabled before touching flash itself.
 *
 * timeout_ms: how long to wait; pass 0 for the default (3 s). */
esp_err_t storage_nvs_lock(uint32_t timeout_ms);
void      storage_nvs_unlock(void);

#ifdef __cplusplus
}
#endif
#endif
