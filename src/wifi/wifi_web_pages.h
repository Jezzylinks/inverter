#ifndef WIFI_WEB_PAGES_H
#define WIFI_WEB_PAGES_H

#ifdef __cplusplus
extern "C"
{
#endif

    const char *wifi_web_pages_get_setup(void);

    const char *wifi_web_pages_get_status(
        const char *status_text,
        const char *ip);

    const char *wifi_web_pages_get_scan(
        const char *scan_results);

    const char *wifi_web_pages_get_saved(void);

    const char *wifi_web_pages_get_reset(void);

#ifdef __cplusplus
}
#endif

#endif