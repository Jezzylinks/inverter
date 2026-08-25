#ifndef OTA_MANIFEST_H
#define OTA_MANIFEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_HTTP_TIMEOUT_MS 10000
#define OTA_MAX_URL_LENGTH 256U
#define OTA_MAX_VERSION_LENGTH 32U
#define OTA_MAX_SHA256_LENGTH 65U
#define OTA_MAX_MANIFEST_BYTES 4096U

typedef struct {
    char version[OTA_MAX_VERSION_LENGTH];
    char url[OTA_MAX_URL_LENGTH];
    char sha256[OTA_MAX_SHA256_LENGTH];
    uint32_t image_size;
} ota_manifest_entry_t;

typedef bool (*ota_manifest_cancel_cb_t)(void *context);

bool ota_manifest_is_https_url(const char *url);
bool ota_manifest_version_is_valid(const char *text);
int ota_manifest_compare_versions(const char *candidate, const char *installed);
char *ota_manifest_trim(char *text);

esp_err_t ota_manifest_fetch_text(const char *url, char *buffer, size_t capacity,
                                  size_t *out_len, ota_manifest_cancel_cb_t cancel_cb,
                                  void *context);
esp_err_t ota_manifest_fetch_and_parse(const char *csv_url,
                                       ota_manifest_entry_t *entry,
                                       ota_manifest_cancel_cb_t cancel_cb,
                                       void *context);
esp_err_t ota_manifest_parse_csv(const char *csv, size_t csv_len,
                                  ota_manifest_entry_t *entry);

#ifdef __cplusplus
}
#endif

#endif /* OTA_MANIFEST_H */
