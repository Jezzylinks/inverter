/**
 * @file wifi_http_server.c
 * @brief Hardened local HTTP server for Wi-Fi provisioning.
 */
#include "wifi_http_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "wifi_manager.h"
#include "wifi_scan.h"
#include "wifi_storage.h"
#include "wifi_web_pages.h"

#define WIFI_HTTP_TAG "WIFI_HTTP"
#define WIFI_HTTP_SCAN_BYTES 4096U
#define WIFI_HTTP_CALLBACK_STACK 3072U
#define WIFI_HTTP_CALLBACK_PRIORITY 4U

static httpd_handle_t s_server;
static SemaphoreHandle_t s_mutex;
static wifi_http_save_callback_t s_save_callback;

static esp_err_t root_handler(httpd_req_t *req);
static esp_err_t save_handler(httpd_req_t *req);
static esp_err_t status_handler(httpd_req_t *req);
static esp_err_t scan_handler(httpd_req_t *req);
static esp_err_t reset_handler(httpd_req_t *req);
static esp_err_t redirect_handler(httpd_req_t *req);
static esp_err_t captive_redirect_handler(httpd_req_t *req);

static const httpd_uri_t s_root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_handler};
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

    wifi_credentials_t credentials = {0};
    const esp_err_t ssid_err = httpd_query_key_value(form, "ssid", credentials.ssid,
                                                      sizeof(credentials.ssid));
    (void)httpd_query_key_value(form, "password", credentials.password,
                                sizeof(credentials.password));
    free(form);

    if (ssid_err != ESP_OK || !wifi_http_valid_text(credentials.ssid,
                                                     sizeof(credentials.ssid), true)) {
        return wifi_http_send_error(req, "400 Bad Request", "Invalid SSID");
    }
    if (!wifi_http_valid_text(credentials.password, sizeof(credentials.password), false) ||
        (credentials.password[0] != '\0' && strlen(credentials.password) < 8U)) {
        return wifi_http_send_error(req, "400 Bad Request", "Password must be empty or at least 8 characters");
    }

    const esp_err_t save_err = wifi_storage_save_credentials(&credentials);
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
    char *results = calloc(WIFI_HTTP_SCAN_BYTES, 1U);
    if (results == NULL) {
        return wifi_http_send_error(req, "503 Service Unavailable", "Out of memory");
    }

    const esp_err_t scan_err = wifi_scan_start(results, WIFI_HTTP_SCAN_BYTES);
    if (scan_err != ESP_OK) {
        free(results);
        ESP_LOGW(WIFI_HTTP_TAG, "Provisioning scan failed: %s", esp_err_to_name(scan_err));
        return wifi_http_send_error(req, "503 Service Unavailable", "Wi-Fi scan unavailable");
    }

    char *page = calloc(WIFI_HTTP_SCAN_BYTES + 768U, 1U);
    if (page == NULL) {
        free(results);
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
    return response_err;
}

static esp_err_t reset_handler(httpd_req_t *req)
{
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

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WIFI_HTTP_SERVER_PORT;
    config.max_uri_handlers = 13U;
    config.stack_size = WIFI_HTTP_STACK_SIZE;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_HTTP_TAG, "HTTP start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t *const routes[] = {
        &s_root_uri, &s_save_uri, &s_status_uri, &s_scan_uri, &s_reset_uri,
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
    return ESP_OK;
}

esp_err_t wifi_http_server_stop(void)
{
    wifi_http_lock();
    httpd_handle_t server = s_server;
    s_server = NULL;
    wifi_http_unlock();

    if (server == NULL) {
        return ESP_OK;
    }

    const esp_err_t err = httpd_stop(server);
    if (err != ESP_OK) {
        ESP_LOGW(WIFI_HTTP_TAG, "HTTP stop failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(WIFI_HTTP_TAG, "Provisioning HTTP server stopped");
    }
    return err;
}

bool wifi_http_server_running(void)
{
    wifi_http_lock();
    const bool running = s_server != NULL;
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
