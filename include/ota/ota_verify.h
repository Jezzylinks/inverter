#ifndef OTA_VERIFY_H
#define OTA_VERIFY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *sha_context;
    bool active;
} ota_verify_context_t;

esp_err_t ota_verify_begin(ota_verify_context_t *context);
esp_err_t ota_verify_update(ota_verify_context_t *context,
                            const uint8_t *data, size_t length);
esp_err_t ota_verify_finish(ota_verify_context_t *context,
                            const char *expected_sha256);
void ota_verify_abort(ota_verify_context_t *context);

#ifdef __cplusplus
}
#endif

#endif /* OTA_VERIFY_H */
