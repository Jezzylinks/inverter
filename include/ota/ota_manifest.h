#ifndef OTA_MANIFEST_H
#define OTA_MANIFEST_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_err.h"
#include "esp_http_client.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_HTTP_TIMEOUT_MS 15000
/* GitHub release URLs redirect to a signed asset URL several hundred bytes
 * long. The default 512-byte HTTP buffers cannot carry that request line. */
/* Keep the HTTP parser/receive staging buffer small; TLS has its own
 * separately configured record buffers. */
#define OTA_HTTP_RX_BUFFER_SIZE 2048
/* GitHub's release redirect can include a long signed Location URL. Keep TX
 * at 2048 rather than 1024 so the redirected request is not truncated. */
#define OTA_HTTP_TX_BUFFER_SIZE 2048
/* Backward-compatible alias for any existing callers. */
#define OTA_HTTP_BUFFER_SIZE OTA_HTTP_TX_BUFFER_SIZE
#define OTA_HTTP_MAX_REDIRECTS 5U
#define OTA_MAX_URL_LENGTH 256U
#define OTA_MAX_VERSION_LENGTH 32U
#define OTA_MAX_SHA256_LENGTH 65U
#define OTA_MAX_MANIFEST_BYTES 2048U

    typedef struct
    {
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

    /**
     * Open an HTTPS request and read its response headers, following 301/302/303/
     * 307/308 redirects. esp_http_client only follows redirects inside
     * esp_http_client_perform(); the open/fetch_headers/read flow used for
     * streaming returns the 30x status itself, which made every GitHub release
     * URL (always a redirect) look like a failure. A redirect that leaves HTTPS is
     * rejected. On ESP_OK the client is positioned at the start of the body.
     */
    esp_err_t ota_http_open_following_redirects(esp_http_client_handle_t client,
                                                int64_t *content_length,
                                                int *status_code);

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
