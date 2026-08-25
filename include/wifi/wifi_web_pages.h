#ifndef WIFI_WEB_PAGES_H
#define WIFI_WEB_PAGES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/** Immutable setup page containing only local provisioning actions. */
const char *wifi_web_pages_get_setup(void);

/** Render dynamic HTML into the caller-provided buffer. */
int wifi_web_pages_render_status(char *buffer, size_t buffer_len,
                                 const char *status_text, const char *ip);
int wifi_web_pages_render_scan(char *buffer, size_t buffer_len,
                               const char *scan_results);

const char *wifi_web_pages_get_saved(void);
const char *wifi_web_pages_get_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_WEB_PAGES_H */
