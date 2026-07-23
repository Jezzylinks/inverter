#include "security.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "system_state.h"

static const char *TAG = "security";
extern system_state_t sys_state;

#define NVS_NAMESPACE "security"
#define NVS_KEY_HASH "sec_pin_hash"
#define NVS_KEY_SALT "sec_pin_salt"
#define NVS_KEY_FORCE_CHG "sec_force_chg"

#define SALT_LEN 16
#define HASH_LEN 32 // SHA-256 output

// ---- internal state (RAM only, not persisted across reboot by design) ----
static struct
{
    SemaphoreHandle_t mutex;
    uint8_t attempts;
    int64_t lockout_until_ms; // 0 = not locked out
    bool force_change;
    bool initialized;
} s_sec;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static esp_err_t compute_hash(const uint8_t pin[SECURITY_PIN_LEN],
                              const uint8_t salt[SALT_LEN],
                              uint8_t out_hash[HASH_LEN])
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0)
    { // 0 = SHA-256 (not 224)
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }
    mbedtls_sha256_update(&ctx, salt, SALT_LEN);
    mbedtls_sha256_update(&ctx, pin, SECURITY_PIN_LEN);
    if (mbedtls_sha256_finish(&ctx, out_hash) != 0)
    {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }
    mbedtls_sha256_free(&ctx);
    return ESP_OK;
}

