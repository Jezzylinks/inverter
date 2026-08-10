/**
 * @file ota_service.h
 * @brief OTA Update Service Interface
 */

#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_err.h"
#include "stdbool.h"

#define OTA_TASK_STACK_SIZE 8192
#define OTA_TASK_PRIORITY 5
#define OTA_HTTP_TIMEOUT_MS 30000

    typedef enum
    {
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
     * @brief Initialize OTA service
     * @return ESP_OK on success
     */
    esp_err_t ota_service_init(void);

    /**
     * @brief Start OTA update from URL
     * @param url Firmware download URL (HTTPS)
     * @return ESP_OK if task started, error otherwise
     */
    esp_err_t ota_service_start(const char *url);

    /**
     * @brief Cancel ongoing OTA
     * @return ESP_OK on success
     */
    esp_err_t ota_service_cancel(void);

    /**
     * @brief Check remote version
     * @param version_url URL to version file
     * @param new_version Output buffer for version string
     * @param version_len Buffer size
     * @return ESP_OK on success
     */
    esp_err_t ota_service_check_version(const char *version_url, char *new_version, size_t version_len);

    /**
     * @brief Get current firmware version
     * @param buffer Output buffer
     * @param len Buffer size
     * @return ESP_OK on success
     */
    esp_err_t ota_service_get_current_version(char *buffer, size_t len);

    /**
     * @brief Register progress callback
     * @param callback Function called with 0-100 progress
     * @return ESP_OK on success
     */
    esp_err_t ota_service_register_progress_callback(ota_progress_callback_t callback);

    /**
     * @brief Register status callback
     * @param callback Function called on status changes
     * @return ESP_OK on success
     */
    esp_err_t ota_service_register_status_callback(ota_status_callback_t callback);

    /**
     * @brief Check if OTA is currently in progress
     * @return true if OTA is running
     */
    bool ota_service_in_progress(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_SERVICE_H */