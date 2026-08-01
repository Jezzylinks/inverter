/**
 * @file wifi_security.h
 * @brief Wi-Fi Security Layer
 *
 * Supports:
 *  - Root CA storage
 *  - Client certificate
 *  - Client private key
 *  - Device hostname
 *  - Secure random generation
 *  - SHA256 hashing
 *  - TLS credential management
 */

#ifndef WIFI_SECURITY_H
#define WIFI_SECURITY_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

    /*=========================================================
     *
     *                  CONSTANTS
     *
     *========================================================*/

#define WIFI_SECURITY_NAMESPACE "wifi_sec"

#define WIFI_KEY_ROOT_CA "root_ca"
#define WIFI_KEY_CLIENT_CERT "client_cert"
#define WIFI_KEY_PRIVATE_KEY "private_key"
#define WIFI_KEY_DEVICE_NAME "device_name"

#define WIFI_SHA256_LENGTH 32

#define WIFI_RANDOM_LENGTH 32

    /*=========================================================
     *
     *                  TYPES
     *
     *========================================================*/

    typedef enum
    {
        WIFI_SECURITY_NONE = 0,

        WIFI_SECURITY_TLS,

        WIFI_SECURITY_MTLS

    } wifi_security_mode_t;

    typedef struct
    {
        char *root_ca;

        char *client_cert;

        char *private_key;

        wifi_security_mode_t mode;

    } wifi_security_context_t;

    /*=========================================================
     *
     *                  INITIALIZATION
     *
     *========================================================*/

    esp_err_t wifi_security_init(void);

    /*=========================================================
     *
     *          ROOT CERTIFICATE
     *
     *========================================================*/

    esp_err_t wifi_security_save_root_ca(
        const char *certificate);

    esp_err_t wifi_security_load_root_ca(
        char *buffer,
        size_t length);

    esp_err_t wifi_security_delete_root_ca(void);

    /*=========================================================
     *
     *          CLIENT CERTIFICATE
     *
     *========================================================*/

    esp_err_t wifi_security_save_client_certificate(
        const char *certificate);

    esp_err_t wifi_security_load_client_certificate(
        char *buffer,
        size_t length);

    esp_err_t wifi_security_delete_client_certificate(void);

    /*=========================================================
     *
     *          PRIVATE KEY
     *
     *========================================================*/

    esp_err_t wifi_security_save_private_key(
        const char *key);

    esp_err_t wifi_security_load_private_key(
        char *buffer,
        size_t length);

    esp_err_t wifi_security_delete_private_key(void);

    /*=========================================================
     *
     *          HASHING
     *
     *========================================================*/

    esp_err_t wifi_security_sha256(
        const uint8_t *input,
        size_t length,
        uint8_t hash[WIFI_SHA256_LENGTH]);

    /*=========================================================
     *
     *          RANDOM
     *
     *========================================================*/

    esp_err_t wifi_security_random(
        uint8_t *buffer,
        size_t length);

    /*=========================================================
     *
     *          STATUS
     *
     *========================================================*/

    bool wifi_security_has_root_ca(void);

    bool wifi_security_has_client_certificate(void);

    bool wifi_security_has_private_key(void);

    /*=========================================================
     *
     *          ERASE
     *
     *========================================================*/

    esp_err_t wifi_security_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif