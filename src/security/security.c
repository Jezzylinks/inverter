#include "security/security.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "system/system_state.h"

static const char *TAG = "security";
extern system_state_t sys_state;

#define NVS_KEY_HASH "sec_pin_hash"
#define NVS_KEY_SALT "sec_pin_salt"
#define NVS_KEY_FORCE_CHG "sec_force_chg"
#define SALT_LEN 16U
#define HASH_LEN 32U

typedef struct {
    SemaphoreHandle_t mutex;
    uint8_t attempts[SECURITY_LOCKOUT_SCOPE_COUNT];
    int64_t lockout_until_ms[SECURITY_LOCKOUT_SCOPE_COUNT];
    bool force_change;
    bool initialized;
} security_runtime_t;

static security_runtime_t s_sec;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void secure_zero(void *ptr, size_t len)
{
    volatile uint8_t *bytes = (volatile uint8_t *)ptr;
    while (len-- > 0U) {
        *bytes++ = 0U;
    }
}

static bool pin_is_valid(const uint8_t pin[SECURITY_PIN_LEN])
{
    if (!pin) {
        return false;
    }
    for (size_t i = 0; i < SECURITY_PIN_LEN; ++i) {
        if (pin[i] > 9U) {
            return false;
        }
    }
    return true;
}

static esp_err_t compute_hash(const uint8_t pin[SECURITY_PIN_LEN],
                              const uint8_t salt[SALT_LEN],
                              uint8_t out_hash[HASH_LEN])
{
    if (!pin_is_valid(pin) || !salt || !out_hash) {
        return ESP_ERR_INVALID_ARG;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    int result = mbedtls_sha256_starts(&ctx, 0);
    if (result == 0) {
        result = mbedtls_sha256_update(&ctx, salt, SALT_LEN);
    }
    if (result == 0) {
        result = mbedtls_sha256_update(&ctx, pin, SECURITY_PIN_LEN);
    }
    if (result == 0) {
        result = mbedtls_sha256_finish(&ctx, out_hash);
    }
    mbedtls_sha256_free(&ctx);
    return (result == 0) ? ESP_OK : ESP_FAIL;
}

static bool secure_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t difference = 0U;
    for (size_t i = 0; i < len; ++i) {
        difference |= (uint8_t)(a[i] ^ b[i]);
    }
    return difference == 0U;
}

static esp_err_t persist_pin(const uint8_t pin[SECURITY_PIN_LEN], bool force_change)
{
    if (!pin_is_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t salt[SALT_LEN] = {0};
    uint8_t hash[HASH_LEN] = {0};
    esp_fill_random(salt, sizeof(salt));
    esp_err_t err = compute_hash(pin, salt, hash);
    if (err != ESP_OK) {
        secure_zero(salt, sizeof(salt));
        secure_zero(hash, sizeof(hash));
        return err;
    }

    nvs_handle_t handle = 0;
    err = nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, NVS_KEY_HASH, hash, sizeof(hash));
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, NVS_KEY_SALT, salt, sizeof(salt));
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, NVS_KEY_FORCE_CHG, force_change ? 1U : 0U);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle) {
        nvs_close(handle);
    }

    secure_zero(salt, sizeof(salt));
    secure_zero(hash, sizeof(hash));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist PIN: %s", esp_err_to_name(err));
    }
    return err;
}

static void security_cleanup_failed_init(void)
{
    if (s_sec.mutex) {
        vSemaphoreDelete(s_sec.mutex);
    }
    memset(&s_sec, 0, sizeof(s_sec));
}

