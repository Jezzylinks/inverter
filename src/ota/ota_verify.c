#include "ota_verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "esp_log.h"
#include "mbedtls/sha256.h"

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

esp_err_t ota_verify_begin(ota_verify_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }
    context->sha_context = NULL;
    context->active = false;

    mbedtls_sha256_context *sha = calloc(1, sizeof(*sha));
    if (!sha) {
        return ESP_ERR_NO_MEM;
    }
    mbedtls_sha256_init(sha);
    if (mbedtls_sha256_starts(sha, 0) != 0) {
        mbedtls_sha256_free(sha);
        free(sha);
        return ESP_FAIL;
    }
    context->sha_context = sha;
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
    mbedtls_sha256_context *sha = (mbedtls_sha256_context *)context->sha_context;
    return mbedtls_sha256_update(sha, data, length) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ota_verify_finish(ota_verify_context_t *context,
                            const char *expected_sha256)
{
    if (!context || !context->active || !context->sha_context ||
        !expected_sha256) {
        return ESP_ERR_INVALID_ARG;
    }
    mbedtls_sha256_context *sha = (mbedtls_sha256_context *)context->sha_context;
    unsigned char digest[32] = {0};
    esp_err_t result = ESP_OK;
    if (mbedtls_sha256_finish(sha, digest) != 0 ||
        !ota_digest_matches(digest, expected_sha256)) {
        ESP_LOGE(TAG, "OTA SHA-256 mismatch");
        result = ESP_ERR_INVALID_CRC;
    }
    mbedtls_sha256_free(sha);
    free(sha);
    context->sha_context = NULL;
    context->active = false;
    return result;
}

void ota_verify_abort(ota_verify_context_t *context)
{
    if (!context || !context->active || !context->sha_context) {
        return;
    }
    mbedtls_sha256_context *sha = (mbedtls_sha256_context *)context->sha_context;
    mbedtls_sha256_free(sha);
    free(sha);
    context->sha_context = NULL;
    context->active = false;
}
