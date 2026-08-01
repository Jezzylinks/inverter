#include "wifi_web_pages.h"

#include <stdio.h>

static char page_buffer[2048];

/*==========================================================
 *
 *              SETUP PAGE
 *
 *=========================================================*/

const char *wifi_web_pages_get_setup(void)
{

    return

        "<!DOCTYPE html>"
        "<html>"

        "<head>"

        "<title>Inverter WiFi Setup</title>"

        "<meta name='viewport' "
        "content='width=device-width,initial-scale=1'>"

        "<script>"

        "function selectSSID(ssid)"
        "{"
        "document.getElementById('ssid').value=ssid;"
        "}"

        "</script>"

        "</head>"

        "<body>"

        "<h2>Solar Inverter WiFi Setup</h2>"

        "<form action='/save' method='get'>"

        "SSID:<br>"

        "<input id='ssid' "
        "name='ssid' "
        "type='text'>"

        "<br><br>"

        "Password:<br>"

        "<input name='password' "
        "type='password'>"

        "<br><br>"

        "<button type='submit'>"
        "Connect"
        "</button>"

        "</form>"

        "<br>"

        "<a href='/scan'>"
        "Scan Networks"
        "</a>"

        "<br><br>"

        "<a href='/status'>"
        "WiFi Status"
        "</a>"

        "<br>"

        "<a href='/reset'>"
        "Reset WiFi"
        "</a>"

        "</body>"

        "</html>";
}

/*==========================================================
 *
 *              STATUS PAGE
 *
 *=========================================================*/

const char *wifi_web_pages_get_status(
    const char *status_text,
    const char *ip)
{

    snprintf(page_buffer,
             sizeof(page_buffer),

             "<!DOCTYPE html>"
             "<html>"
             "<head>"
             "<title>Status</title>"
             "</head>"

             "<body>"

             "<h2>WiFi Status</h2>"

             "<p>Status: %s</p>"
             "<p>IP: %s</p>"

             "<br>"
             "<a href='/'>Back</a>"

             "</body>"
             "</html>",

             status_text,
             ip);

    return page_buffer;
}

/*==========================================================
 *
 *              SCAN PAGE
 *
 *=========================================================*/

const char *wifi_web_pages_get_scan(
    const char *scan_results)
{

    snprintf(page_buffer,
             sizeof(page_buffer),

             "<!DOCTYPE html>"

             "<html>"

             "<head>"

             "<title>WiFi Networks</title>"

             "<meta name='viewport' "
             "content='width=device-width,initial-scale=1'>"

             "<script>"

             "function choose(ssid)"
             "{"
             "window.location='/select?ssid='+ssid;"
             "}"

             "</script>"

             "</head>"

             "<body>"

             "<h2>Select Network</h2>"

             "%s"

             "<br>"

             "<a href='/'>Back</a>"

             "</body>"

             "</html>",

             scan_results);

    return page_buffer;
}

/*==========================================================
 *
 *              SAVED PAGE
 *
 *=========================================================*/

const char *wifi_web_pages_get_saved(void)
{

    return

        "<!DOCTYPE html>"
        "<html>"
        "<body>"

        "<h2>Credentials Saved</h2>"

        "<p>Restarting WiFi...</p>"

        "</body>"
        "</html>";
}

/*==========================================================
 *
 *              RESET PAGE
 *
 *=========================================================*/

const char *wifi_web_pages_get_reset(void)
{

    return

        "<!DOCTYPE html>"
        "<html>"
        "<body>"

        "<h2>WiFi Reset</h2>"

        "<p>Credentials erased.</p>"

        "</body>"
        "</html>";
}