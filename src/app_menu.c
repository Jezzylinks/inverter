#include "app_menu.h"

#include <stdatomic.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lcd_writer.h"
#include "lcd.h"
#include "security/security.h"
#include "app_services.h"

#define APP_MENU_TAG "APP_MENU"
#define APP_MENU_HISTORY_DEPTH 10
#define APP_MENU_INDICATOR_MIN_ITEMS 3
#define APP_MENU_INDICATOR_MAX_LEN 6
#if LCD_GEOMETRY_20X4
#define APP_MENU_ARROW CHAR_WIFI_TX
#else
#define APP_MENU_ARROW CHAR_BAR_5
#endif
#define APP_MENU_INDENT ' '

typedef struct {
    menu_state_t state;
    int selection;
} app_menu_history_entry_t;

typedef struct {
    app_menu_history_entry_t stack[APP_MENU_HISTORY_DEPTH];
    int depth;
} app_menu_history_t;

extern system_state_t sys_state;
extern lcd_render_state_t sys_lcd;
extern SemaphoreHandle_t sys_state_mutex;

static app_menu_history_t s_menu_history = {0};
// Main application navigation.
static const menu_item_t main_menu_items[] = {
    {"Settings", MENU_SETTINGS},
    {"Monitoring", MENU_MONITORING},
    {"Diagnostic", MENU_DIAGNOSTIC},
    {"Wi-Fi", MENU_WIFI_CONFIG},
    {"Firmware Update", MENU_OTA},
    {"Factory Reset", MENU_FACTORY_RESET},
    {"Security", MENU_SECURITY},
    {"View Settings", MENU_SYSTEM_INFO}};

// SETTINGS MENU (5 items)
static const menu_item_t settings_items[] = {
    {"Voltage Thresh", MENU_SETTINGS},
    {"Current Limit", MENU_SETTINGS},
    {"Freq Range", MENU_SETTINGS},
    {"Temp Alarm", MENU_SETTINGS},
    {"Sys Timeout", MENU_SETTINGS},
    {"Auto Shutdown", MENU_SETTINGS},
    {"Scroll Enable", MENU_SETTINGS},
    {"Scroll Speed", MENU_SETTINGS},
    {"Battery Type", MENU_SETTINGS},
    {"Voltage System", MENU_SETTINGS},
    {"Sound", MENU_SETTINGS},
    {"Quiet Hours", MENU_SETTINGS},
    {"Quiet Start", MENU_SETTINGS},
    {"Quiet End", MENU_SETTINGS},
    {"UTC Offset", MENU_SETTINGS},
    {"Set Hour", MENU_SETTINGS},
    {"Set Minute", MENU_SETTINGS}};

// MONITORING MENU (8 items)
static const menu_item_t monitoring_items[] = {
    {"Voltage", MENU_MONITORING},
    {"Current", MENU_MONITORING},
    {"Frequency", MENU_MONITORING},
    {"Temperature", MENU_MONITORING},
    {"Power Factor", MENU_MONITORING},
    {"Efficiency", MENU_MONITORING},
    {"Battery SOC", MENU_MONITORING},
    {"Battery Ah", MENU_MONITORING}};

// DIAGNOSTIC MENU (6 items)
static const menu_item_t diagnostic_items[] = {
    {"System Health", MENU_DIAGNOSTIC},
    {"Error Logs", MENU_DIAGNOSTIC},
    {"Performance", MENU_DIAGNOSTIC},
    {"Device Info", MENU_DIAGNOSTIC},
    {"Uptime", MENU_DIAGNOSTIC},
    {"Memory Usage", MENU_DIAGNOSTIC}};

// The top-level Wi-Fi menu stays focused on everyday actions. Technical
// configuration lives in the nested Settings menu below.
static const menu_item_t wifi_items[] = {
    {"ON/OFF", MENU_WIFI_CONFIG},
    {"Status", MENU_WIFI_CONFIG},
    {"Connect", MENU_WIFI_CONFIG},
    {"Networks", MENU_WIFI_CONFIG},
    {"Settings", MENU_WIFI_SETTINGS}};

static const menu_item_t wifi_settings_items[] = {
    {"Saved Network", MENU_WIFI_SETTINGS},
    {"Network Mode", MENU_WIFI_SETTINGS},
    {"DHCP", MENU_WIFI_SETTINGS},
    {"AP Clients", MENU_WIFI_SETTINGS}};

static const menu_item_t ota_items[] = {
    {"Check for Update", MENU_OTA},
    {"Install Available", MENU_OTA},
    {"Cancel Update", MENU_OTA}};

