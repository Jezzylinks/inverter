#ifndef APP_INPUT_H
#define APP_INPUT_H

#include "app/button_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

void handle_power_button_event(button_event_info_t *event_info, void *user_data);
void handle_enter_menu_button_event(button_event_info_t *event_info, void *user_data);
void handle_up_button_event(button_event_info_t *event_info, void *user_data);
void handle_down_button_event(button_event_info_t *event_info, void *user_data);
void handle_back_button_event(button_event_info_t *event_info, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* APP_INPUT_H */
