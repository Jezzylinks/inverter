#ifndef APP_BUTTONS_H
#define APP_BUTTONS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize all physical buttons on the single shared button-controller task. */
esp_err_t app_buttons_init(void);

/** Stop and destroy all button registrations, then tear down the shared controller. */
void app_buttons_deinit(void);

/** Reset captured per-button statistics without changing bindings. */
void app_buttons_reset_statistics(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BUTTONS_H */
