#include "ota/ota_verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "esp_log.h"
#include "mbedtls/md.h"

static const char *TAG = "OTA_SERVICE";

static bool ota_digest_matches(const unsigned char digest[32],
                               const char *expected_hex)
{
    char actual[65] = {0};
    for (size_t i = 0U; i < 32U; ++i) {
        snprintf(&actual[i * 2U], 3U, "%02x", digest[i]);
    }
    return strcasecmp(actual, expected_hex) == 0;
}

/*
 * SHA-256 is driven through the mbedtls_md API on purpose. The ESP32 hardware
 * SHA port (MBEDTLS_SHA256_ALT) uses a larger mbedtls_sha256_context than the
 * stock header, so if this file is compiled without the ESP mbedtls config the
 * sizeof() used for calloc() is 4 bytes too small and the library overruns the
 * block (heap corruption). mbedtls_md_context_t has a fixed layout and the
 * library allocates the real SHA context itself, so it is safe either way.
 */
esp_err_t ota_verify_begin(ota_verify_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }
    context->sha_context = NULL;
    context->active = false;

    mbedtls_md_context_t *md = calloc(1, sizeof(*md));
    if (!md) {
        return ESP_ERR_NO_MEM;
    }
    mbedtls_md_init(md);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL || mbedtls_md_setup(md, info, 0) != 0 ||
        mbedtls_md_starts(md) != 0) {
        mbedtls_md_free(md);
        free(md);
        return ESP_FAIL;
    }
    context->sha_context = md;
    context->active = true;
    return ESP_OK;
}

esp_err_t ota_verify_update(ota_verify_context_t *context,
                            const uint8_t *data, size_t length)
{
    if (!context || !context->active || !context->sha_context ||
        (!data && length != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length == 0U) {
        return ESP_OK;
    }
    mbedtls_md_context_t *md = (mbedtls_md_context_t *)context->sha_context;
    return mbedtls_md_update(md, data, length) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ota_verify_finish(ota_verify_context_t *context,
                            const char *expected_sha256)
{
    if (!context || !context->active || !context->sha_context ||
        !expected_sha256) {
        return ESP_ERR_INVALID_ARG;
    }
    mbedtls_md_context_t *md = (mbedtls_md_context_t *)context->sha_context;
    unsigned char digest[32] = {0};
    esp_err_t result = ESP_OK;
    if (mbedtls_md_finish(md, digest) != 0 ||
        !ota_digest_matches(digest, expected_sha256)) {
        ESP_LOGE(TAG, "OTA SHA-256 mismatch");
        result = ESP_ERR_INVALID_CRC;
    }
    mbedtls_md_free(md);
    free(md);
    context->sha_context = NULL;
    context->active = false;
    return result;
}

void ota_verify_abort(ota_verify_context_t *context)
{
    if (!context || !context->active || !context->sha_context) {
        return;
    }
    mbedtls_md_context_t *md = (mbedtls_md_context_t *)context->sha_context;
    mbedtls_md_free(md);
    free(md);
    context->sha_context = NULL;
    context->active = false;
}
