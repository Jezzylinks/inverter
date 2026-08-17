#ifndef OTA_DOWNLOAD_H
#define OTA_DOWNLOAD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef bool (*ota_download_cancel_cb_t)(void *context);
typedef void (*ota_download_progress_cb_t)(int percent, void *context);
typedef void (*ota_download_verifying_cb_t)(void *context);

esp_err_t ota_download_firmware(const char *url, const char *expected_sha256,
                                uint32_t expected_size,
                                ota_download_cancel_cb_t cancel_cb,
                                ota_download_progress_cb_t progress_cb,
                                ota_download_verifying_cb_t verifying_cb,
                                void *context);

#ifdef __cplusplus
}
#endif

#endif /* OTA_DOWNLOAD_H */
