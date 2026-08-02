/**
 * @file wifi_http_server.c
 * @brief Wi-Fi Configuration HTTP Server
 */

#include "wifi_http_server.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_server.h"

#include "wifi_storage.h"
#include "wifi_web_pages.h"
#include "wifi_manager.h"
#include "wifi_scan.h"
#include "wifi_config.h"
#include "wifi_events.h"

#include "esp_netif.h"

static const char *TAG = "WIFI_HTTP";

static httpd_handle_t s_server = NULL;
static wifi_http_save_callback_t s_save_callback = NULL;

/*----------------------------------------------------------
 * Forward Declarations
 *---------------------------------------------------------*/
static esp_err_t root_handler(httpd_req_t *req);
static esp_err_t save_handler(httpd_req_t *req);
static esp_err_t status_handler(httpd_req_t *req);
static esp_err_t scan_handler(httpd_req_t *req);
static esp_err_t reset_handler(httpd_req_t *req);
static esp_err_t select_handler(httpd_req_t *req);
static esp_err_t redirect_handler(httpd_req_t *req);
static esp_err_t generate_204_handler(httpd_req_t *req);
static esp_err_t gen_204_handler(httpd_req_t *req);
static esp_err_t hotspot_detect_handler(httpd_req_t *req);
static esp_err_t success_html_handler(httpd_req_t *req);
static esp_err_t ncsi_handler(httpd_req_t *req);
static esp_err_t connecttest_handler(httpd_req_t *req);
static esp_err_t success_txt_handler(httpd_req_t *req);

/*----------------------------------------------------------
 * URI Table
 *---------------------------------------------------------*/
static const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_handler,
};

static const httpd_uri_t save_uri = {
    .uri = "/save",
    .method = HTTP_POST, /* SECURITY: POST for credentials, not GET */
    .handler = save_handler,
};

static const httpd_uri_t status_uri = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = status_handler,
};

static const httpd_uri_t scan_uri = {
    .uri = "/scan",
    .method = HTTP_GET,
    .handler = scan_handler,
};

static const httpd_uri_t reset_uri = {
    .uri = "/reset",
    .method = HTTP_GET,
    .handler = reset_handler,
};

static const httpd_uri_t select_uri = {
    .uri = "/select",
    .method = HTTP_GET,
    .handler = select_handler,
};

static const httpd_uri_t uri_generate_204 = {
    .uri = "/generate_204",
    .method = HTTP_GET,
    .handler = generate_204_handler,
};

static const httpd_uri_t uri_gen_204 = {
    .uri = "/gen_204",
    .method = HTTP_GET,
    .handler = gen_204_handler,
};

static const httpd_uri_t uri_hotspot = {
    .uri = "/hotspot-detect.html",
    .method = HTTP_GET,
    .handler = hotspot_detect_handler,
};

static const httpd_uri_t uri_success_html = {
    .uri = "/library/test/success.html",
    .method = HTTP_GET,
    .handler = success_html_handler,
};

static const httpd_uri_t uri_ncsi = {
    .uri = "/ncsi.txt",
    .method = HTTP_GET,
    .handler = ncsi_handler,
};

static const httpd_uri_t uri_connecttest = {
    .uri = "/connecttest.txt",
    .method = HTTP_GET,
    .handler = connecttest_handler,
};

static const httpd_uri_t uri_success_txt = {
    .uri = "/success.txt",
    .method = HTTP_GET,
    .handler = success_txt_handler,
};

/* Catch-all redirect — registered last with wildcard */
static const httpd_uri_t redirect_uri = {
    .uri = "/*",
    .method = HTTP_GET,
    .handler = redirect_handler,
};

