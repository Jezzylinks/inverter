#ifndef LCD_EVENT_RECEIVER_H
#define LCD_EVENT_RECEIVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the task that drains EVENT_SUB_LCD and forwards display-safe notices. */
esp_err_t lcd_event_receiver_start(void);

/** Stop the LCD event receiver cooperatively. */
esp_err_t lcd_event_receiver_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* LCD_EVENT_RECEIVER_H */
