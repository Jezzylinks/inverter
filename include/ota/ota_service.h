/**
 * @file ota_service.h
 * @brief Secure OTA update service with CSV manifest support.
 */
#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "ota/ota_manifest.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_TASK_STACK_SIZE 8192U
#define OTA_TASK_PRIORITY 5U
/* Keep cancellation responsive while retaining bounded HTTPS operations. */

typedef enum {
    OTA_STATUS_IDLE = 0,
    OTA_STATUS_STARTED,
    OTA_STATUS_DOWNLOADING,
    OTA_STATUS_VERIFYING,
    OTA_STATUS_SUCCESS,
    OTA_STATUS_FAILED,
    OTA_STATUS_CANCELLED
} ota_status_t;

typedef void (*ota_progress_callback_t)(int percent);
typedef void (*ota_status_callback_t)(ota_status_t status, int percent);

/**
 * CSV format accepted by ota_service_start_from_csv():
 *
 *   version,url,sha256,size
 *   1.2.3,https://updates.example/inverter.bin,<64-hex SHA-256>,123456
 *
 * The header is optional. Blank lines and lines beginning with '#' are ignored.
 * The first valid data row is selected. CSV values are bounded and must not
 * contain embedded newlines. Version, URL, SHA-256, and image size are required.
 * The downloaded image must match both the declared size and SHA-256 before the
 * next OTA partition is selected.
 */
esp_err_t ota_manifest_parse_csv(const char *csv, size_t csv_len,
                                  ota_manifest_entry_t *entry);

esp_err_t ota_service_init(void);
/** Validate the running image after startup checks when rollback is enabled. */
esp_err_t ota_service_validate_running_app(bool healthy);
/** True when a previous OTA image was rolled back and needs one notification. */
bool ota_service_rollback_notification_pending(void);
esp_err_t ota_service_start(const char *url);
esp_err_t ota_service_start_from_csv(const char *csv_url);

/**
 * Fetch and parse an HTTPS CSV manifest without scheduling an OTA download.
 * update_available is true only when the manifest version is newer than the
 * currently running application version.
 */
esp_err_t ota_service_check_csv_manifest(const char *csv_url,
                                         ota_manifest_entry_t *entry,
                                         bool *update_available);

esp_err_t ota_service_cancel(void);
esp_err_t ota_service_check_version(const char *version_url,
                                    char *new_version,
                                    size_t version_len);
esp_err_t ota_service_get_current_version(char *buffer, size_t len);
esp_err_t ota_service_register_progress_callback(ota_progress_callback_t callback);
esp_err_t ota_service_register_status_callback(ota_status_callback_t callback);
bool ota_service_in_progress(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_SERVICE_H */
