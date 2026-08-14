#include "app_menu.h"

#include <stdatomic.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lcd_writer.h"
#include "security/security.h"

#define APP_MENU_TAG "APP_MENU"
#define APP_MENU_HISTORY_DEPTH 10
#define APP_MENU_INDICATOR_MIN_ITEMS 3
#define APP_MENU_INDICATOR_MAX_LEN 6
#define APP_MENU_ARROW '>'
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
    {"WiFi Control", MENU_WIFI_CONFIG},
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

// Wi-Fi control actions persist enablement intent in NVS and apply immediately.
static const menu_item_t wifi_items[] = {
    {"WiFi On / Off", MENU_WIFI_CONFIG},
    {"Scan Networks", MENU_WIFI_CONFIG},
    {"Connect Saved", MENU_WIFI_CONFIG},
    {"Disconnect", MENU_WIFI_CONFIG},
    {"Start Setup AP", MENU_WIFI_CONFIG},
    {"WiFi Status", MENU_WIFI_CONFIG}};

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

    char r0[17], r1[17];

    int item_count = 0;
    const menu_item_t *items = get_menu_items(menu_st, &item_count);

    if (!items || item_count == 0)
    {
        snprintf(r0, 17, "%-16s", "(empty menu)");
        snprintf(r1, 17, "%-16s", "");
        return;
    }

    if (selection < 0)
        selection = 0;
    if (selection >= item_count)
        selection = item_count - 1;

    /* Battery remaining is refreshed by lcd_update_main_data() on every ADC
     * cycle, so this menu render remains current in ON and STANDBY states. */
    snprintf(r0, 17, "%c%-10.10s %3u%%",
             APP_MENU_ARROW, items[selection].label,
             (unsigned)sys_lcd.battery_pct);

    /* Row 1: next item and position indicator; battery remains on row 0. */
    int next = (selection + 1) % item_count;
    if (item_count >= APP_MENU_INDICATOR_MIN_ITEMS)
    {
        char ind[12];
        snprintf(ind, sizeof(ind), "%u/%u",
                 (unsigned)((selection + 1) % 100),
                 (unsigned)(item_count % 100));
        snprintf(r1, 17, "%c%-8.8s %5.5s",
                 APP_MENU_INDENT, items[next].label, ind);
    }
    else
    {
        snprintf(r1, 17, "%c%-15.15s", APP_MENU_INDENT, items[next].label);
    }
    lcd_show_menu(r0, r1);
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