esp_err_t security_init(void)
{
    if (s_sec.initialized) {
        return ESP_OK;
    }

    memset(&s_sec, 0, sizeof(s_sec));
    s_sec.mutex = xSemaphoreCreateMutex();
    if (!s_sec.mutex) {
        return ESP_ERR_NO_MEM;
    }

    if (!sys_state.security.enabled) {
        s_sec.initialized = true;
        s_sec.force_change = false;
        ESP_LOGW(TAG, "panel PIN security is disabled by configuration");
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_SYSTEM, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        const uint8_t default_pin[SECURITY_PIN_LEN] = SECURITY_DEFAULT_PIN;
        const esp_err_t provision_err = persist_pin(default_pin, true);
        if (provision_err != ESP_OK) {
            security_cleanup_failed_init();
            return err;
        }
        s_sec.force_change = true;
        memset(s_sec.attempts, 0, sizeof(s_sec.attempts));
        memset(s_sec.lockout_until_ms, 0, sizeof(s_sec.lockout_until_ms));
        s_sec.initialized = true;
        ESP_LOGW(TAG, "settings namespace unavailable; default PIN 0000 provisioned");
        return ESP_OK;
    }

    uint8_t stored_hash[HASH_LEN] = {0};
    uint8_t stored_salt[SALT_LEN] = {0};
    size_t hash_len = sizeof(stored_hash);
    size_t salt_len = sizeof(stored_salt);
    err = nvs_get_blob(handle, NVS_KEY_HASH, stored_hash, &hash_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        const uint8_t default_pin[SECURITY_PIN_LEN] = SECURITY_DEFAULT_PIN;
        err = persist_pin(default_pin, true);
        if (err == ESP_OK) {
            s_sec.force_change = true;
            s_sec.initialized = true;
            ESP_LOGW(TAG, "default PIN provisioned; change it before enabling remote control");
        } else {
            security_cleanup_failed_init();
        }
        secure_zero(stored_hash, sizeof(stored_hash));
        secure_zero(stored_salt, sizeof(stored_salt));
        return err;
    }
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, NVS_KEY_SALT, stored_salt, &salt_len);
    }
    uint8_t force_change = 0U;
    if (err == ESP_OK) {
        const esp_err_t force_err = nvs_get_u8(handle, NVS_KEY_FORCE_CHG, &force_change);
        if (force_err != ESP_OK && force_err != ESP_ERR_NVS_NOT_FOUND) {
            err = force_err;
        }
    }
    nvs_close(handle);

    if (err != ESP_OK || hash_len != HASH_LEN || salt_len != SALT_LEN) {
        ESP_LOGW(TAG, "missing or invalid persisted PIN material; provisioning default 0000");
        secure_zero(stored_hash, sizeof(stored_hash));
        secure_zero(stored_salt, sizeof(stored_salt));

        const uint8_t default_pin[SECURITY_PIN_LEN] = SECURITY_DEFAULT_PIN;
        const esp_err_t provision_err = persist_pin(default_pin, true);
        if (provision_err != ESP_OK) {
            security_cleanup_failed_init();
            return provision_err;
        }

        s_sec.force_change = true;
        memset(s_sec.attempts, 0, sizeof(s_sec.attempts));
        memset(s_sec.lockout_until_ms, 0, sizeof(s_sec.lockout_until_ms));
        s_sec.initialized = true;
        ESP_LOGW(TAG, "default PIN 0000 provisioned; change it before remote access");
        return ESP_OK;
    }

    secure_zero(stored_hash, sizeof(stored_hash));
    secure_zero(stored_salt, sizeof(stored_salt));
    s_sec.force_change = force_change != 0U;
    s_sec.initialized = true;
    return ESP_OK;
}

