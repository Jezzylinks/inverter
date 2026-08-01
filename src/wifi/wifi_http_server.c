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
#include "wifi_storage.h"

#include "esp_netif.h"

static const char *TAG =
    "WIFI_HTTP";

static httpd_handle_t
    s_server = NULL;

static wifi_http_save_callback_t s_save_callback = NULL;

static const httpd_uri_t status_uri =
    {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
};

static const httpd_uri_t scan_uri =
    {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = scan_handler,
};

static const httpd_uri_t reset_uri =
    {
        .uri = "/reset",
        .method = HTTP_GET,
        .handler = reset_handler,
};

static const httpd_uri_t select_uri =
    {
        .uri = "/select",
        .method = HTTP_GET,
        .handler = select_handler};

static const httpd_uri_t uri_generate_204 =
    {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = generate_204_handler};

static const httpd_uri_t uri_gen_204 =
    {
        .uri = "/gen_204",
        .method = HTTP_GET,
        .handler = gen_204_handler};

static const httpd_uri_t uri_hotspot =
    {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = hotspot_detect_handler};

static const httpd_uri_t uri_success_html =
    {
        .uri = "/library/test/success.html",
        .method = HTTP_GET,
        .handler = success_html_handler};

static const httpd_uri_t uri_ncsi =
    {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = ncsi_handler};

static const httpd_uri_t uri_connecttest =
    {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = connecttest_handler};

static const httpd_uri_t uri_success_txt =
    {
        .uri = "/success.txt",
        .method = HTTP_GET,
        .handler = success_txt_handler};
/*==========================================================
 *
 *              ROOT PAGE
 *
 *=========================================================*/

