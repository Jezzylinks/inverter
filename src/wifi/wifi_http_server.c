/**
 * @file wifi_http_server.c
 * @brief Hardened local HTTP server for Wi-Fi provisioning.
 */
#include "wifi/wifi_http_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "wifi/wifi_manager.h"
#include "wifi/wifi_scan.h"
#include "wifi/wifi_storage.h"
#include "wifi/wifi_web_pages.h"
#include "security/security.h"

#define WIFI_HTTP_TAG "WIFI_HTTP"
#define WIFI_HTTP_SCAN_BYTES 4096U
#define WIFI_HTTP_CALLBACK_STACK 3072U
#define WIFI_HTTP_CALLBACK_PRIORITY 4U
#define WIFI_HTTPS_CERT_FILE "https_server.crt"
#define WIFI_HTTPS_KEY_FILE "https_server.key"
#define WIFI_HTTPS_MAX_PEM_BYTES 8192U
#define WIFI_HTTPS_SPIFFS_PARTITION "spiffs"
/* Where this module mounts the partition itself, and where the dashboard
 * (web_dashboard_server.c) mounts the same partition. Both are searched so the
 * PEM files are found whichever module owns the mount. */
#define WIFI_HTTPS_SPIFFS_BASE "/spiffs"
#define WIFI_HTTPS_ALT_BASE "/www"

static httpd_handle_t s_server;
static httpd_handle_t s_https_server;
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_scan_guard;
static int64_t s_last_scan_us;
static wifi_http_save_callback_t s_save_callback;

static esp_err_t root_handler(httpd_req_t *req);
static esp_err_t save_handler(httpd_req_t *req);
static esp_err_t status_handler(httpd_req_t *req);
static esp_err_t scan_handler(httpd_req_t *req);
static esp_err_t reset_handler(httpd_req_t *req);
static esp_err_t redirect_handler(httpd_req_t *req);
static esp_err_t captive_redirect_handler(httpd_req_t *req);
static esp_err_t http_landing_handler(httpd_req_t *req);
static esp_err_t http_sensitive_action_blocked(httpd_req_t *req);
static esp_err_t wifi_https_register_routes(httpd_handle_t server);
static void wifi_http_lock(void);
static void wifi_http_unlock(void);

/* Overwrite a sensitive buffer in a way the compiler cannot optimise away. */
static void wifi_http_wipe(void *buffer, size_t length)
{
    volatile unsigned char *p = (volatile unsigned char *)buffer;
    while (p != NULL && length-- > 0U) {
        *p++ = 0U;
    }
}

/* Mounts the SPIFFS partition that holds the device-local certificate and
 * key. *mounted_here is true only when this call registered the filesystem, so
 * the caller knows it must unregister it again (otherwise the dashboard cannot
 * mount the same partition after provisioning ends). */
static esp_err_t wifi_https_mount_spiffs(bool *mounted_here)
{
    *mounted_here = false;

    const esp_vfs_spiffs_conf_t conf = {
        .base_path = WIFI_HTTPS_SPIFFS_BASE,
        .partition_label = WIFI_HTTPS_SPIFFS_PARTITION,
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_OK) {
        *mounted_here = true;
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        /* Another module (e.g. the dashboard, at /www) already owns the mount. */
        size_t total = 0U, used = 0U;
        err = esp_spiffs_info(WIFI_HTTPS_SPIFFS_PARTITION, &total, &used);
    }
    return err;
}