bool security_verify_pin_for_scope(const uint8_t pin[SECURITY_PIN_LEN],
                                    security_lockout_scope_t scope)
{
    if (!s_sec.initialized || !pin_is_valid(pin) ||
        scope >= SECURITY_LOCKOUT_SCOPE_COUNT) {
        return false;
    }
    /* Protected operations must always verify the persisted hash. The
     * general scope may be bypassed only when panel security is explicitly
     * disabled; factory reset and OTA never accept an arbitrary PIN. */
    if (!sys_state.security.enabled && scope == SECURITY_LOCKOUT_GENERAL) {
        return true;
    }

    if (xSemaphoreTake(s_sec.mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const int64_t now = now_ms();
    if (s_sec.lockout_until_ms[scope] != 0 &&
        now < s_sec.lockout_until_ms[scope]) {
        xSemaphoreGive(s_sec.mutex);
        return false;
    }
    if (s_sec.lockout_until_ms[scope] != 0) {
        s_sec.lockout_until_ms[scope] = 0;
        s_sec.attempts[scope] = 0;
    }
    xSemaphoreGive(s_sec.mutex);

    uint8_t stored_hash[HASH_LEN] = {0};
    uint8_t salt[SALT_LEN] = {0};
    uint8_t candidate_hash[HASH_LEN] = {0};
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_SYSTEM, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t hash_len = sizeof(stored_hash);
        size_t salt_len = sizeof(salt);
        err = nvs_get_blob(handle, NVS_KEY_HASH, stored_hash, &hash_len);
        if (err == ESP_OK) {
            err = nvs_get_blob(handle, NVS_KEY_SALT, salt, &salt_len);
        }
        if (err == ESP_OK && (hash_len != HASH_LEN || salt_len != SALT_LEN)) {
            err = ESP_ERR_INVALID_SIZE;
        }
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        err = compute_hash(pin, salt, candidate_hash);
    }

    const bool match = (err == ESP_OK) && secure_equal(candidate_hash, stored_hash, HASH_LEN);
    secure_zero(stored_hash, sizeof(stored_hash));
    secure_zero(salt, sizeof(salt));
    secure_zero(candidate_hash, sizeof(candidate_hash));

    if (xSemaphoreTake(s_sec.mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (match) {
        s_sec.attempts[scope] = 0;
        s_sec.lockout_until_ms[scope] = 0;
    } else if (s_sec.attempts[scope] < UINT8_MAX) {
        s_sec.attempts[scope]++;
        if (s_sec.attempts[scope] >= SECURITY_MAX_ATTEMPTS) {
            s_sec.lockout_until_ms[scope] = now_ms() + SECURITY_LOCKOUT_MS;
            ESP_LOGW(TAG, "PIN lockout scope %d active for %d ms",
                     (int)scope, SECURITY_LOCKOUT_MS);
        }
    }
    xSemaphoreGive(s_sec.mutex);
    return match;
}

bool security_verify_pin(const uint8_t pin[SECURITY_PIN_LEN])
{
    return security_verify_pin_for_scope(pin, SECURITY_LOCKOUT_GENERAL);
}

esp_err_t security_set_pin(const uint8_t new_pin[SECURITY_PIN_LEN])
{
    if (!s_sec.initialized || !pin_is_valid(new_pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = persist_pin(new_pin, false);
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_sec.mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_sec.force_change = false;
    memset(s_sec.attempts, 0, sizeof(s_sec.attempts));
    memset(s_sec.lockout_until_ms, 0, sizeof(s_sec.lockout_until_ms));
    xSemaphoreGive(s_sec.mutex);
    return ESP_OK;
}

bool security_is_locked_out_for_scope(security_lockout_scope_t scope)
{
    if (!s_sec.initialized || !s_sec.mutex || scope >= SECURITY_LOCKOUT_SCOPE_COUNT) {
        return false;
    }
    xSemaphoreTake(s_sec.mutex, portMAX_DELAY);
    const int64_t now = now_ms();
    const bool locked = s_sec.lockout_until_ms[scope] != 0 &&
                        now < s_sec.lockout_until_ms[scope];
    if (!locked && s_sec.lockout_until_ms[scope] != 0) {
        s_sec.lockout_until_ms[scope] = 0;
        s_sec.attempts[scope] = 0;
    }
    xSemaphoreGive(s_sec.mutex);
    return locked;
}

int64_t security_lockout_remaining_ms_for_scope(security_lockout_scope_t scope)
{
    if (!s_sec.initialized || !s_sec.mutex || scope >= SECURITY_LOCKOUT_SCOPE_COUNT) {
        return 0;
    }
    xSemaphoreTake(s_sec.mutex, portMAX_DELAY);
    const int64_t remaining = (s_sec.lockout_until_ms[scope] > now_ms())
                                  ? (s_sec.lockout_until_ms[scope] - now_ms())
                                  : 0;
    xSemaphoreGive(s_sec.mutex);
    return remaining;
}

bool security_is_locked_out(void)
{
    return security_is_locked_out_for_scope(SECURITY_LOCKOUT_GENERAL);
}

int64_t security_lockout_remaining_ms(void)
{
    return security_lockout_remaining_ms_for_scope(SECURITY_LOCKOUT_GENERAL);
}

bool security_pin_change_required(void)
{
    if (!s_sec.initialized || !s_sec.mutex) {
        return false;
    }
    xSemaphoreTake(s_sec.mutex, portMAX_DELAY);
    const bool required = s_sec.force_change;
    xSemaphoreGive(s_sec.mutex);
    return required;
}

void security_reset_attempts(void)
{
    if (!s_sec.initialized || !s_sec.mutex) {
        return;
    }
    xSemaphoreTake(s_sec.mutex, portMAX_DELAY);
    memset(s_sec.attempts, 0, sizeof(s_sec.attempts));
    memset(s_sec.lockout_until_ms, 0, sizeof(s_sec.lockout_until_ms));
    xSemaphoreGive(s_sec.mutex);
}

void security_reset_scope_attempts(security_lockout_scope_t scope)
{
    if (!s_sec.initialized || !s_sec.mutex ||
        scope >= SECURITY_LOCKOUT_SCOPE_COUNT) {
        return;
    }
    xSemaphoreTake(s_sec.mutex, portMAX_DELAY);
    s_sec.attempts[scope] = 0U;
    s_sec.lockout_until_ms[scope] = 0;
    xSemaphoreGive(s_sec.mutex);
}

uint8_t security_attempts_remaining_for_scope(security_lockout_scope_t scope)
{
    if (!s_sec.initialized || !s_sec.mutex ||
        scope >= SECURITY_LOCKOUT_SCOPE_COUNT) {
        return 0U;
    }

    xSemaphoreTake(s_sec.mutex, portMAX_DELAY);
    const uint8_t attempts = s_sec.attempts[scope];
    const uint8_t remaining = (attempts >= SECURITY_MAX_ATTEMPTS)
                                  ? 0U
                                  : (uint8_t)(SECURITY_MAX_ATTEMPTS - attempts);
    xSemaphoreGive(s_sec.mutex);
    return remaining;
}
