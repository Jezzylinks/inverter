/**
 * @file wifi_security.c
 * @brief Wi-Fi Security Layer
 */

#include "wifi/wifi_security.h"
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

#include "mbedtls/sha256.h"

#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "WIFI_SECURITY";

/*----------------------------------------------------------
 *
 * PRIVATE HELPERS
 *
 *---------------------------------------------------------*/

static esp_err_t wifi_security_open(nvs_handle_t *handle,
                                    nvs_open_mode_t mode)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return nvs_open(WIFI_SECURITY_NAMESPACE,
                    mode,
                    handle);
}

static esp_err_t wifi_security_commit_close(nvs_handle_t handle)
{
    esp_err_t err = nvs_commit(handle);

    nvs_close(handle);

    return err;
}

/*----------------------------------------------------------
 *
 * INITIALIZATION
 *
 *---------------------------------------------------------*/

esp_err_t wifi_security_init(void)
{
    esp_err_t err;

    err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());

        err = nvs_flash_init();
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "NVS Init Failed (%s)",
                 esp_err_to_name(err));

        return err;
    }

    ESP_LOGI(TAG,
             "WiFi Security Initialized");

    return ESP_OK;
}

/*----------------------------------------------------------
 *
 * ROOT CA
 *
 *---------------------------------------------------------*/

esp_err_t wifi_security_save_root_ca(
    const char *certificate)
{
    if (certificate == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t err =
        wifi_security_open(&handle,
                           NVS_READWRITE);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_str(handle,
                      WIFI_KEY_ROOT_CA,
                      certificate);

    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = wifi_security_commit_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "Root CA Saved");
    }

    return err;
}

esp_err_t wifi_security_load_root_ca(
    char *buffer,
    size_t length)
{
    if (buffer == NULL ||
        length == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t err =
        wifi_security_open(&handle,
                           NVS_READONLY);

    if (err != ESP_OK)
    {
        return err;
    }

    size_t required = length;

    err = nvs_get_str(handle,
                      WIFI_KEY_ROOT_CA,
                      buffer,
                      &required);

    nvs_close(handle);

    return err;
}

esp_err_t wifi_security_delete_root_ca(void)
{
    nvs_handle_t handle;

    esp_err_t err =
        wifi_security_open(&handle,
                           NVS_READWRITE);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_erase_key(handle,
                        WIFI_KEY_ROOT_CA);

    if (err != ESP_OK &&
        err != ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(handle);
        return err;
    }

    return wifi_security_commit_close(handle);
}

bool wifi_security_has_root_ca(void)
{
    char dummy[2];

    return (wifi_security_load_root_ca(dummy,
                                       sizeof(dummy)) == ESP_OK);
}

/*----------------------------------------------------------
 *
 * CLIENT CERTIFICATE
 *
 *---------------------------------------------------------*/

esp_err_t wifi_security_save_client_certificate(
    const char *certificate)
{
    if (certificate == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t err =
        wifi_security_open(&handle,
                           NVS_READWRITE);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_str(handle,
                      WIFI_KEY_CLIENT_CERT,
                      certificate);

    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = wifi_security_commit_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "Client certificate saved");
    }

    return err;
}

esp_err_t wifi_security_load_client_certificate(
    char *buffer,
    size_t length)
{
    if (buffer == NULL || length == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t err =
        wifi_security_open(&handle,
                           NVS_READONLY);

    if (err != ESP_OK)
    {
        return err;
    }

    size_t required = length;

    err = nvs_get_str(handle,
                      WIFI_KEY_CLIENT_CERT,
                      buffer,
                      &required);

    nvs_close(handle);

    return err;
}

esp_err_t wifi_security_delete_client_certificate(void)
{
    nvs_handle_t handle;

    esp_err_t err =
        wifi_security_open(&handle,
                           NVS_READWRITE);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_erase_key(handle,
                        WIFI_KEY_CLIENT_CERT);

    if (err != ESP_OK &&
        err != ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(handle);
        return err;
    }

    return wifi_security_commit_close(handle);
}

bool wifi_security_has_client_certificate(void)
{
    char dummy[2];

    return (wifi_security_load_client_certificate(
                dummy,
                sizeof(dummy)) == ESP_OK);
}

/*----------------------------------------------------------
 *
 * PRIVATE KEY
 *
 *---------------------------------------------------------*/

esp_err_t wifi_security_save_private_key(
    const char *key)
{
    if (key == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t err =
        wifi_security_open(&handle,
                           NVS_READWRITE);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_str(handle,
                      WIFI_KEY_PRIVATE_KEY,
                      key);

    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = wifi_security_commit_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "Private key saved");
    }

    return err;
}

esp_err_t wifi_security_load_private_key(
    char *buffer,
    size_t length)
{
    if (buffer == NULL || length == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t err =
        wifi_security_open(&handle,
                           NVS_READONLY);

    if (err != ESP_OK)
    {
        return err;
    }

    size_t required = length;

    err = nvs_get_str(handle,
                      WIFI_KEY_PRIVATE_KEY,
                      buffer,
                      &required);

    nvs_close(handle);

    return err;
}

esp_err_t wifi_security_delete_private_key(void)
{
    nvs_handle_t handle;

    esp_err_t err =
        wifi_security_open(&handle,
                           NVS_READWRITE);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_erase_key(handle,
                        WIFI_KEY_PRIVATE_KEY);

    if (err != ESP_OK &&
        err != ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(handle);
        return err;
    }

    return wifi_security_commit_close(handle);
}

bool wifi_security_has_private_key(void)
{
    char dummy[2];

    return (wifi_security_load_private_key(
                dummy,
                sizeof(dummy)) == ESP_OK);
}

/*----------------------------------------------------------
 *
 * SHA256 HASH
 *
 *---------------------------------------------------------*/

esp_err_t wifi_security_sha256(
    const uint8_t *input,
    size_t length,
    uint8_t hash[WIFI_SHA256_LENGTH])
{
    if (input == NULL || hash == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    mbedtls_sha256_context ctx;

    mbedtls_sha256_init(&ctx);

#if MBEDTLS_VERSION_MAJOR >= 3

    if (mbedtls_sha256_starts(&ctx, 0) != 0)
    {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }

#else

    if (mbedtls_sha256_starts_ret(&ctx, 0) != 0)
    {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }

#endif

#if MBEDTLS_VERSION_MAJOR >= 3

    if (mbedtls_sha256_update(&ctx, input, length) != 0)
    {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }

#else

    if (mbedtls_sha256_update_ret(&ctx, input, length) != 0)
    {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }

#endif

#if MBEDTLS_VERSION_MAJOR >= 3

    if (mbedtls_sha256_finish(&ctx, hash) != 0)
    {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }

#else

    if (mbedtls_sha256_finish_ret(&ctx, hash) != 0)
    {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }

#endif

    mbedtls_sha256_free(&ctx);

    return ESP_OK;
}

/*----------------------------------------------------------
 *
 * CRYPTOGRAPHIC RANDOM
 *
 *---------------------------------------------------------*/

esp_err_t wifi_security_random(
    uint8_t *buffer,
    size_t length)
{
    if (buffer == NULL || length == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_fill_random(buffer, length);

    return ESP_OK;
}

/*----------------------------------------------------------
 *
 * FACTORY RESET
 *
 *---------------------------------------------------------*/

esp_err_t wifi_security_factory_reset(void)
{
    nvs_handle_t handle;

    esp_err_t err =
        wifi_security_open(&handle,
                           NVS_READWRITE);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_erase_all(handle);

    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }

    err = wifi_security_commit_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "WiFi security storage erased");
    }

    return err;
}