#include "wifi/wifi_web_pages.h"

#include <stdio.h>

const char *wifi_web_pages_get_setup(void)
{
    return "<!doctype html><html><head>"
           "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>Inverter Wi-Fi Setup</title>"
           "</head><body><h2>Inverter Wi-Fi Setup</h2>"
           "<form action='/save' method='post' autocomplete='off'>"
           "<label>Network name<br><input name='ssid' maxlength='32' required></label><br><br>"
           "<label>Password<br><input name='password' type='password' minlength='8' maxlength='63'></label><br><br>"
           "<label>Security PIN<br><input name='pin' type='password' inputmode='numeric' pattern='[0-9]{4}' minlength='4' maxlength='4' required></label><br><br>"
           "<button type='submit'>Save and connect</button></form>"
           "<p><a href='/scan'>Scan nearby networks</a> | <a href='/status'>Status</a></p>"
           "<form action='/reset' method='post'>"
           "<label>Security PIN<br><input name='pin' type='password' inputmode='numeric' pattern='[0-9]{4}' minlength='4' maxlength='4' required></label><br>"
           "<button type='submit'>Erase saved credentials</button></form>"
           "</body></html>";
}

int wifi_web_pages_render_status(char *buffer, size_t buffer_len,
                                 const char *status_text, const char *ip)
{
    if (buffer == NULL || buffer_len == 0U || status_text == NULL || ip == NULL) {
        return -1;
    }
    return snprintf(buffer, buffer_len,
                    "<!doctype html><html><head><meta charset='utf-8'>"
                    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<title>Wi-Fi Status</title></head><body><h2>Wi-Fi Status</h2>"
                    "<p>Status: %s</p><p>IP: %s</p><p><a href='/'>Back</a></p>"
                    "</body></html>", status_text, ip);
}

int wifi_web_pages_render_scan(char *buffer, size_t buffer_len,
                               const char *scan_results)
{
    if (buffer == NULL || buffer_len == 0U || scan_results == NULL) {
        return -1;
    }
    return snprintf(buffer, buffer_len,
                    "<!doctype html><html><head><meta charset='utf-8'>"
                    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<title>Nearby Wi-Fi Networks</title></head><body>"
                    "<h2>Nearby Wi-Fi Networks</h2>%s"
                    "<p>Enter the network name on the setup page to connect.</p>"
                    "<p><a href='/'>Back</a></p></body></html>", scan_results);
}

const char *wifi_web_pages_get_saved(void)
{
    return "<!doctype html><html><head><meta charset='utf-8'><title>Saved</title></head>"
           "<body><h2>Credentials saved</h2>"
           "<p>The inverter is connecting to the selected network.</p>"
           "<p>You may close this page.</p></body></html>";
}

const char *wifi_web_pages_get_reset(void)
{
    return "<!doctype html><html><head><meta charset='utf-8'><title>Reset complete</title></head>"
           "<body><h2>Credentials erased</h2>"
           "<p>The provisioning portal remains available.</p><p><a href='/'>Back</a></p>"
           "</body></html>";
}