static const menu_item_t factory_reset_items[] = {
    {"Reset All Data", MENU_FACTORY_RESET},
    {"Clear Settings", MENU_FACTORY_RESET},
    {"Erase Logs", MENU_FACTORY_RESET}};

static const menu_item_t security_items[] = {
    {"Change PIN", MENU_SECURITY},
    {"View PIN Status", MENU_SECURITY},
    {"Reset PIN", MENU_SECURITY}};

static const char *wifi_menu_label(int index)
{
    switch (index) {
    case 0:
        return app_services_wifi_enabled() ? "Wi-Fi: ON" : "Wi-Fi: OFF";
    case 2:
        return app_services_wifi_connect_action_label();
    default:
        return wifi_items[index].label;
    }
}

static const char *wifi_settings_menu_label(int index)
{
    switch (index) {
    case 0:
        return app_services_wifi_saved_network_label();
    case 1:
    {
        static char mode[LCD_LINE_SIZE];
        snprintf(mode, sizeof(mode), "Mode: %s", app_services_wifi_mode_name());
        return mode;
    }
    case 2:
        return app_services_wifi_dhcp_enabled() ? "DHCP: ON" : "DHCP: OFF";
    default:
        return wifi_settings_items[index].label;
    }
}

const menu_item_t *get_menu_items(menu_state_t state, int *item_count)
{
    if (!item_count)
        return NULL;
    switch (state)
    {
    case MAIN_MENU:
        *item_count = sizeof(main_menu_items) / sizeof(main_menu_items[0]);
        return main_menu_items;

    case MENU_SETTINGS:
        *item_count = sizeof(settings_items) / sizeof(settings_items[0]);
        return settings_items;

    case MENU_MONITORING:
        *item_count = sizeof(monitoring_items) / sizeof(monitoring_items[0]);
        return monitoring_items;

    case MENU_DIAGNOSTIC:
        *item_count = sizeof(diagnostic_items) / sizeof(diagnostic_items[0]);
        return diagnostic_items;

    case MENU_WIFI_CONFIG:
        *item_count = sizeof(wifi_items) / sizeof(wifi_items[0]);
        return wifi_items;

    case MENU_WIFI_SETTINGS:
        *item_count = sizeof(wifi_settings_items) / sizeof(wifi_settings_items[0]);
        return wifi_settings_items;

    case MENU_OTA:
        *item_count = sizeof(ota_items) / sizeof(ota_items[0]);
        return ota_items;

    case MENU_FACTORY_RESET:
        *item_count = sizeof(factory_reset_items) / sizeof(factory_reset_items[0]);
        return factory_reset_items;
    case MENU_SECURITY:
        *item_count = sizeof(security_items) / sizeof(security_items[0]);
        return security_items;
    default:
        *item_count = 0;
        return NULL;
    }
}

void show_menu_screen(menu_state_t menu_st, int selection)
{
    const uint8_t security_menu_count = 3;

    if (menu_st == MENU_SECURITY)
    {
        if (selection < 0)
            selection = 0;
        if (selection >= security_menu_count)
            selection = security_menu_count - 1;

        LCD_LOCK();
        sys_lcd.screen = LCD_SCREEN_SECURITY;
        atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_IDLE);
        atomic_store(&sys_lcd.security.action, SECURITY_ACTION_NONE);
        atomic_store(&sys_lcd.security.menu_selection, (uint8_t)selection);
        LCD_UNLOCK();
        return;
    }

    int item_count = 0;
    const menu_item_t *items = get_menu_items(menu_st, &item_count);

    if (!items || item_count == 0)
    {
        char r0[LCD_LINE_SIZE], r1[LCD_LINE_SIZE];
        snprintf(r0, LCD_LINE_SIZE, "%-*s", LCD_COLS, "(empty menu)");
        snprintf(r1, LCD_LINE_SIZE, "%-*s", LCD_COLS, "");
        lcd_show_menu(r0, r1);
        return;
    }

    if (selection < 0)
        selection = 0;
    if (selection >= item_count)
        selection = item_count - 1;

    uint8_t cols = lcd_geometry_cols();