/*----------------------------------------------------------
 * Root Page Handler
 *---------------------------------------------------------*/
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, wifi_web_pages_get_setup(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/*----------------------------------------------------------
 * Save Handler — POST only for security
 *---------------------------------------------------------*/
static esp_err_t save_handler(httpd_req_t *req)
{
    /* Reject GET requests at the door */
    if (req->method != HTTP_POST)
    {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        httpd_resp_sendstr(req, "Use POST for credential submission");
        return ESP_OK;
    }

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "No data received");
        return ESP_OK;
    }
    buf[ret] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};

    /* Parse application/x-www-form-urlencoded body */
    if (httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid)) != ESP_OK)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Missing SSID");
        return ESP_OK;
    }

    /* Password is optional for open networks, but warn if empty */
    httpd_query_key_value(buf, "password", password, sizeof(password));

    /* Validate SSID */
    if (ssid[0] == '\0' || strlen(ssid) > 32)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Invalid SSID");
        return ESP_OK;
    }

    wifi_credentials_t credentials;
    memset(&credentials, 0, sizeof(credentials));

    strncpy(credentials.ssid, ssid, sizeof(credentials.ssid) - 1);
    credentials.ssid[sizeof(credentials.ssid) - 1] = '\0';

    strncpy(credentials.password, password, sizeof(credentials.password) - 1);
    credentials.password[sizeof(credentials.password) - 1] = '\0';

    esp_err_t err = wifi_storage_save_credentials(&credentials);
    if (err != ESP_OK)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Failed to save credentials");
        return ESP_OK;
    }

    /* Save network configuration */
    wifi_network_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.mode = WIFI_MODE_APSTA;
    cfg.auto_reconnect = true;
    cfg.reconnect_interval_ms = 5000;
    cfg.dhcp = true;

    /* TODO: Load from build config or device-specific storage */
    strncpy(cfg.ap_ssid, WIFI_PROVISION_AP_SSID, sizeof(cfg.ap_ssid) - 1);
    cfg.ap_ssid[sizeof(cfg.ap_ssid) - 1] = '\0';

    strncpy(cfg.ap_password, WIFI_PROVISION_AP_PASSWORD, sizeof(cfg.ap_password) - 1);
    cfg.ap_password[sizeof(cfg.ap_password) - 1] = '\0';

    cfg.ap_channel = WIFI_PROVISION_CHANNEL;

    err = wifi_storage_save_network_config(&cfg);
    if (err != ESP_OK)
    {
        /* Rollback: erase credentials to keep state consistent */
        wifi_storage_erase_credentials();
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Failed to save network configuration");
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "Saved. Connecting...");

    if (s_save_callback)
    {
        s_save_callback();
    }

    return ESP_OK;
}

/*----------------------------------------------------------
 * Status Handler
 *---------------------------------------------------------*/
static esp_err_t status_handler(httpd_req_t *req)
{
    const wifi_status_t *status = wifi_manager_get_status();

    char status_text[32] = "UNKNOWN";
    char ip_text[32] = "0.0.0.0";

    if (status != NULL)
    {
        switch (status->state)
        {
        case WIFI_STATE_CONNECTED:
            strncpy(status_text, "CONNECTED", sizeof(status_text) - 1);
            break;
        case WIFI_STATE_CONNECTING:
            strncpy(status_text, "CONNECTING", sizeof(status_text) - 1);
            break;
        case WIFI_STATE_DISCONNECTED:
            strncpy(status_text, "DISCONNECTED", sizeof(status_text) - 1);
            break;
        case WIFI_STATE_FAILED:
            strncpy(status_text, "FAILED", sizeof(status_text) - 1);
            break;
        default:
            strncpy(status_text, "IDLE", sizeof(status_text) - 1);
            break;
        }
        status_text[sizeof(status_text) - 1] = '\0';

        snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&status->ip));
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
                    wifi_web_pages_get_status(status_text, ip_text),
                    HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Scan Handler — Per-request buffer, not static
 *---------------------------------------------------------*/
static esp_err_t scan_handler(httpd_req_t *req)
{
    /* Allocate per-request buffer to avoid thread-safety issues */
    const size_t results_size = 4096;
    char *results = malloc(results_size);
    if (results == NULL)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Out of memory");
        return ESP_OK;
    }

    memset(results, 0, results_size);

    esp_err_t err = wifi_scan_start(results, results_size);
    if (err != ESP_OK)
    {
        strncpy(results, "Scan failed", results_size - 1);
        results[results_size - 1] = '\0';
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, wifi_web_pages_get_scan(results), HTTPD_RESP_USE_STRLEN);

    free(results);
    return ESP_OK;
}

