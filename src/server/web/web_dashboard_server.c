#include "server/web/web_dashboard_server.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_spiffs.h"

#define WEB_DASHBOARD_TAG "WEB_DASHBOARD"
#define WEB_DASHBOARD_BASE_PATH "/www"
#define WEB_DASHBOARD_PARTITION "spiffs"

static bool s_mounted;
static bool s_registered;

static void dashboard_security_headers(httpd_req_t *req, bool asset)
{
    (void)httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    (void)httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    (void)httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    (void)httpd_resp_set_hdr(req, "Cache-Control", asset ? "public, max-age=3600" : "no-store, max-age=0");
    (void)httpd_resp_set_hdr(req, "Content-Security-Policy",
                             "default-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'; connect-src 'self' ws: wss:");
}

static esp_err_t send_file(httpd_req_t *req, const char *path, const char *content_type, bool asset)
{
    struct stat info;
    if (stat(path, &info) != 0 || info.st_size < 0) {
        httpd_resp_set_status(req, "404 Not Found");
        dashboard_security_headers(req, false);
        return httpd_resp_sendstr(req, "Not found");
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        dashboard_security_headers(req, false);
        return httpd_resp_sendstr(req, "Dashboard file unavailable");
    }

    dashboard_security_headers(req, asset);
    (void)httpd_resp_set_type(req, content_type);
    char buffer[1024];
    size_t read_count;
    esp_err_t err = ESP_OK;
    while ((read_count = fread(buffer, 1U, sizeof(buffer), file)) > 0U) {
        err = httpd_resp_send_chunk(req, buffer, read_count);
        if (err != ESP_OK) {
            break;
        }
    }
    fclose(file);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    return send_file(req, WEB_DASHBOARD_BASE_PATH "/index.html", "text/html; charset=utf-8", false);
}

static esp_err_t css_handler(httpd_req_t *req)
{
    return send_file(req, WEB_DASHBOARD_BASE_PATH "/assets/app.css", "text/css; charset=utf-8", true);
}

static esp_err_t js_handler(httpd_req_t *req)
{
    return send_file(req, WEB_DASHBOARD_BASE_PATH "/assets/app.js", "application/javascript; charset=utf-8", true);
}

static const httpd_uri_t s_index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
};

static const httpd_uri_t s_css_uri = {
    .uri = "/assets/app.css",
    .method = HTTP_GET,
    .handler = css_handler,
};

static const httpd_uri_t s_js_uri = {
    .uri = "/assets/app.js",
    .method = HTTP_GET,
    .handler = js_handler,
};

esp_err_t web_dashboard_server_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    const esp_vfs_spiffs_conf_t config = {
        .base_path = WEB_DASHBOARD_BASE_PATH,
        .partition_label = WEB_DASHBOARD_PARTITION,
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&config);
    if (err != ESP_OK) {
        ESP_LOGW(WEB_DASHBOARD_TAG, "Dashboard filesystem unavailable: %s", esp_err_to_name(err));
        return err;
    }

    struct stat index_info;
    if (stat(WEB_DASHBOARD_BASE_PATH "/index.html", &index_info) != 0) {
        (void)esp_vfs_spiffs_unregister(WEB_DASHBOARD_PARTITION);
        ESP_LOGW(WEB_DASHBOARD_TAG, "Dashboard assets are not present; upload the SPIFFS data image");
        return ESP_ERR_NOT_FOUND;
    }
    s_mounted = true;
    ESP_LOGI(WEB_DASHBOARD_TAG, "Dashboard filesystem mounted");
    return ESP_OK;
}

esp_err_t web_dashboard_server_deinit(void)
{
    if (s_registered) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mounted) {
        (void)esp_vfs_spiffs_unregister(WEB_DASHBOARD_PARTITION);
        s_mounted = false;
    }
    return ESP_OK;
}

esp_err_t web_dashboard_server_register(httpd_handle_t server)
{
    if (server == NULL || !s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    const httpd_uri_t *routes[] = {&s_index_uri, &s_css_uri, &s_js_uri};
    for (size_t i = 0U; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        const esp_err_t err = httpd_register_uri_handler(server, routes[i]);
        if (err != ESP_OK) {
            for (size_t j = 0U; j < i; ++j) {
                (void)httpd_unregister_uri_handler(server, routes[j]->uri, routes[j]->method);
            }
            return err;
        }
    }
    s_registered = true;
    return ESP_OK;
}

esp_err_t web_dashboard_server_unregister(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_OK;
    }
    (void)httpd_unregister_uri_handler(server, "/", HTTP_GET);
    (void)httpd_unregister_uri_handler(server, "/assets/app.css", HTTP_GET);
    (void)httpd_unregister_uri_handler(server, "/assets/app.js", HTTP_GET);
    s_registered = false;
    return ESP_OK;
}

bool web_dashboard_server_available(void)
{
    return s_mounted && s_registered;
}