static char *wifi_https_read_pem(const char *path, size_t *out_len)
{
    struct stat st;
    if (path == NULL || out_len == NULL || stat(path, &st) != 0 ||
        st.st_size <= 0 || (size_t)st.st_size > WIFI_HTTPS_MAX_PEM_BYTES) {
        return NULL;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    const size_t size = (size_t)st.st_size;
    char *buffer = calloc(size + 1U, 1U);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    const size_t read_count = fread(buffer, 1U, size, file);
    fclose(file);
    if (read_count != size || strstr(buffer, "-----BEGIN ") == NULL ||
        strstr(buffer, "-----END ") == NULL) {
        wifi_http_wipe(buffer, size + 1U);
        free(buffer);
        return NULL;
    }
    *out_len = size + 1U;
    return buffer;
}

/* Looks for <dir>/<name> under every known mount point. */
static char *wifi_https_read_pem_file(const char *name, size_t *out_len)
{
    static const char *const dirs[] = {WIFI_HTTPS_SPIFFS_BASE, WIFI_HTTPS_ALT_BASE};
    for (size_t i = 0U; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        char path[48];
        const int written = snprintf(path, sizeof(path), "%s/%s", dirs[i], name);
        if (written <= 0 || (size_t)written >= sizeof(path)) {
            continue;
        }
        char *pem = wifi_https_read_pem(path, out_len);
        if (pem != NULL) {
            return pem;
        }
    }
    return NULL;
}

static void wifi_https_free_pem(char *pem)
{
    if (pem != NULL) {
        wifi_http_wipe(pem, strlen(pem));
        free(pem);
    }
}

static esp_err_t wifi_https_start_optional(void)
{
    bool mounted_here = false;
    esp_err_t err = wifi_https_mount_spiffs(&mounted_here);
    if (err != ESP_OK) {
        ESP_LOGW(WIFI_HTTP_TAG,
                 "HTTPS unavailable: SPIFFS mount failed (%s); HTTP captive portal remains available",
                 esp_err_to_name(err));
        return err;
    }

    size_t cert_len = 0U, key_len = 0U;
    char *cert_pem = wifi_https_read_pem_file(WIFI_HTTPS_CERT_FILE, &cert_len);
    char *key_pem = wifi_https_read_pem_file(WIFI_HTTPS_KEY_FILE, &key_len);
    if (mounted_here) {
        /* The PEM data is in RAM now. Release the filesystem so the dashboard
         * can mount the same partition once provisioning has ended. */
        (void)esp_vfs_spiffs_unregister(WIFI_HTTPS_SPIFFS_PARTITION);
    }
    if (cert_pem == NULL || key_pem == NULL) {
        ESP_LOGW(WIFI_HTTP_TAG,
                 "HTTPS certificate/key missing or invalid; create device-local PEM files and upload SPIFFS");
        wifi_https_free_pem(cert_pem);
        wifi_https_free_pem(key_pem);
        return ESP_ERR_NOT_FOUND;
    }

    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.port_secure = WIFI_HTTPS_SERVER_PORT;
    config.httpd.max_uri_handlers = 13U;
    config.httpd.max_open_sockets = 2U;
    config.httpd.stack_size = 10240U;
    config.httpd.lru_purge_enable = true;
    /* Needed so the wildcard catch-all route matches; without it the URI is compared literally. */
    config.httpd.uri_match_fn = httpd_uri_match_wildcard;
    config.servercert = (const uint8_t *)cert_pem;
    config.servercert_len = cert_len;
    config.prvtkey_pem = (const uint8_t *)key_pem;
    config.prvtkey_len = key_len;

    httpd_handle_t server = NULL;
    err = httpd_ssl_start(&server, &config);
    /* httpd_ssl_start() copies the PEM data (ESP-IDF 5.x), so our copies are
     * no longer needed. Wipe and free them immediately. */
    wifi_https_free_pem(cert_pem);
    wifi_https_free_pem(key_pem);
    if (err != ESP_OK) {
        ESP_LOGW(WIFI_HTTP_TAG,
                 "HTTPS server start failed: %s; HTTP captive portal remains available",
                 esp_err_to_name(err));
        return err;
    }

    err = wifi_https_register_routes(server);
    if (err != ESP_OK) {
        (void)httpd_ssl_stop(server);
        return err;
    }
    wifi_http_lock();
    s_https_server = server;
    wifi_http_unlock();
    ESP_LOGI(WIFI_HTTP_TAG, "HTTPS provisioning server started on port %u",
             WIFI_HTTPS_SERVER_PORT);
    return ESP_OK;
}

static const httpd_uri_t s_root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_handler};
static const httpd_uri_t s_http_root_uri = {.uri = "/", .method = HTTP_GET, .handler = http_landing_handler};
static const httpd_uri_t s_http_save_uri = {.uri = "/save", .method = HTTP_POST, .handler = http_sensitive_action_blocked};
static const httpd_uri_t s_http_status_uri = {.uri = "/status", .method = HTTP_GET, .handler = http_sensitive_action_blocked};
static const httpd_uri_t s_http_scan_uri = {.uri = "/scan", .method = HTTP_GET, .handler = http_sensitive_action_blocked};
static const httpd_uri_t s_http_reset_uri = {.uri = "/reset", .method = HTTP_POST, .handler = http_sensitive_action_blocked};
static const httpd_uri_t s_save_uri = {.uri = "/save", .method = HTTP_POST, .handler = save_handler};
static const httpd_uri_t s_status_uri = {.uri = "/status", .method = HTTP_GET, .handler = status_handler};
static const httpd_uri_t s_scan_uri = {.uri = "/scan", .method = HTTP_GET, .handler = scan_handler};
static const httpd_uri_t s_reset_uri = {.uri = "/reset", .method = HTTP_POST, .handler = reset_handler};
static const httpd_uri_t s_generate_204_uri = {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_redirect_handler};
static const httpd_uri_t s_gen_204_uri = {.uri = "/gen_204", .method = HTTP_GET, .handler = captive_redirect_handler};
static const httpd_uri_t s_hotspot_uri = {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect_handler};
static const httpd_uri_t s_success_html_uri = {.uri = "/library/test/success.html", .method = HTTP_GET, .handler = captive_redirect_handler};
static const httpd_uri_t s_ncsi_uri = {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_redirect_handler};
static const httpd_uri_t s_connecttest_uri = {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_redirect_handler};
static const httpd_uri_t s_success_txt_uri = {.uri = "/success.txt", .method = HTTP_GET, .handler = captive_redirect_handler};
static const httpd_uri_t s_redirect_uri = {.uri = "/*", .method = HTTP_GET, .handler = redirect_handler};