if (lcd_geometry_is_20x4())
{
    char rows[LCD_ROWS][LCD_LINE_SIZE];
    const char *row_ptrs[LCD_ROWS];
    const uint8_t visible_rows = lcd_geometry_rows();
    int top = selection >= visible_rows ? selection - visible_rows + 1 : 0;
    char indicator[APP_MENU_INDICATOR_MAX_LEN + 1];
    int indicator_len = snprintf(indicator, sizeof(indicator), "%d/%d",
                                 selection + 1, item_count);

    for (uint8_t row = 0; row < visible_rows; ++row)
    {
        int item_index = top + row;
        row_ptrs[row] = rows[row];
        if (item_index >= item_count)
        {
            if (row == visible_rows - 1)
                snprintf(rows[row], LCD_LINE_SIZE, "%*s%s",
                         cols - indicator_len, "", indicator);
            else
                snprintf(rows[row], LCD_LINE_SIZE, "%*s", cols, "");
            continue;
        }

        char marker = item_index == selection ? APP_MENU_ARROW : APP_MENU_INDENT;
        const char *label = menu_st == MENU_WIFI_CONFIG
                                ? wifi_menu_label(item_index)
                                : menu_st == MENU_WIFI_SETTINGS
                                    ? wifi_settings_menu_label(item_index)
                                    : items[item_index].label;
        int label_width = cols - 1;
        if (row == visible_rows - 1)
        {
            label_width -= indicator_len + 1;
            if (label_width < 1)
                label_width = 1;
            snprintf(rows[row], LCD_LINE_SIZE, "%c%-*.*s %s",
                     marker, label_width, label_width,
                     label, indicator);
        }
        else
        {
            snprintf(rows[row], LCD_LINE_SIZE, "%c%-*.*s",
                                          marker, label_width, label_width, label);
        }
    }
    lcd_show_menu_rows(row_ptrs, visible_rows);
}
else
{
    char r0[LCD_LINE_SIZE], r1[LCD_LINE_SIZE];

    /* Preserve the established 16×2 menu behavior. */
    const char *selected_label = menu_st == MENU_WIFI_CONFIG
                                     ? wifi_menu_label(selection)
                                     : menu_st == MENU_WIFI_SETTINGS
                                         ? wifi_settings_menu_label(selection)
                                         : items[selection].label;
    snprintf(r0, LCD_LINE_SIZE, "%c%-*.*s", APP_MENU_ARROW,
             cols - 1, cols - 1, selected_label);

    int next = (selection + 1) % item_count;
    if (item_count >= APP_MENU_INDICATOR_MIN_ITEMS)
    {
        char ind[APP_MENU_INDICATOR_MAX_LEN + 1];
        int ind_len = snprintf(ind, sizeof(ind), "%d/%d",
                               selection + 1, item_count);
        int label_w = cols - 1 - ind_len;
        if (label_w < 1)
            label_w = 1;
        const char *next_label = menu_st == MENU_WIFI_CONFIG
                                     ? wifi_menu_label(next)
                                     : menu_st == MENU_WIFI_SETTINGS
                                         ? wifi_settings_menu_label(next)
                                         : items[next].label;
        snprintf(r1, LCD_LINE_SIZE, "%c%-*.*s%s",
                 APP_MENU_INDENT, label_w, label_w,
                 next_label, ind);
    }
    else
    {
        const char *next_label = menu_st == MENU_WIFI_CONFIG
                                     ? wifi_menu_label(next)
                                     : menu_st == MENU_WIFI_SETTINGS
                                         ? wifi_settings_menu_label(next)
                                         : items[next].label;
        snprintf(r1, LCD_LINE_SIZE, "%c%-*.*s", APP_MENU_INDENT,
                 cols - 1, cols - 1, next_label);
    }
    lcd_show_menu(r0, r1);
}
}

/*==============================================================================
  CONVENIENCE: switch to main screen (wraps lcd_show_main)
==============================================================================*/
void go_to_main_screen(void)
{
    lcd_show_main();
}
void push_menu_history(menu_state_t state, uint8_t selection)
{
    if (s_menu_history.depth < APP_MENU_HISTORY_DEPTH)
    {
        s_menu_history.stack[s_menu_history.depth].state = state;
        s_menu_history.stack[s_menu_history.depth].selection = selection;
        s_menu_history.depth++;
        ESP_LOGD(APP_MENU_TAG, "History push: state=%d selection=%d depth=%d",
                 state, selection, s_menu_history.depth);
    }
    else
    {
        ESP_LOGW(APP_MENU_TAG, "Menu history is full");
    }
}

bool pop_menu_history(menu_state_t *state, int *selection)
{
    if (s_menu_history.depth > 0)
    {
        s_menu_history.depth--;
        *state = s_menu_history.stack[s_menu_history.depth].state;
        *selection = s_menu_history.stack[s_menu_history.depth].selection;
        ESP_LOGD(APP_MENU_TAG, "History pop: state=%d selection=%d depth=%d",
                 *state, *selection, s_menu_history.depth);
        return true;
    }
    ESP_LOGD(APP_MENU_TAG, "Menu history is empty");
    return false;
}

void clear_menu_history(void)
{
    s_menu_history.depth = 0;
    ESP_LOGD(APP_MENU_TAG, "Menu history cleared");
}