static esp_err_t root_handler(
    httpd_req_t *req)
{

    httpd_resp_send(
        req,
        wifi_web_pages_get_setup(),
        HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

/*==========================================================
 *
 *              SAVE HANDLER
 *
 *=========================================================*/

static esp_err_t save_handler(
    httpd_req_t *req)
{
    char query[128];

    if (httpd_req_get_url_query_str(
            req,
            query,
            sizeof(query)) != ESP_OK)
    {
        httpd_resp_sendstr(
            req,
            "Missing parameters");

        return ESP_OK;
    }

    char ssid[33] = {0};

    char password[65] = {0};

    httpd_query_key_value(
        query,
        "ssid",
        ssid,
        sizeof(ssid));

    httpd_query_key_value(
        query,
        "password",
        password,
        sizeof(password));

    wifi_credentials_t credentials;

    memset(&credentials,
           0,
           sizeof(credentials));

    strncpy(credentials.ssid,
            ssid,
            sizeof(credentials.ssid) - 1);

    strncpy(credentials.password,
            password,
            sizeof(credentials.password) - 1);

    esp_err_t err =
        wifi_storage_save_credentials(&credentials);

    if (err == ESP_OK)
    {
        /*
         * Save network configuration
         */
        wifi_network_config_t cfg;

        memset(&cfg, 0, sizeof(cfg));

        cfg.mode = WIFI_MODE_APSTA;

        cfg.auto_reconnect = true;

        cfg.reconnect_interval_ms = 5000;

        /*
         * Use DHCP by default.
         * If you later add a checkbox on the web page
         * for "Use Static IP", update this value from
         * the user's selection.
         */
        cfg.dhcp = true;

        /*
         * AP configuration
         */
        strncpy(cfg.ap_ssid,
                "INV_SETUP",
                sizeof(cfg.ap_ssid) - 1);

        strncpy(cfg.ap_password,
                "12345678",
                sizeof(cfg.ap_password) - 1);

        cfg.ap_channel = 6;

        err = wifi_storage_save_network_config(&cfg);

        if (err != ESP_OK)
        {
            httpd_resp_sendstr(req,
                               "Failed to save network configuration");
            return ESP_OK;
        }

        httpd_resp_sendstr(
            req,
            "Saved. Connecting...");

        if (s_save_callback)
        {
            s_save_callback();
        }
    }
    else
    {
        httpd_resp_sendstr(
            req,
            "Save failed");
    }

    return ESP_OK;
}

/*==========================================================
 *
 *              URI TABLE
 *
 *=========================================================*/

static const httpd_uri_t root_uri =
    {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
};

static const httpd_uri_t save_uri =
    {
        .uri = "/save",
        .method = HTTP_GET,
        .handler = save_handler,
};

/*----------------------------------------------------------
 * Android
 * http://connectivitycheck.gstatic.com/generate_204
 *---------------------------------------------------------*/

static esp_err_t generate_204_handler(
    httpd_req_t *req)
{
    httpd_resp_set_status(req,
                          "302 Found");

    httpd_resp_set_hdr(req,
                       "Location",
                       "/");

    httpd_resp_send(req,
                    NULL,
                    0);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Apple Captive Portal
 *---------------------------------------------------------*/

static esp_err_t hotspot_detect_handler(
    httpd_req_t *req)
{
    httpd_resp_set_status(req,
                          "302 Found");

    httpd_resp_set_hdr(req,
                       "Location",
                       "/");

    httpd_resp_send(req,
                    NULL,
                    0);

    return ESP_OK;
}

/*----------------------------------------------------------
 * macOS
 *---------------------------------------------------------*/

static esp_err_t success_html_handler(
    httpd_req_t *req)
{
    httpd_resp_set_status(req,
                          "302 Found");

    httpd_resp_set_hdr(req,
                       "Location",
                       "/");

    httpd_resp_send(req,
                    NULL,
                    0);

    return ESP_OK;
}
/*----------------------------------------------------------
 * Android (Older Versions)
 *---------------------------------------------------------*/

static esp_err_t gen_204_handler(
    httpd_req_t *req)
{
    return generate_204_handler(req);
}

/*----------------------------------------------------------
 * Windows NCSI
 *---------------------------------------------------------*/

static esp_err_t ncsi_handler(
    httpd_req_t *req)
{
    httpd_resp_set_status(req,
                          "302 Found");

    httpd_resp_set_hdr(req,
                       "Location",
                       "/");

    httpd_resp_send(req,
                    NULL,
                    0);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Windows Connect Test
 *---------------------------------------------------------*/

static esp_err_t connecttest_handler(
    httpd_req_t *req)
{
    httpd_resp_set_status(req,
                          "302 Found");

    httpd_resp_set_hdr(req,
                       "Location",
                       "/");

    httpd_resp_send(req,
                    NULL,
                    0);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Firefox
 *---------------------------------------------------------*/

static esp_err_t success_txt_handler(
    httpd_req_t *req)
{
    httpd_resp_set_status(req,
                          "302 Found");

    httpd_resp_set_hdr(req,
                       "Location",
                       "/");

    httpd_resp_send(req,
                    NULL,
                    0);

    return ESP_OK;
}

/*==========================================================
 *
 *              START SERVER
 *
 *=========================================================*/

esp_err_t wifi_http_server_start(void)
{
    if (s_server)
    {
        return ESP_OK;
    }

    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();

    config.server_port =
        WIFI_HTTP_SERVER_PORT;

    esp_err_t err =
        httpd_start(
            &s_server,
            &config);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "HTTP server start failed");

        return err;
    }

    httpd_register_uri_handler(
        s_server,
        &root_uri);

    httpd_register_uri_handler(
        s_server,
        &save_uri);

    httpd_register_uri_handler(
        s_server,
        &status_uri);

    httpd_register_uri_handler(
        s_server,
        &scan_uri);

    httpd_register_uri_handler(
        s_server,
        &reset_uri);

    httpd_register_uri_handler(
        s_server,
        &select_uri);

    httpd_register_uri_handler(s_server,
                               &uri_generate_204);

    httpd_register_uri_handler(s_server,
                               &uri_gen_204);

    httpd_register_uri_handler(s_server,
                               &uri_hotspot);

    httpd_register_uri_handler(s_server,
                               &uri_success_html);

    httpd_register_uri_handler(s_server,
                               &uri_ncsi);

    httpd_register_uri_handler(s_server,
                               &uri_connecttest);

    httpd_register_uri_handler(s_server,
                               &uri_success_txt);

    ESP_LOGI(TAG,
             "HTTP server started");

    return ESP_OK;
}

/*==========================================================
 *
 *              STOP SERVER
 *
 *=========================================================*/

esp_err_t wifi_http_server_stop(void)
{
    if (s_server == NULL)
    {
        return ESP_OK;
    }

    httpd_stop(s_server);

    s_server = NULL;

    ESP_LOGI(TAG,
             "HTTP server stopped");

    return ESP_OK;
}

bool wifi_http_server_running(void)
{
    return (s_server != NULL);
}

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

/*==========================================================
 *
 *              STATUS HANDLER
 *
 *=========================================================*/

static esp_err_t status_handler(httpd_req_t *req)
{
    const wifi_status_t *status =
        wifi_manager_get_status();

    char status_text[32];

    char ip_text[16];

    if (status == NULL)
    {
        strcpy(status_text,
               "UNKNOWN");

        strcpy(ip_text,
               "0.0.0.0");
    }
    else
    {
        switch (status->state)
        {
        case WIFI_STATE_CONNECTED:

            strcpy(status_text,
                   "CONNECTED");

            break;

        case WIFI_STATE_CONNECTING:

            strcpy(status_text,
                   "CONNECTING");

            break;

        case WIFI_STATE_DISCONNECTED:

            strcpy(status_text,
                   "DISCONNECTED");

            break;

        case WIFI_STATE_FAILED:

            strcpy(status_text,
                   "FAILED");

            break;

        default:

            strcpy(status_text,
                   "IDLE");

            break;
        }

        snprintf(ip_text,
                 sizeof(ip_text),
                 IPSTR,
                 IP2STR(&status->ip));
    }

    httpd_resp_send(
        req,
        wifi_web_pages_get_status(
            status_text,
            ip_text),
        HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

/*----------------------------------------------------------
 * Catch-all Redirect
 *---------------------------------------------------------*/

static esp_err_t redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req,
                          "302 Found");

    httpd_resp_set_hdr(req,
                       "Location",
                       "/");

    httpd_resp_send(req,
                    NULL,
                    0);

    return ESP_OK;
}

/*==========================================================
 *
 *              SCAN HANDLER
 *
 *=========================================================*/

static esp_err_t scan_handler(httpd_req_t *req)
{
    static char results[1024];

    memset(results,
           0,
           sizeof(results));

    esp_err_t err =
        wifi_scan_start(
            results,
            sizeof(results));

    if (err != ESP_OK)
    {
        strcpy(results,
               "Scan failed");
    }

    httpd_resp_send(
        req,
        wifi_web_pages_get_scan(results),
        HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

/*==========================================================
 *
 *              RESET HANDLER
 *
 *=========================================================*/

static esp_err_t reset_handler(httpd_req_t *req)
{

    esp_err_t err =
        wifi_storage_erase_credentials();

    if (err == ESP_OK)
    {
        httpd_resp_send(
            req,
            wifi_web_pages_get_reset(),
            HTTPD_RESP_USE_STRLEN);

        ESP_LOGW(TAG,
                 "WiFi credentials erased");
    }
    else
    {
        httpd_resp_sendstr(
            req,
            "Reset failed");
    }

    return ESP_OK;
}

static esp_err_t select_handler(httpd_req_t *req)
{
    char query[100];

    char ssid[33];

    if (httpd_req_get_url_query_str(
            req,
            query,
            sizeof(query)) != ESP_OK)
    {
        return ESP_FAIL;
    }

    if (httpd_query_key_value(
            query,
            "ssid",
            ssid,
            sizeof(ssid)) != ESP_OK)
    {
        return ESP_FAIL;
    }

    char redirect[100];

    snprintf(redirect,
             sizeof(redirect),
             "<script>"
             "localStorage.ssid='%s';"
             "location.href='/';"
             "</script>",
             ssid);

    httpd_resp_send(
        req,
        redirect,
        HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}