static esp_err_t wifi_https_register_routes(httpd_handle_t server)
{
    const httpd_uri_t *const routes[] = {
        &s_root_uri, &s_save_uri, &s_status_uri, &s_scan_uri, &s_reset_uri,
        &s_generate_204_uri, &s_gen_204_uri, &s_hotspot_uri, &s_success_html_uri,
        &s_ncsi_uri, &s_connecttest_uri, &s_success_txt_uri, &s_redirect_uri,
    };
    for (size_t i = 0U; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        const esp_err_t err = httpd_register_uri_handler(server, routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(WIFI_HTTP_TAG, "HTTPS route registration failed for %s: %s",
                     routes[i]->uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}


static void wifi_http_lock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void wifi_http_unlock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

static void wifi_http_set_security_headers(httpd_req_t *req)
{
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    (void)httpd_resp_set_hdr(req, "Pragma", "no-cache");
    (void)httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    (void)httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    (void)httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    (void)httpd_resp_set_hdr(req, "Content-Security-Policy",
                             "default-src 'self'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'");
}

static esp_err_t wifi_http_send_text(httpd_req_t *req, const char *status,
                                     const char *content_type, const char *body)
{
    wifi_http_set_security_headers(req);
    if (status != NULL) {
        (void)httpd_resp_set_status(req, status);
    }
    (void)httpd_resp_set_type(req, content_type);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t wifi_http_send_error(httpd_req_t *req, const char *status,
                                      const char *message)
{
    return wifi_http_send_text(req, status, "text/plain; charset=utf-8", message);
}

static esp_err_t wifi_http_redirect_home(httpd_req_t *req)
{
    wifi_http_set_security_headers(req);
    (void)httpd_resp_set_status(req, "302 Found");
    (void)httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static bool wifi_http_valid_text(const char *text, size_t capacity, bool required)
{
    const size_t length = strnlen(text, capacity);
    if ((required && length == 0U) || length >= capacity) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        const unsigned char value = (unsigned char)text[i];
        if (value < 0x20U || value == 0x7FU) {
            return false;
        }
    }
    return true;
}

static int wifi_http_hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Decodes application/x-www-form-urlencoded text in place ('+' becomes a
 * space, %XX becomes that byte). httpd_query_key_value() does NOT decode, so
 * without this an SSID such as "My WiFi" is stored as "My+WiFi" and a password
 * such as "p@ss" as "p%40ss". Returns false on a malformed escape or an encoded
 * NUL. The output is never longer than the input. */
static bool wifi_http_url_decode(char *text)
{
    char *out = text;
    for (const char *in = text; *in != '\0'; ++in) {
        unsigned char value = (unsigned char)*in;
        if (*in == '+') {
            value = (unsigned char)' ';
        } else if (*in == '%') {
            const int high = wifi_http_hex_value(in[1]);
            const int low = (high >= 0) ? wifi_http_hex_value(in[2]) : -1;
            if (high < 0 || low < 0) {
                return false;
            }
            value = (unsigned char)((high << 4) | low);
            if (value == 0U) {
                return false;
            }
            in += 2;
        }
        *out++ = (char)value;
    }
    *out = '\0';
    return true;
}

static bool wifi_http_verify_pin(const char *form)
{
    char pin_text[SECURITY_PIN_LEN + 1U] = {0};
    if (!form || httpd_query_key_value(form, "pin", pin_text,
                                       sizeof(pin_text)) != ESP_OK ||
        strlen(pin_text) != SECURITY_PIN_LEN) {
        return false;
    }

    uint8_t pin[SECURITY_PIN_LEN] = {0};
    for (size_t i = 0U; i < SECURITY_PIN_LEN; ++i) {
        if (pin_text[i] < '0' || pin_text[i] > '9') {
            memset(pin, 0, sizeof(pin));
            return false;
        }
        pin[i] = (uint8_t)(pin_text[i] - '0');
    }
    /* Dedicated scope: failed web attempts must not lock the front panel. */
    const bool valid = security_verify_pin_for_scope(
        pin, SECURITY_LOCKOUT_PROVISIONING);
    memset(pin, 0, sizeof(pin));
    memset(pin_text, 0, sizeof(pin_text));
    return valid;
}

static esp_err_t wifi_http_receive_form(httpd_req_t *req, char **form)
{
    if (req->content_len <= 0 || req->content_len > (int)WIFI_HTTP_MAX_FORM_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t length = (size_t)req->content_len;
    char *buffer = calloc(length + 1U, 1U);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0U;
    while (received < length) {
        const int result = httpd_req_recv(req, buffer + received, length - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            free(buffer);
            return ESP_ERR_TIMEOUT;
        }
        if (result <= 0) {
            free(buffer);
            return ESP_FAIL;
        }
        received += (size_t)result;
    }

    buffer[length] = '\0';
    *form = buffer;
    return ESP_OK;
}

static void wifi_http_save_callback_task(void *arg)
{
    (void)arg;
    wifi_http_lock();
    wifi_http_save_callback_t callback = s_save_callback;
    wifi_http_unlock();
    if (callback != NULL) {
        callback();
    }
    vTaskDelete(NULL);
}

static esp_err_t wifi_http_schedule_save_callback(void)
{
    wifi_http_save_callback_t callback = NULL;
    wifi_http_lock();
    callback = s_save_callback;
    wifi_http_unlock();

    if (callback == NULL) {
        return ESP_OK;
    }

    if (xTaskCreate(wifi_http_save_callback_task, "wifi_saved", WIFI_HTTP_CALLBACK_STACK,
                    NULL, WIFI_HTTP_CALLBACK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}


static esp_err_t http_landing_handler(httpd_req_t *req)
{
    static const char unavailable_page[] =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Vonix Inverter Setup</title></head><body>"
        "<h2>Vonix Inverter</h2><p>Secure setup is currently unavailable.</p>"
        "<p>Wi-Fi credentials are never accepted over an unencrypted connection. "
        "Restart Wi-Fi setup from the inverter panel and try again.</p></body></html>";
    static const char page[] =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Vonix Inverter Secure Setup</title></head><body>"
        "<h2>Vonix Inverter</h2><p>Secure setup is available over HTTPS.</p>"
        "<p><a href='https://192.168.4.1/'>Open secure Wi-Fi setup</a></p>"
        "<p>Your browser may warn because this device uses a locally generated certificate. "
        "Only proceed if you are connected directly to your own inverter.</p></body></html>";
    return wifi_http_send_text(req, "200 OK", "text/html; charset=utf-8",
                               wifi_https_server_running() ? page : unavailable_page);
}

static esp_err_t http_sensitive_action_blocked(httpd_req_t *req)
{
    return wifi_http_send_text(req, "426 Upgrade Required", "text/plain; charset=utf-8",
                               "Sensitive actions require HTTPS. Open https://192.168.4.1/");
}

static esp_err_t root_handler(httpd_req_t *req)
{
    return wifi_http_send_text(req, "200 OK", "text/html; charset=utf-8",
                               wifi_web_pages_get_setup());
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char *form = NULL;
    const esp_err_t receive_err = wifi_http_receive_form(req, &form);
    if (receive_err == ESP_ERR_INVALID_SIZE) {
        return wifi_http_send_error(req, "413 Payload Too Large", "Invalid form size");
    }
    if (receive_err == ESP_ERR_TIMEOUT) {
        return wifi_http_send_error(req, "408 Request Timeout", "Form receive timed out");
    }
    if (receive_err != ESP_OK) {
        return wifi_http_send_error(req, "400 Bad Request", "Invalid form body");
    }

    if (!wifi_http_verify_pin(form)) {
        free(form);
        return wifi_http_send_error(req, "401 Unauthorized",
                                    "Valid security PIN required");
    }

    wifi_credentials_t credentials = {0};
    /* Percent-encoding can triple the length, so extract into larger scratch
     * buffers, decode, then bounds-check against the real field sizes. A value
     * that does not fit even when encoded is rejected, never silently cut. */
    char ssid_enc[(sizeof(credentials.ssid) - 1U) * 3U + 1U] = {0};
    char pass_enc[(sizeof(credentials.password) - 1U) * 3U + 1U] = {0};
    const esp_err_t ssid_err = httpd_query_key_value(form, "ssid", ssid_enc,
                                                      sizeof(ssid_enc));
    const esp_err_t pass_err = httpd_query_key_value(form, "password", pass_enc,
                                                      sizeof(pass_enc));
    wifi_http_wipe(form, strlen(form));
    free(form);

    const char *reject = NULL;
    if (ssid_err != ESP_OK || !wifi_http_url_decode(ssid_enc) ||
        strlen(ssid_enc) >= sizeof(credentials.ssid)) {
        reject = "Invalid SSID";
    } else if ((pass_err != ESP_OK && pass_err != ESP_ERR_NOT_FOUND) ||
               !wifi_http_url_decode(pass_enc) ||
               strlen(pass_enc) >= sizeof(credentials.password)) {
        reject = "Invalid password";
    } else {
        memcpy(credentials.ssid, ssid_enc, strlen(ssid_enc) + 1U);
        memcpy(credentials.password, pass_enc, strlen(pass_enc) + 1U);
    }
    wifi_http_wipe(pass_enc, sizeof(pass_enc));
    if (reject != NULL) {
        wifi_http_wipe(&credentials, sizeof(credentials));
        return wifi_http_send_error(req, "400 Bad Request", reject);
    }

    if (!wifi_http_valid_text(credentials.ssid, sizeof(credentials.ssid), true)) {
        wifi_http_wipe(&credentials, sizeof(credentials));
        return wifi_http_send_error(req, "400 Bad Request", "Invalid SSID");
    }
    if (!wifi_http_valid_text(credentials.password, sizeof(credentials.password), false) ||
        (credentials.password[0] != '\0' && strlen(credentials.password) < 8U)) {
        wifi_http_wipe(&credentials, sizeof(credentials));
        return wifi_http_send_error(req, "400 Bad Request", "Password must be empty or at least 8 characters");
    }

    const esp_err_t save_err = wifi_storage_save_credentials(&credentials);
    wifi_http_wipe(&credentials, sizeof(credentials));
    if (save_err != ESP_OK) {
        ESP_LOGE(WIFI_HTTP_TAG, "Credential save failed: %s", esp_err_to_name(save_err));
        return wifi_http_send_error(req, "500 Internal Server Error", "Could not save credentials");
    }

    const esp_err_t callback_err = wifi_http_schedule_save_callback();
    if (callback_err != ESP_OK) {
        ESP_LOGE(WIFI_HTTP_TAG, "Credential callback scheduling failed: %s", esp_err_to_name(callback_err));
        return wifi_http_send_error(req, "503 Service Unavailable", "Credentials saved; retry connection from the panel");
    }

    return wifi_http_send_text(req, "200 OK", "text/html; charset=utf-8",
                               wifi_web_pages_get_saved());
}

static const char *wifi_http_state_text(wifi_connection_state_t state)
{
    switch (state) {
    case WIFI_STATE_CONNECTED: return "CONNECTED";
    case WIFI_STATE_CONNECTING:
    case WIFI_STATE_RECONNECTING: return "CONNECTING";
    case WIFI_STATE_PROVISIONING: return "PROVISIONING";
    case WIFI_STATE_FAILED: return "FAILED";
    case WIFI_STATE_DISCONNECTED: return "DISCONNECTED";
    default: return "IDLE";
    }
}

static esp_err_t status_handler(httpd_req_t *req)
{
    const wifi_status_t *status = wifi_manager_get_status();
    char ip_text[16] = "0.0.0.0";
    const char *state_text = "IDLE";
    if (status != NULL) {
        state_text = wifi_http_state_text(status->state);
        snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&status->ip));
    }
    char page[512] = {0};
    const int rendered = wifi_web_pages_render_status(page, sizeof(page), state_text, ip_text);
    if (rendered < 0 || (size_t)rendered >= sizeof(page)) {
        return wifi_http_send_error(req, "500 Internal Server Error", "Status rendering failed");
    }
    return wifi_http_send_text(req, "200 OK", "text/html; charset=utf-8", page);
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    /* A client may not use the provisioning endpoint to continuously trigger
     * expensive radio scans and heap allocations. Reject concurrent scans and
     * enforce a five-second cooldown between completed/started scans. */
    if (s_scan_guard == NULL || xSemaphoreTake(s_scan_guard, 0) != pdTRUE) {
        return wifi_http_send_error(req, "429 Too Many Requests", "Scan already in progress");
    }
    const int64_t now_us = esp_timer_get_time();
    if (s_last_scan_us != 0 && now_us - s_last_scan_us < 5000000LL) {
        xSemaphoreGive(s_scan_guard);
        return wifi_http_send_error(req, "429 Too Many Requests", "Please wait before scanning again");
    }
    s_last_scan_us = now_us;
    char *results = calloc(WIFI_HTTP_SCAN_BYTES, 1U);
    if (results == NULL) {
        xSemaphoreGive(s_scan_guard);
        return wifi_http_send_error(req, "503 Service Unavailable", "Out of memory");
    }

    const esp_err_t scan_err = wifi_scan_start(results, WIFI_HTTP_SCAN_BYTES);
    if (scan_err != ESP_OK) {
        free(results);
        xSemaphoreGive(s_scan_guard);
        ESP_LOGW(WIFI_HTTP_TAG, "Provisioning scan failed: %s", esp_err_to_name(scan_err));
        return wifi_http_send_error(req, "503 Service Unavailable", "Wi-Fi scan unavailable");
    }

    char *page = calloc(WIFI_HTTP_SCAN_BYTES + 768U, 1U);
    if (page == NULL) {
        free(results);
        xSemaphoreGive(s_scan_guard);
        return wifi_http_send_error(req, "503 Service Unavailable", "Out of memory");
    }
    const int rendered = wifi_web_pages_render_scan(page, WIFI_HTTP_SCAN_BYTES + 768U, results);
    free(results);
    if (rendered < 0 || rendered >= (int)(WIFI_HTTP_SCAN_BYTES + 768U)) {
        free(page);
        return wifi_http_send_error(req, "500 Internal Server Error", "Scan rendering failed");
    }
    const esp_err_t response_err = wifi_http_send_text(req, "200 OK", "text/html; charset=utf-8", page);
    free(page);
    xSemaphoreGive(s_scan_guard);
    return response_err;
}

static esp_err_t reset_handler(httpd_req_t *req)
{
    char *form = NULL;
    const esp_err_t receive_err = wifi_http_receive_form(req, &form);
    if (receive_err != ESP_OK) {
        return wifi_http_send_error(req, "400 Bad Request",
                                    "Security PIN form required");
    }
    const bool authorized = wifi_http_verify_pin(form);
    free(form);
    if (!authorized) {
        return wifi_http_send_error(req, "401 Unauthorized",
                                    "Valid security PIN required");
    }

    const esp_err_t erase_err = wifi_storage_erase_credentials();
    if (erase_err != ESP_OK) {
        ESP_LOGE(WIFI_HTTP_TAG, "Credential erase failed: %s", esp_err_to_name(erase_err));
        return wifi_http_send_error(req, "500 Internal Server Error", "Could not erase credentials");
    }
    return wifi_http_send_text(req, "200 OK", "text/html; charset=utf-8",
                               wifi_web_pages_get_reset());
}

static esp_err_t redirect_handler(httpd_req_t *req)
{
    return wifi_http_redirect_home(req);
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    return wifi_http_redirect_home(req);
}

esp_err_t wifi_http_server_start(void)
{
    wifi_http_lock();
    if (s_server != NULL) {
        wifi_http_unlock();
        return ESP_OK;
    }
    wifi_http_unlock();

    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_scan_guard == NULL) {
        s_scan_guard = xSemaphoreCreateMutex();
        if (s_scan_guard == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WIFI_HTTP_SERVER_PORT;
    config.max_uri_handlers = 13U;
    config.max_open_sockets = 3U;
    config.stack_size = WIFI_HTTP_STACK_SIZE;
    config.lru_purge_enable = true;
    /* Needed so the wildcard catch-all route matches; without it the URI is compared literally. */
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_HTTP_TAG, "HTTP start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t *const routes[] = {
        &s_http_root_uri, &s_http_save_uri, &s_http_status_uri, &s_http_scan_uri, &s_http_reset_uri,
        &s_generate_204_uri, &s_gen_204_uri, &s_hotspot_uri, &s_success_html_uri,
        &s_ncsi_uri, &s_connecttest_uri, &s_success_txt_uri, &s_redirect_uri,
    };
    for (size_t i = 0U; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(server, routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(WIFI_HTTP_TAG, "Route registration failed for %s: %s",
                     routes[i]->uri, esp_err_to_name(err));
            (void)httpd_stop(server);
            return err;
        }
    }

    wifi_http_lock();
    s_server = server;
    wifi_http_unlock();
    ESP_LOGI(WIFI_HTTP_TAG, "Provisioning HTTP server started on port %u", WIFI_HTTP_SERVER_PORT);

    /* Keep HTTP for OS captive-portal detection; offer the same handlers on
     * HTTPS for users who explicitly open https://192.168.4.1/. */
    (void)wifi_https_start_optional();
    return ESP_OK;
}

esp_err_t wifi_http_server_stop(void)
{
    wifi_http_lock();
    httpd_handle_t server = s_server;
    httpd_handle_t https_server = s_https_server;
    s_server = NULL;
    s_https_server = NULL;
    wifi_http_unlock();

    esp_err_t first_err = ESP_OK;
    if (https_server != NULL) {
        const esp_err_t https_err = httpd_ssl_stop(https_server);
        if (https_err != ESP_OK) {
            ESP_LOGW(WIFI_HTTP_TAG, "HTTPS stop failed: %s", esp_err_to_name(https_err));
            first_err = https_err;
        } else {
            ESP_LOGI(WIFI_HTTP_TAG, "Provisioning HTTPS server stopped");
        }
    }

    if (server == NULL) {
        return first_err;
    }
    const esp_err_t err = httpd_stop(server);
    if (err != ESP_OK) {
        ESP_LOGW(WIFI_HTTP_TAG, "HTTP stop failed: %s", esp_err_to_name(err));
        if (first_err == ESP_OK) first_err = err;
    } else {
        ESP_LOGI(WIFI_HTTP_TAG, "Provisioning HTTP server stopped");
    }
    return first_err;
}

bool wifi_http_server_running(void)
{
    wifi_http_lock();
    const bool running = s_server != NULL;
    wifi_http_unlock();
    return running;
}


bool wifi_https_server_running(void)
{
    wifi_http_lock();
    const bool running = s_https_server != NULL;
    wifi_http_unlock();
    return running;
}

esp_err_t wifi_http_server_register_save_callback(wifi_http_save_callback_t callback)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    wifi_http_lock();
    s_save_callback = callback;
    wifi_http_unlock();
    return ESP_OK;
}