/*----------------------------------------------------------
 * Reset Handler
 *---------------------------------------------------------*/
static esp_err_t reset_handler(httpd_req_t *req)
{
    esp_err_t err = wifi_storage_erase_credentials();
    if (err == ESP_OK)
    {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, wifi_web_pages_get_reset(), HTTPD_RESP_USE_STRLEN);
        ESP_LOGW(TAG, "WiFi credentials erased");
    }
    else
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Reset failed");
    }
    return ESP_OK;
}

/*----------------------------------------------------------
 * Select Handler — URL-encode SSID for JavaScript safety
 *---------------------------------------------------------*/
static esp_err_t select_handler(httpd_req_t *req)
{
    char query[128];
    char ssid[33] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Missing parameters");
        return ESP_OK;
    }

    if (httpd_query_key_value(query, "ssid", ssid, sizeof(ssid)) != ESP_OK)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Missing SSID");
        return ESP_OK;
    }

    /* Basic XSS prevention: reject SSIDs with dangerous chars */
    for (size_t i = 0; ssid[i] != '\0'; i++)
    {
        if (ssid[i] == '\'' || ssid[i] == '"' || ssid[i] == '<' ||
            ssid[i] == '>' || ssid[i] == '&' || ssid[i] == '\\')
        {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "Invalid characters in SSID");
            return ESP_OK;
        }
    }

    char redirect[256];
    int len = snprintf(redirect, sizeof(redirect),
                       "<script>"
                       "localStorage.ssid='%s';"
                       "location.href='/';"
                       "</script>",
                       ssid);

    if (len < 0 || (size_t)len >= sizeof(redirect))
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Response too large");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, redirect, HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Captive Portal Handlers
 *---------------------------------------------------------*/
static esp_err_t generate_204_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t gen_204_handler(httpd_req_t *req)
{
    return generate_204_handler(req);
}

static esp_err_t hotspot_detect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t success_html_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t ncsi_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t connecttest_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t success_txt_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/*----------------------------------------------------------
 * Catch-all Redirect
 *---------------------------------------------------------*/
static esp_err_t redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/*----------------------------------------------------------
 * Start Server
 *---------------------------------------------------------*/
esp_err_t wifi_http_server_start(void)
{
    if (s_server != NULL)
    {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WIFI_HTTP_PORT;
    config.max_uri_handlers = WIFI_HTTP_MAX_URI + 1; /* +1 for catch-all */

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Register all URI handlers */
    const httpd_uri_t *uris[] = {
        &root_uri, &save_uri, &status_uri, &scan_uri,
        &reset_uri, &select_uri, &uri_generate_204, &uri_gen_204,
        &uri_hotspot, &uri_success_html, &uri_ncsi,
        &uri_connecttest, &uri_success_txt, &redirect_uri,
        NULL};

    for (const httpd_uri_t **uri = uris; *uri != NULL; uri++)
    {
        err = httpd_register_uri_handler(s_server, *uri);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to register URI %s: %s",
                     (*uri)->uri, esp_err_to_name(err));
            /* Continue — non-critical if one handler fails */
        }
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Stop Server
 *---------------------------------------------------------*/
esp_err_t wifi_http_server_stop(void)
{
    if (s_server == NULL)
    {
        return ESP_OK;
    }

    esp_err_t err = httpd_stop(s_server);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "HTTP server stop failed: %s", esp_err_to_name(err));
        return err;
    }

    s_server = NULL;

    ESP_LOGI(TAG, "HTTP server stopped");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Server State
 *---------------------------------------------------------*/
bool wifi_http_server_running(void)
{
    return (s_server != NULL);
}

/*----------------------------------------------------------
 * Callback Registration
 *---------------------------------------------------------*/
esp_err_t wifi_http_server_register_save_callback(
    wifi_http_save_callback_t callback)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_save_callback = callback;

    return ESP_OK;
}