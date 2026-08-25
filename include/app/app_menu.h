#ifndef APP_MENU_H
#define APP_MENU_H

#include <stddef.h>

#include "system/system_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *label;
    menu_state_t state;
} menu_item_t;

const menu_item_t *get_menu_items(menu_state_t state, int *item_count);
void show_menu_screen(menu_state_t menu_st, int selection);
void go_to_main_screen(void);
void push_menu_history(menu_state_t state, uint8_t selection);
bool pop_menu_history(menu_state_t *state, int *selection);
void clear_menu_history(void);
size_t app_settings_count(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_MENU_H */