static esp_err_t store_pin(const uint8_t pin[SECURITY_PIN_LEN])
{
    uint8_t salt[SALT_LEN];
    esp_fill_random(salt, SALT_LEN);

    uint8_t hash[HASH_LEN];
    esp_err_t err = compute_hash(pin, salt, hash);
    if (err != ESP_OK)
    {
        return err;
    }

    nvs_handle_t h;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(h, NVS_KEY_HASH, hash, HASH_LEN);
    if (err == ESP_OK)
    {
        err = nvs_set_blob(h, NVS_KEY_SALT, salt, SALT_LEN);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to persist PIN: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t set_force_change_flag(bool force)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_FORCE_CHG, force ? 1 : 0);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t security_init(void)
{
    if (s_sec.initialized)
    {
        return ESP_OK;
    }

    s_sec.mutex = xSemaphoreCreateMutex();
    if (s_sec.mutex == NULL)
    {
        ESP_LOGE(TAG, "failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    s_sec.attempts = 0;
    s_sec.lockout_until_ms = 0;

    /* security_en is loaded by nvs_load_all() before this function runs.
     * If security is disabled, mark initialized but skip PIN provisioning. */
    if (!sys_state.security.enabled)
    {
        ESP_LOGI(TAG, "security disabled, skipping PIN init");
        s_sec.initialized = true;
        s_sec.force_change = false;
        return ESP_OK;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t hash[HASH_LEN];
    size_t hash_len = sizeof(hash);
    err = nvs_get_blob(h, NVS_KEY_HASH, hash, &hash_len);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        // First boot: provision default PIN, force a change.
        ESP_LOGW(TAG, "no PIN found, provisioning default PIN");
        nvs_close(h);

        const uint8_t default_pin[SECURITY_PIN_LEN] = SECURITY_DEFAULT_PIN;
        esp_err_t store_err = store_pin(default_pin);
        if (store_err != ESP_OK)
        {
            return store_err;
        }
        esp_err_t flag_err = set_force_change_flag(true);
        if (flag_err != ESP_OK)
        {
            return flag_err;
        }
        s_sec.force_change = true;
        s_sec.initialized = true;
        return ESP_OK;
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to read PIN hash: %s", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }

    uint8_t force_chg_u8 = 0;
    esp_err_t flag_err = nvs_get_u8(h, NVS_KEY_FORCE_CHG, &force_chg_u8);
    // Missing key just means "not required" (older provisioning); not an error.
    s_sec.force_change = (flag_err == ESP_OK) && (force_chg_u8 != 0);

    nvs_close(h);
    s_sec.initialized = true;
    ESP_LOGI(TAG, "security module initialized, force_change=%d", s_sec.force_change);
    return ESP_OK;
}

bool security_verify_pin(const uint8_t pin[SECURITY_PIN_LEN])
{
    if (!s_sec.initialized)
    {
        ESP_LOGE(TAG, "security_verify_pin called before security_init");
        return false;
    }

    xSemaphoreTake(s_sec.mutex, portMAX_DELAY);

    if (s_sec.lockout_until_ms != 0)
    {
        if (now_ms() < s_sec.lockout_until_ms)
        {
            xSemaphoreGive(s_sec.mutex);
            return false; // locked out, do not consume an attempt
        }
        // lockout expired
        s_sec.lockout_until_ms = 0;
        s_sec.attempts = 0;
    }

    xSemaphoreGive(s_sec.mutex); // release before NVS I/O + hashing (not touching shared state)

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t stored_hash[HASH_LEN];
    uint8_t salt[SALT_LEN];
    size_t hash_len = sizeof(stored_hash);
    size_t salt_len = sizeof(salt);

    err = nvs_get_blob(h, NVS_KEY_HASH, stored_hash, &hash_len);
    if (err == ESP_OK)
    {
        err = nvs_get_blob(h, NVS_KEY_SALT, salt, &salt_len);
    }
    nvs_close(h);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to load PIN for verification: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t candidate_hash[HASH_LEN];
    if (compute_hash(pin, salt, candidate_hash) != ESP_OK)
    {
        return false;
    }

    bool match = (memcmp(candidate_hash, stored_hash, HASH_LEN) == 0);

    xSemaphoreTake(s_sec.mutex, portMAX_DELAY);
    if (match)
    {
        s_sec.attempts = 0;
        s_sec.lockout_until_ms = 0;
    }
    else
    {
        s_sec.attempts++;
        if (s_sec.attempts >= SECURITY_MAX_ATTEMPTS)
        {
            s_sec.lockout_until_ms = now_ms() + SECURITY_LOCKOUT_MS;
            ESP_LOGW(TAG, "max PIN attempts reached, locking out for %d ms", SECURITY_LOCKOUT_MS);
        }
    }
    xSemaphoreGive(s_sec.mutex);

    return match;
}

esp_err_t security_set_pin(const uint8_t new_pin[SECURITY_PIN_LEN])
{
    esp_err_t err = store_pin(new_pin);
    if (err != ESP_OK)
    {
        return err;
    }
    err = set_force_change_flag(false);
    if (err != ESP_OK)
    {
        return err;
    }

    xSemaphoreTake(s_sec.mutex, portMAX_DELAY);
    s_sec.force_change = false;
    s_sec.attempts = 0;
    s_sec.lockout_until_ms = 0;
    xSemaphoreGive(s_sec.mutex);

    return ESP_OK;
}

bool security_is_locked_out(void)
{
    xSemaphoreTake(s_sec.mutex, portMAX_DELAY);
    bool locked = (s_sec.lockout_until_ms != 0) && (now_ms() < s_sec.lockout_until_ms);
    xSemaphoreGive(s_sec.mutex);
    return locked;
}

int64_t security_lockout_remaining_ms(void)
{
    xSemaphoreTake(s_sec.mutex, portMAX_DELAY);
    int64_t remaining = 0;
    if (s_sec.lockout_until_ms != 0)
    {
        int64_t delta = s_sec.lockout_until_ms - now_ms();
        remaining = (delta > 0) ? delta : 0;
    }
    xSemaphoreGive(s_sec.mutex);
    return remaining;
}

bool security_pin_change_required(void)
{
    xSemaphoreTake(s_sec.mutex, portMAX_DELAY);
    bool force = s_sec.force_change;
    xSemaphoreGive(s_sec.mutex);
    return force;
}

void security_reset_attempts(void)
{
    xSemaphoreTake(s_sec.mutex, portMAX_DELAY);
    s_sec.attempts = 0;
    s_sec.lockout_until_ms = 0;
    xSemaphoreGive(s_sec.mutex);
}