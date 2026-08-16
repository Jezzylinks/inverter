#include "app_input.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "app_menu.h"
#include "app_services.h"
#include "battery/battery_estimator.h"
#include "events/system_events.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hardware_config.h"
#include "lcd.h"
#include "lcd_flash_queue.h"
#include "lcd_writer.h"
#include "utils.h"
#include "wifi/wifi_controller.h"
#include "security/change_pin_flow.h"
#include "security/factory_reset.h"
#include "security/security.h"
#include "system_state.h"
#include "utility/buzzer.h"

#define APP_INPUT_TAG "APP_INPUT"
#define APP_SEQUENCE_TIMEOUT_MS 3000U

extern system_state_t sys_state;
extern lcd_render_state_t sys_lcd;
extern battery_estimator_t bat_estimate;
extern SemaphoreHandle_t change_pin_mutex;
extern change_pin_ctx_t change_pin_ctx;

static uint8_t s_ota_auth_selection = 1U;
static bool s_ota_feedback_active = false;
static bool s_wifi_settings_child_active = false;

static void return_factory_reset_to_menu(void)
{
    const uint8_t selection = sys_state.menu_selection;
    factory_reset_cancel(&sys_lcd.factory_reset);
    sys_state.in_detail_view = false;
    sys_state.menu_state = MENU_FACTORY_RESET;
    sys_state.menu_selection = selection;
    sys_state.last_activity_time = esp_timer_get_time() / 1000;
    show_menu_screen(MENU_FACTORY_RESET, selection);
}

static void exit_factory_reset_to_home(void)
{
    factory_reset_cancel(&sys_lcd.factory_reset);
    sys_state.in_detail_view = false;
    sys_state.menu_state = MENU_NONE;
    sys_state.menu_selection = 0;
    clear_menu_history();
    go_to_main_screen();
}

static void exit_factory_reset_to_previous_menu(void)
{
    factory_reset_cancel(&sys_lcd.factory_reset);

    menu_state_t previous_menu;
    int previous_selection;
    if (pop_menu_history(&previous_menu, &previous_selection)) {
        sys_state.menu_state = previous_menu;
        sys_state.menu_selection = previous_selection;
        sys_state.last_activity_time = esp_timer_get_time() / 1000;
        if (previous_menu == MENU_NONE) {
            go_to_main_screen();
        } else {
            show_menu_screen(previous_menu, previous_selection);
        }
    } else {
        exit_factory_reset_to_home();
    }
}

static void begin_ota_auth(uint8_t selection)
{
    s_ota_auth_selection = selection;
    xSemaphoreTake(change_pin_mutex, portMAX_DELAY);
    change_pin_start_ex(&change_pin_ctx, CHANGE_PIN_MODE_VERIFY_ONLY);
    xSemaphoreGive(change_pin_mutex);

    sys_state.menu_state = MENU_SECURITY;
    sys_state.menu_selection = 0U;
    atomic_store(&sys_lcd.security.action, SECURITY_ACTION_OTA_AUTH);
    atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_PIN_FLOW);
    sys_lcd.screen = LCD_SCREEN_SECURITY;
}

static void return_from_ota_auth(bool authenticated)
{
    atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_IDLE);
    atomic_store(&sys_lcd.security.action, SECURITY_ACTION_NONE);
    sys_state.menu_state = MENU_OTA;
    sys_state.menu_selection = s_ota_auth_selection;

    if (authenticated) {
        if (app_services_request_update_confirmation() != ESP_OK) {
            show_menu_screen(MENU_OTA, sys_state.menu_selection);
        }
    } else {
        show_menu_screen(MENU_OTA, sys_state.menu_selection);
    }
}

extern void post_button_click_event(void);
extern void inverter_power_on(void);
extern void shutdown_inverter(void);
extern bool check_safety_conditions(void);
extern void inverter_show_last_start_error(void);
extern void enter_diagnostic_mode(void);
extern void exit_diagnostic_mode(void);
extern void perform_factory_reset(void);
extern void clear_settings(void);
extern void reload_default_settings(void);
extern bool save_settings(void);
extern void erase_logs(void);
extern void lcd_draw_diagnostics_screen(uint8_t index);
extern void lcd_draw_settings_view_screen(uint8_t index);
extern void lcd_show_monitoring_detail(const char *label, float value, const char *unit);
extern void enter_detail_view(menu_state_t parent_menu, int parent_selection);
extern void exit_detail_view(void);
extern void increase_value(bool fast_mode, bool precision_mode);
extern void decrease_value(bool fast_mode, bool precision_mode);
extern void exit_value_edit_mode(bool save_changes);
extern void handle_value_confirmation(void);
extern void update_system_parameter(value_edit_context_t *config, float new_value);
extern value_edit_context_t *get_current_value_config(void);
extern float *get_current_value_pointer(void);
extern void lcd_show_factory_reset_screen(void);
extern void lcd_show_value_saved_screen(void);
extern void lcd_show_value_canceled_screen(void);
extern void lcd_show_value_edit_screen(void);
extern const char *get_error_string(uint32_t flags);
extern void edit_voltage_threshold(void);
extern void edit_current_limit(void);
extern void edit_frequency_range(void);
extern void edit_temperature_alarm(void);
extern void edit_system_timeout(void);
extern void edit_auto_shutdown(void);
extern void edit_scroll_enable(void);
extern void edit_scroll_speed(void);
extern void edit_battery_type(void);
extern void edit_battery_voltage_system(void);
extern void edit_sound_enable(void);
extern void edit_quiet_hours_enable(void);
extern void edit_quiet_hours_start(void);
extern void edit_quiet_hours_end(void);
extern void edit_utc_offset(void);
extern void edit_set_time_hour(void);
extern void edit_set_time_minute(void);

static bool ota_confirmation_is_pending(void)
{
    app_ota_status_t status;
    app_services_get_ota_status(&status);
    return status.confirmation_pending;
}

static void handle_wifi_scan_move(bool up)
{
    uint8_t count = 0U;
    uint8_t selected = 0U;
    uint8_t top = 0U;
    LCD_LOCK();
    if (sys_lcd.screen != LCD_SCREEN_WIFI_SCAN) {
        LCD_UNLOCK();
        return;
    }
    count = sys_lcd.wifi_scan.count;
    selected = sys_lcd.wifi_scan.selected_index;
    top = sys_lcd.wifi_scan.top_index;
    LCD_UNLOCK();
    if (count == 0U) {
        return;
    }

    selected = up
        ? (selected == 0U ? count - 1U : selected - 1U)
        : (selected + 1U) % count;
    const uint8_t visible = lcd_geometry_is_20x4() ? 3U : 2U;
    if (selected < top) {
        top = selected;
    } else if (selected >= top + visible) {
        top = selected - visible + 1U;
    }
    lcd_update_wifi_selection(selected, top);
}

static void handle_wifi_clients_move(bool up)
{
    uint8_t count = 0U;
    uint8_t selected = 0U;
    LCD_LOCK();
    count = sys_lcd.wifi_clients.count;
    selected = sys_lcd.wifi_clients.selected;
    LCD_UNLOCK();
    if (count == 0U) return;
    selected = up
        ? (selected == 0U ? count - 1U : selected - 1U)
        : (selected + 1U) % count;
    lcd_update_wifi_client_selection(selected);
}

static void handle_wifi_password_char(bool up)
{
    static const char alphabet[] = " abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&()*+,-./:;=?@[]_";
    char password[LCD_WIFI_PASSWORD_MAX_LEN + 1U] = {0};
    char current = 'a';
    uint8_t length = 0U;
    LCD_LOCK();
    current = sys_lcd.wifi_password.current_char;
    length = sys_lcd.wifi_password.length;
    strncpy(password, sys_lcd.wifi_password.password, sizeof(password) - 1U);
    LCD_UNLOCK();
    const char *found = strchr(alphabet, current);
    size_t index = found ? (size_t)(found - alphabet) : 1U;
    const size_t count = sizeof(alphabet) - 1U;
    index = up ? (index + count - 1U) % count : (index + 1U) % count;
    lcd_update_wifi_password(alphabet[index], password, length);
}

static void handle_wifi_password_append(void)
{
    char password[LCD_WIFI_PASSWORD_MAX_LEN + 1U] = {0};
    char current = 'a';
    uint8_t length = 0U;
    LCD_LOCK();
    current = sys_lcd.wifi_password.current_char;
    length = sys_lcd.wifi_password.length;
    strncpy(password, sys_lcd.wifi_password.password, sizeof(password) - 1U);
    LCD_UNLOCK();
    if (length >= LCD_WIFI_PASSWORD_MAX_LEN) {
        lcd_flash_message("Password full", "Double=connect", 900U);
        return;
    }
    password[length++] = current;
    password[length] = '\0';
    lcd_update_wifi_password(current, password, length);
}

static void handle_wifi_password_submit(void)
{
    const esp_err_t err = app_services_wifi_submit_password();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        lcd_flash_message("Connect failed", "Try again", 1200U);
    }
}

static void handle_wifi_client_delete(void)
{
    uint8_t selected = 0U;
    LCD_LOCK();
    selected = sys_lcd.wifi_clients.selected;
    LCD_UNLOCK();
    const esp_err_t err = app_services_disconnect_ap_client_at(selected);
    if (err == ESP_OK) {
        app_services_show_ap_clients();
    } else {
        lcd_flash_message("Remove Failed", "Try again", 1200U);
    }
}

static void handle_wifi_menu_action(uint8_t selection)
{
    switch (selection) {
    case 0:
        (void)app_services_set_wifi_enabled(!app_services_wifi_enabled());
        show_menu_screen(MENU_WIFI_CONFIG, selection);
        break;
    case 1:
        app_services_show_wifi_status();
        break;
    case 2:
        if (app_services_wifi_is_ap_only()) {
            lcd_flash_message("AP mode active", "No STA connect", 1200U);
        } else if (wifi_controller_is_connected()) {
            (void)app_services_wifi_request_disconnect();
        } else {
            (void)app_services_wifi_reconnect();
        }
        break;
    case 3:
        (void)app_services_wifi_scan();
        break;
    case 4:
        s_wifi_settings_child_active = false;
        push_menu_history(MENU_WIFI_CONFIG, selection);
        sys_state.menu_state = MENU_WIFI_SETTINGS;
        sys_state.menu_selection = 0U;
        show_menu_screen(MENU_WIFI_SETTINGS, 0);
        break;
    default:
        break;
    }
}

static void handle_wifi_settings_action(uint8_t selection)
{
    switch (selection) {
    case 0:
        s_wifi_settings_child_active = false;
        (void)app_services_wifi_request_forget_saved();
        break;
    case 1:
        s_wifi_settings_child_active = true;
        lcd_flash_message("Network Mode", app_services_wifi_mode_name(), 1200U);
        break;
    case 2:
        s_wifi_settings_child_active = false;
        (void)app_services_wifi_toggle_dhcp();
        show_menu_screen(MENU_WIFI_SETTINGS, selection);
        break;
    case 3:
        s_wifi_settings_child_active = true;
        app_services_show_ap_clients();
        break;
    default:
        break;
    }
}

static void handle_ota_menu_action(uint8_t selection)
{
    switch (selection) {
    case 0:
        s_ota_feedback_active = true;
        (void)app_services_check_for_update(true);
        break;
    case 1:
        s_ota_feedback_active = true;
        if (sys_state.security.enabled) {
            begin_ota_auth(selection);
        } else {
            (void)app_services_request_update_confirmation();
        }
        break;
    case 2:
        s_ota_feedback_active = true;
        (void)app_services_cancel_update();
        break;
    default:
        break;
    }
}

/* ── handle_power_button_event() ─────────────────────────────────────────── */
void handle_power_button_event(button_event_info_t *event_info,
                               void *user_data)
{
    if (!sys_state.system_ready)
        return;

    if (event_info->event == BUTTON_EVENT_PRESS)
    {
        post_button_click_event();
    }
    int64_t current_time = event_info->timestamp_us / 1000;

    /* Never let the power button interrupt an in-progress factory reset.
     * The erase/format sequence must run to completion undisturbed. */
    if (atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_PHASE_PROGRESS)
    {
        return;
    }

    if (event_info->event == BUTTON_EVENT_CLICK && ota_confirmation_is_pending()) {
        (void)app_services_cancel_update();
        show_menu_screen(MENU_OTA, sys_state.menu_selection);
        return;
    }

    if (sys_state.menu_state == MENU_FACTORY_RESET &&
        atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_RESET_PIN_ENTRY)
    {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            exit_factory_reset_to_home();
        }
        return;
    }

    switch (event_info->event)
    {

    case BUTTON_EVENT_CLICK:
    {
        if (app_services_wifi_scan_is_active()) {
            (void)app_services_wifi_scan_cancel();
        }
        /* Cancel the active edit, then continue through the normal power
         * path so the power button always returns to the whole main page. */
        /* Always forget an abandoned edit/confirmation/detail operation before
         * continuing. The power button is the full-page exit. */
        exit_value_edit_mode(false);
        sys_state.in_confirmation_screen = false;
        sys_state.in_info_screen = false;
        if (sys_state.in_detail_view)
        {
            sys_state.in_detail_view = false;
            sys_state.inverter.inverter_state = sys_state.pre_detail_inverter_state;
        }
        /* P4: exit diagnostic mode */
        if (sys_state.inverter.inverter_state == INVERTER_DIAGNOSTIC)
        {
            exit_diagnostic_mode();
            sys_state.power_button_sequence_count = 0;
            break;
        }
        /* P5: close menu and clear any pending factory-reset session. */
        if (sys_state.menu_state != MENU_NONE)
        {
            factory_reset_cancel(&sys_lcd.factory_reset);
            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
            sys_state.in_detail_view = false;
            clear_menu_history();
            go_to_main_screen();
            break;
        }
        /* P6: contextual main screen */
        sys_state.power_button_sequence_count = 0;
        clear_menu_history();
        switch (sys_state.inverter.inverter_state)
        {
        case INVERTER_ON:
        case INVERTER_STARTING:
            go_to_main_screen();
            break;
        case INVERTER_STANDBY:
        {
            uint8_t pct = (uint8_t)battery_estimator_get_soc(&bat_estimate);
            lcd_show_standby(sys_state.inverter.battery.voltage, pct,
                             sys_state.inverter.connected);
            break;
        }
        case INVERTER_FAULT:
        {
            char r0[LCD_LINE_SIZE], r1[LCD_LINE_SIZE];
            snprintf(r0, LCD_LINE_SIZE, "%-*s", LCD_COLS, "** FAULT ACTIVE ");
            snprintf(r1, LCD_LINE_SIZE, "%-*.*s", LCD_COLS, LCD_COLS,
                     get_error_string(sys_state.error.error_flags));
            lcd_show_fault(r0, r1);
            vTaskDelay(pdMS_TO_TICKS(2000));
            go_to_main_screen();
            break;
        }
        default:
            go_to_main_screen();
            break;
        }
        break;
    } /* end BUTTON_EVENT_CLICK */

    case BUTTON_EVENT_LONG_PRESS:
    {

        /* A power hold abandons any setting edit; it must never leave a
         * stale option that can be reopened after the power operation. */
        if (sys_state.value_edit_mode)
            exit_value_edit_mode(false);
        if (sys_state.inverter.inverter_state == INVERTER_DIAGNOSTIC)
        {
            exit_diagnostic_mode();
            break;
        }
        /* Confirmed factory reset via triple-click + hold */
        if (sys_state.menu_state == MENU_FACTORY_RESET &&
            sys_state.power_button_sequence_count > 0)
        {
            sys_state.power_button_sequence_count = 0;
            clear_menu_history();
            lcd_show_factory_progress(0);
            vTaskDelay(pdMS_TO_TICKS(500));
            perform_factory_reset();
            break;
        }
        if (sys_state.menu_state != MENU_NONE)
        {
            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
            sys_state.in_detail_view = false;
            clear_menu_history();
        }
        switch (sys_state.inverter.inverter_state)
        {
        case INVERTER_OFF:
        case INVERTER_STANDBY:
            inverter_power_on();
            break;
        case INVERTER_ON:
        case INVERTER_STARTING:
            /* shutdown_inverter() already sets menu_state = MENU_NONE and
             * redraws the main screen — don't stomp on that afterward. */
            shutdown_inverter();
            break;
        case INVERTER_FAULT:
            lcd_show_fault("Clearing fault  ", "Please wait...  ");
            vTaskDelay(pdMS_TO_TICKS(1000));
            sys_state.error.error_flags &= (ERR_EEPROM | ERR_FAN_FAIL);
            sys_state.inverter.inverter_state = INVERTER_OFF;
            if (check_safety_conditions())
            {
                inverter_power_on();
                if (sys_state.inverter.inverter_state == INVERTER_ON)
                    gpio_set_level(GPIO_POWER_RELAY, 1);
            }
            else
            {
                inverter_show_last_start_error();
                post_buzzer_event(false);

                vTaskDelay(pdMS_TO_TICKS(2000));
                go_to_main_screen();
            }
            break;
        default:
            break;
        }
        break;
    }

    case BUTTON_EVENT_DOUBLE_CLICK:
    {
        ESP_LOGI("POWER BUTTON", "Button clicked twice");
        if (sys_state.value_edit_mode)
            exit_value_edit_mode(false);
        if (sys_state.inverter.inverter_state == INVERTER_ON ||
            sys_state.inverter.inverter_state == INVERTER_STARTING)
        {
            /* was 10000ms — every other flash in this file is ~1500ms */
            lcd_flash_info("Stop inverter   ", "before diag!    ", 1500);
            break;
        }
        if (sys_state.inverter.inverter_state != INVERTER_DIAGNOSTIC)
        {
            sys_state.in_detail_view = false;
            sys_state.in_confirmation_screen = false;
            sys_state.value_edit_mode = false;
            sys_state.value_changed = false;
            sys_state.pending_confirmation = false;
            clear_menu_history();
            enter_diagnostic_mode();
        }
        else
        {
            exit_diagnostic_mode();
        }
        sys_state.power_button_sequence_count = 0;
        break;
    }

    case BUTTON_EVENT_TRIPLE_CLICK:
    {
        ESP_LOGI("POWER BUTTON", "Clicked three times");
        if (sys_state.inverter.inverter_state == INVERTER_ON ||
            sys_state.inverter.inverter_state == INVERTER_STARTING)
        {
            lcd_flash_info("Stop inverter   ", "before reset!   ", 1500);
            break;
        }
        if (sys_state.inverter.inverter_state == INVERTER_DIAGNOSTIC)
            exit_diagnostic_mode();
        if (sys_state.value_edit_mode)
            exit_value_edit_mode(false);
        sys_state.in_detail_view = false;
        sys_state.in_confirmation_screen = false;
        push_menu_history(sys_state.menu_state, sys_state.menu_selection);
        sys_state.menu_state = MENU_FACTORY_RESET;
        sys_state.menu_selection = 0;
        sys_state.power_button_sequence_count = 1;
        sys_state.power_sequence_start_time = current_time;
        lcd_show_confirm("FACTORY RESET?  ", "Hold=Yes Back=No");
        break;
    }

    default:
        break;
    }

    /* Factory reset confirmation timeout */
    if (sys_state.power_button_sequence_count > 0 &&
        (current_time - sys_state.power_sequence_start_time) > APP_SEQUENCE_TIMEOUT_MS)
    {
        sys_state.power_button_sequence_count = 0;
        menu_state_t prev;
        int prev_sel;
        if (pop_menu_history(&prev, &prev_sel))
        {
            sys_state.menu_state = prev;
            sys_state.menu_selection = prev_sel;
        }
        else
        {
            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
        }
        factory_reset_cancel(&sys_lcd.factory_reset);
        go_to_main_screen();
    }

    sys_state.last_activity_time = current_time;
}

/* ── display_menu_state() ───────────────────────────────────────────────── */
menu_state_t display_menu_state(void)
{
    const char *r0, *r1;
    switch (sys_state.menu_state)
    {
    case MENU_NONE:
        r0 = "Main Screen     ";
        r1 = "Press Enter     ";
        break;
    case MAIN_MENU:
        r0 = "Main Menu       ";
        r1 = "Use Up/Down     ";
        break;
    case MENU_SETTINGS:
        r0 = "Settings        ";
        r1 = "Select Option   ";
        break;
    case MENU_MONITORING:
        r0 = "Monitoring      ";
        r1 = "View Stats      ";
        break;
    case MENU_DIAGNOSTIC:
        r0 = "Diagnostic      ";
        r1 = "Run Tests       ";
        break;
    case MENU_FACTORY_RESET:
        r0 = "Factory Reset?  ";
        r1 = "Enter=Yes Back=N";
        break;
    case MENU_WIFI_CONFIG:
        r0 = "WiFi Config     ";
        r1 = "Enter=Setup     ";
        break;
    default:
        r0 = "Unknown Menu    ";
        r1 = "                ";
        break;
    }
    lcd_show_menu(r0, r1);
    vTaskDelay(pdMS_TO_TICKS(1500));
    return sys_state.menu_state;
}

// ENTER/MENU BUTTON - Navigate menus and enter/confirm values
/* ── handle_enter_menu_button_event() ──────────────────────────────────── */

void handle_enter_menu_button_event(button_event_info_t *event_info,
                                    void *user_data)
{

    if (!sys_state.system_ready)
        return;

    if (event_info->event == BUTTON_EVENT_PRESS)
    {
        post_button_click_event();
    }

    if (atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_PHASE_PROGRESS)
    {
        return;
    }

    if (event_info->event == BUTTON_EVENT_CLICK && ota_confirmation_is_pending()) {
        (void)app_services_confirm_update();
        return;
    }

    if (event_info->event == BUTTON_EVENT_CLICK &&
        app_services_wifi_forget_confirmation_pending()) {
        (void)app_services_wifi_confirm_forget_saved();
        return;
    }
    if (event_info->event == BUTTON_EVENT_CLICK &&
        app_services_wifi_disconnect_confirmation_pending()) {
        (void)app_services_wifi_confirm_disconnect();
        return;
    }

    /* ── Factory reset PIN gate (intercept before everything else) ── */
    if (sys_state.menu_state == MENU_FACTORY_RESET &&
        atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_RESET_PIN_ENTRY)
    {
        if (event_info->event == BUTTON_EVENT_CLICK)
        {
            factory_reset_handle_pin_entry(&sys_lcd.factory_reset, BTN_ENTER);
        }
        return;
    }

    if (sys_state.menu_state == MENU_SECURITY)
    {
        security_phase_t phase = atomic_load(&sys_lcd.security.phase);

        if (phase == SECURITY_PHASE_VIEW_STATUS)
        {
            atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_IDLE);
            atomic_store(&sys_lcd.security.action, SECURITY_ACTION_NONE);
            return;
        }

        if (phase == SECURITY_PHASE_PIN_FLOW)
        {
            if (event_info->event != BUTTON_EVENT_CLICK)
            {
                return;
            }

            bool flow_done = false;

            xSemaphoreTake(change_pin_mutex, portMAX_DELAY);
            const security_action_t security_action =
                atomic_load(&sys_lcd.security.action);
            switch (security_action)
            {
            case SECURITY_ACTION_CHANGE_PIN:
            case SECURITY_ACTION_RESET_PIN:
            case SECURITY_ACTION_OTA_AUTH:
                flow_done = change_pin_handle_button(&change_pin_ctx, BTN_ENTER);
                break;
            default:
                flow_done = true;
                break;
            }
            xSemaphoreGive(change_pin_mutex);

            if (flow_done)
            {
                const bool ota_authenticated =
                    security_action == SECURITY_ACTION_OTA_AUTH &&
                    change_pin_ctx.phase == CHANGE_PIN_SUCCESS;
                if (security_action == SECURITY_ACTION_OTA_AUTH) {
                    return_from_ota_auth(ota_authenticated);
                } else {
                    atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_IDLE);
                    atomic_store(&sys_lcd.security.action, SECURITY_ACTION_NONE);
                    sys_lcd.screen = LCD_SCREEN_SECURITY;
                }
            }
            return;
        }
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_PASSWORD) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            handle_wifi_password_append();
        } else if (event_info->event == BUTTON_EVENT_DOUBLE_CLICK) {
            handle_wifi_password_submit();
        }
        return;
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_NETWORK_DETAILS) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            uint8_t page = 0U;
            LCD_LOCK();
            page = sys_lcd.wifi_network_detail.page;
            LCD_UNLOCK();
            if (lcd_geometry_is_20x4() || page >= 2U) {
                (void)app_services_wifi_connect_selected(0U);
            } else {
                lcd_update_wifi_network_detail_page((uint8_t)(page + 1U));
            }
        }
        return;
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_SCAN) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            lcd_wifi_scan_stage_t stage;
            uint8_t selected;
            LCD_LOCK();
            stage = sys_lcd.wifi_scan.stage;
            selected = sys_lcd.wifi_scan.selected_index;
            LCD_UNLOCK();
            if (stage == LCD_WIFI_SCAN_SCANNING) {
                (void)app_services_wifi_scan_cancel();
                show_menu_screen(MENU_WIFI_CONFIG, 1U);
            } else {
                /* Selecting a found network starts the connection flow. The
                 * connection screen owns the RSSI, animation, timeout, and
                 * terminal result instead of returning to Networks. */
                (void)app_services_wifi_connect_selected(selected);
            }
        }
        return;
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_CLIENTS) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            handle_wifi_client_delete();
        }
        return;
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_STATUS) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            uint8_t page;
            LCD_LOCK();
            page = sys_lcd.wifi_status.page;
            LCD_UNLOCK();
            lcd_update_wifi_status_page((uint8_t)(page + 1U));
        }
        return;
    }

    /* Standby is intentionally idle. Enter is the only control that changes
     * its information page; Up/Down and timer-driven rotation do nothing. */
    if (event_info->event == BUTTON_EVENT_CLICK &&
        sys_state.menu_state == MENU_NONE &&
        sys_state.inverter.inverter_state == INVERTER_STANDBY)
    {
        lcd_standby_next_page();
        return;
    }

    if (event_info->event == BUTTON_EVENT_CLICK &&
        sys_state.menu_state == MENU_NONE &&
        sys_lcd.screen == LCD_SCREEN_MAIN)
    {
        lcd_main_next_page();
        return;
    }

    switch (event_info->event)
    {

    case BUTTON_EVENT_CLICK:
        /* Value edit save */

        if (sys_state.value_edit_mode)
        {
            if (sys_state.pending_confirmation)
            {
                handle_value_confirmation();
            }
            else
            {
                exit_value_edit_mode(true);
            }
            break;
        }

        if (sys_state.menu_state == MENU_FACTORY_RESET &&
            atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_PHASE_CONFIRM)
        {
            const char *done_msg = "                ";
            switch (atomic_load(&sys_lcd.factory_reset.action))
            {
            case FACTORY_ACTION_RESET_ALL:
                ESP_LOGI(APP_INPUT_TAG, "Factory reset confirmed by user");
                atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_PROGRESS);
                atomic_store(&sys_lcd.factory_reset.progress_pct, 0);
                perform_factory_reset();
                atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_DONE);
                break;

            case FACTORY_ACTION_CLEAR_SETTINGS:
                ESP_LOGI(APP_INPUT_TAG, "Clear Settings confirmed by user");
                atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_PROGRESS);
                atomic_store(&sys_lcd.factory_reset.progress_pct, 0);
                clear_settings();
                reload_default_settings();
                save_settings();
                atomic_store(&sys_lcd.factory_reset.progress_pct, 100);
                post_buzzer_event(true);
                sys_state.power_button_sequence_count = 0;
                atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_DONE);
                done_msg = "Settings Cleared";
                break;

            case FACTORY_ACTION_ERASE_LOGS:
                ESP_LOGI(APP_INPUT_TAG, "Erase Logs confirmed by user");
                atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_PROGRESS);
                atomic_store(&sys_lcd.factory_reset.progress_pct, 0);
                erase_logs();
                atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_DONE);
                done_msg = "Logs Erased     ";
                break;

            default:
                break;
            }

            factory_reset_cancel(&sys_lcd.factory_reset);
            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
            clear_menu_history();
            go_to_main_screen();
            lcd_flash_info("Done            ", done_msg, 1500);
            break;
        }

        /* Menu navigation */
        switch (sys_state.menu_state)
        {
        case MAIN_MENU:
        {
            menu_state_t next = MENU_NONE;
            switch (sys_state.menu_selection)
            {
            case 0:
                next = MENU_SETTINGS;
                break;
            case 1:
                next = MENU_MONITORING;
                break;
            case 2:
                next = MENU_DIAGNOSTIC;
                break;
            case 3:
                next = MENU_WIFI_CONFIG;
                break;
            case 4:
                next = MENU_OTA;
                break;
            case 5:
                next = MENU_FACTORY_RESET;
                break;
            case 6:
                next = MENU_SECURITY;
                break;
            case 7:
                next = MENU_SYSTEM_INFO;
                break;
            }
            if (next != MENU_NONE)
            {
                push_menu_history(sys_state.menu_state, sys_state.menu_selection);
                sys_state.menu_state = next;
                sys_state.menu_selection = 0;
                show_menu_screen(next, sys_state.menu_selection);

                if (next == MENU_SYSTEM_INFO)
                {
                    lcd_draw_settings_view_screen(0);
                }
            }
            break;
        }

        case MENU_SETTINGS:
            switch (sys_state.menu_selection)
            {
            case 0:
                edit_voltage_threshold();
                break;
            case 1:
                edit_current_limit();
                break;
            case 2:
                edit_frequency_range();
                break;
            case 3:
                edit_temperature_alarm();
                break;
            case 4:
                edit_system_timeout();
                break;
            case 5:
                edit_auto_shutdown();
                break;
            case 6:
                edit_scroll_enable();
                break;
            case 7:
                edit_scroll_speed();
                break;
            case 8:
                edit_battery_type();
                break;
            case 9:
                edit_battery_voltage_system();
                break;
            case 10:
                edit_sound_enable();
                break;
            case 11:
                edit_quiet_hours_enable();
                break;
            case 12:
                edit_quiet_hours_start();
                break;
            case 13:
                edit_quiet_hours_end();
                break;
            case 14:
                edit_utc_offset();
                break;
            case 15:
                edit_set_time_hour();
                break;
            case 16:
                edit_set_time_minute();
                break;
            }
            break;

        case MENU_MONITORING:

            if (sys_state.menu_selection < 8)
            {
                enter_detail_view(MENU_MONITORING, sys_state.menu_selection);
                switch (sys_state.menu_selection)
                {
                case 0:
                    lcd_show_monitoring_detail("Voltage", sys_state.inverter.output_voltage, "V");
                    break;
                case 1:
                    lcd_show_monitoring_detail("Current", sys_state.actual_current, "A");
                    break;
                case 2:
                    lcd_show_monitoring_detail("Frequency", sys_state.inverter.output_frequency, "Hz");
                    break;
                case 3:
                    lcd_show_monitoring_detail("Temperature", sys_state.actual_temperature, "C");
                    break;
                case 4:
                    lcd_show_monitoring_detail("Power Factor", sys_state.power_factor, "");
                    break;
                case 5:
                    lcd_show_monitoring_detail("Efficiency", sys_state.efficiency, "%");
                    break;
                case 6:
                    lcd_show_monitoring_detail("Battery SOC", battery_estimator_get_soc(&bat_estimate), "%");
                    break;
                case 7:
                    lcd_show_monitoring_detail("Battery Ah", battery_estimator_get_remaining_ah(&bat_estimate), "Ah");
                    break;
                }
            }
            break;

        case MENU_DIAGNOSTIC:
        {
            int cnt = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &cnt);
            if (!items || sys_state.menu_selection >= (uint8_t)cnt)
                break;
            enter_detail_view(MENU_DIAGNOSTIC, sys_state.menu_selection);
            lcd_draw_diagnostics_screen(sys_state.menu_selection);
            break;
        }

        case MENU_WIFI_CONFIG:
            handle_wifi_menu_action(sys_state.menu_selection);
            break;

        case MENU_WIFI_SETTINGS:
            handle_wifi_settings_action(sys_state.menu_selection);
            break;

        case MENU_OTA:
            handle_ota_menu_action(sys_state.menu_selection);
            break;

        case MENU_FACTORY_RESET:
            if (atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_PHASE_DONE)
            {
                sys_lcd.screen = LCD_SCREEN_MAIN;
                factory_reset_cancel(&sys_lcd.factory_reset);
                break;
            }
            else if (atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_PHASE_IDLE)
            {
                sys_lcd.screen = LCD_SCREEN_FACTORY_RESET;
            }

            switch (sys_state.menu_selection)
            {
            case 0:
                factory_reset_begin(&sys_lcd.factory_reset, FACTORY_ACTION_RESET_ALL);
                break;
            case 1:
                factory_reset_begin(&sys_lcd.factory_reset, FACTORY_ACTION_CLEAR_SETTINGS);
                break;
            case 2:
                factory_reset_begin(&sys_lcd.factory_reset, FACTORY_ACTION_ERASE_LOGS);
                break;
            }
            pin_entry_reset(&sys_lcd.factory_reset.pin_ctx);
            atomic_store(&sys_lcd.factory_reset.phase, FACTORY_RESET_PIN_ENTRY);
            sys_lcd.screen = LCD_SCREEN_FACTORY_RESET;
            break;

        case MENU_SECURITY:
        {

            switch (sys_state.menu_selection)
            {
            case 0: /* Change PIN */
            {
                xSemaphoreTake(change_pin_mutex, portMAX_DELAY);
                change_pin_start_ex(&change_pin_ctx, CHANGE_PIN_MODE_SET_NEW);
                xSemaphoreGive(change_pin_mutex);

                atomic_store(&sys_lcd.security.action, SECURITY_ACTION_CHANGE_PIN);
                atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_PIN_FLOW);

                /* Force-change flag: if PIN is still factory default, flash a
                 * reminder so the user knows why they're being prompted. */
                if (security_pin_change_required())
                {
                    lcd_flash_info("Set your PIN    ", "Default is 0000 ", 1500);
                }
                break;
            }

            case 1: /* View PIN status */
            {
                atomic_store(&sys_lcd.security.action, SECURITY_ACTION_VIEW_STATUS);
                atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_VIEW_STATUS);
                break;
            }

            case 2: /* Reset PIN to factory default (0000) */
            {
                xSemaphoreTake(change_pin_mutex, portMAX_DELAY);
                change_pin_start_ex(&change_pin_ctx, CHANGE_PIN_MODE_RESET_DEFAULT);
                xSemaphoreGive(change_pin_mutex);

                atomic_store(&sys_lcd.security.action, SECURITY_ACTION_RESET_PIN);
                atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_PIN_FLOW);
                break;
            }

            default:
                break;
            }
            break; /* end case MENU_SECURITY */
        }
        default:
            break;
        }
        break;
    case BUTTON_EVENT_LONG_PRESS:
        switch (sys_state.menu_state)
        {
        case MENU_NONE:
            /* Always opens the Main Menu, regardless of inverter state. */
            sys_state.menu_state = MAIN_MENU;
            sys_state.menu_selection = 0;
            show_menu_screen(MAIN_MENU, 0);
            break;
        case MAIN_MENU:
        {
            menu_state_t next = MENU_NONE;
            switch (sys_state.menu_selection)
            {
            case 0:
                next = MENU_SETTINGS;
                break;
            case 1:
                next = MENU_MONITORING;
                sys_lcd.screen = LCD_SCREEN_MONITORING_DETAIL;
                break;
            case 2:
                next = MENU_DIAGNOSTIC;
                sys_lcd.screen = LCD_SCREEN_DIAGNOSTIC;
                break;
            case 3:
                next = MENU_WIFI_CONFIG;
                break;
            case 4:
                next = MENU_OTA;
                break;
            case 5:
                next = MENU_FACTORY_RESET;
                sys_lcd.screen = LCD_SCREEN_FACTORY_RESET;
                break;
            }
            if (next != MENU_NONE)
            {
                push_menu_history(sys_state.menu_state, sys_state.menu_selection);
                sys_state.menu_state = next;
                sys_state.menu_selection = 0;
                show_menu_screen(next, 0);
            }
            break;
        }

        case MENU_MONITORING:
            if (sys_state.menu_selection < 8)
            {
                enter_detail_view(MENU_MONITORING, sys_state.menu_selection);
                switch (sys_state.menu_selection)
                {
                case 0:
                    lcd_show_monitoring_detail("Voltage", sys_state.inverter.output_voltage, "V");
                    break;
                case 1:
                    lcd_show_monitoring_detail("Current", sys_state.actual_current, "A");
                    break;
                case 2:
                    lcd_show_monitoring_detail("Frequency", sys_state.inverter.output_frequency, "Hz");
                    break;
                case 3:
                    lcd_show_monitoring_detail("Temperature", sys_state.actual_temperature, "C");
                    break;
                case 4:
                    lcd_show_monitoring_detail("Power Factor", sys_state.power_factor, "");
                    break;
                case 5:
                    lcd_show_monitoring_detail("Efficiency", sys_state.efficiency, "%");
                    break;
                case 6:
                    lcd_show_monitoring_detail("Battery SOC", battery_estimator_get_soc(&bat_estimate), "%");
                    break;
                case 7:
                    lcd_show_monitoring_detail("Battery Ah", battery_estimator_get_remaining_ah(&bat_estimate), "Ah");
                    break;
                }
            }
            break;
        case MENU_DIAGNOSTIC:
        {
            int cnt = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &cnt);
            if (!items || sys_state.menu_selection >= (uint8_t)cnt)
                break;
            enter_detail_view(MENU_DIAGNOSTIC, sys_state.menu_selection);
            lcd_draw_diagnostics_screen(sys_state.menu_selection);
            break;
        }
        case MENU_WIFI_CONFIG:
            handle_wifi_menu_action(sys_state.menu_selection);
            break;
        case MENU_WIFI_SETTINGS:
            handle_wifi_settings_action(sys_state.menu_selection);
            break;
        case MENU_OTA:
            handle_ota_menu_action(sys_state.menu_selection);
            break;
        case MENU_FACTORY_RESET:
            atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_CONFIRM);
            lcd_show_factory_reset_screen();
            break;
        default:
            break;
        }
        break; /* BUTTON_EVENT_LONG_PRESS */

    case BUTTON_EVENT_DOUBLE_CLICK:

        if (sys_state.value_edit_mode && sys_state.pending_confirmation)
        {
            value_edit_context_t *config = get_current_value_config();
            float *current_value = get_current_value_pointer();
            if (config && current_value)
            {
                update_system_parameter(config, *current_value);
                sys_state.value_changed = true;
                exit_value_edit_mode(true);
                show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
                lcd_flash_info_to("Value Saved!    ", "                ",
                                  800, LCD_SCREEN_MENU);
            }
        }
        else if (sys_state.menu_state == MAIN_MENU)
        {
            push_menu_history(sys_state.menu_state, sys_state.menu_selection);
            sys_state.menu_state = MENU_MONITORING;
            sys_state.menu_selection = 0;
            show_menu_screen(MENU_MONITORING, 0);
        }
        break;

    default:
        break;
    } // switch(event_info->event)
    ESP_LOGI("CONFIRM", "End  of program");
    sys_state.last_activity_time = event_info->timestamp_us / 1000;
}

/* ── acceleration tiers ──────────────────────────────────────────────────
 * Returns which increase_value() flags to use based on how long
 * the button has been continuously held.
 */
static void get_accel_step(int64_t hold_duration_ms, bool *fast, bool *big)
{
    if (hold_duration_ms < 800)
    {
        *fast = false;
        *big = false; /* normal speed */
    }
    else if (hold_duration_ms < 2500)
    {
        *fast = true;
        *big = false; /* ramping up   */
    }
    else
    {
        *fast = true;
        *big = true; /* max speed    */
    }
}

// Up button event handler - Advanced value increase
/* ── handle_up_button_event() ──────────────────────────────────────────── */
void handle_up_button_event(button_event_info_t *event_info,
                            void *user_data)
{
    if (!sys_state.system_ready)
        return;

    if (event_info->event == BUTTON_EVENT_PRESS)
    {
        post_button_click_event();
    }
    int64_t current_time = event_info->timestamp_us / 1000;
    value_edit_context_t *config = get_current_value_config();

    // handle_up_button_event():
    if (sys_state.menu_state == MENU_FACTORY_RESET &&
        atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_RESET_PIN_ENTRY)
    {
        if (event_info->event == BUTTON_EVENT_CLICK)
        {
            factory_reset_handle_pin_entry(&sys_lcd.factory_reset, BTN_UP);
        }
        return;
    }

    // When in security mode
    if (sys_state.menu_state == MENU_SECURITY &&
        atomic_load(&sys_lcd.security.phase) == SECURITY_PHASE_PIN_FLOW)
    {
        if (event_info->event == BUTTON_EVENT_CLICK)
        {
            xSemaphoreTake(change_pin_mutex, portMAX_DELAY);
            change_pin_handle_button(&change_pin_ctx, BTN_UP);
            xSemaphoreGive(change_pin_mutex);
        }
        return; // Up just adjusts the current PIN digit -- never finishes the flow
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_PASSWORD) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            handle_wifi_password_char(true);
        }
        return;
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_NETWORK_DETAILS) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            uint8_t page = 0U;
            LCD_LOCK();
            page = sys_lcd.wifi_network_detail.page;
            LCD_UNLOCK();
            lcd_update_wifi_network_detail_page(page == 0U ? 2U : (uint8_t)(page - 1U));
        }
        return;
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_SCAN) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            handle_wifi_scan_move(true);
        }
        return;
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_CLIENTS) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            handle_wifi_clients_move(true);
        }
        return;
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_STATUS) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            uint8_t page;
            LCD_LOCK();
            page = sys_lcd.wifi_status.page;
            LCD_UNLOCK();
            lcd_update_wifi_status_page((uint8_t)(page + 1U));
        }
        return;
    }

    // View Settings screen -- scroll through g_settings[] with wraparound
    if (sys_state.menu_state == MENU_SYSTEM_INFO)
    {
        if (event_info->event == BUTTON_EVENT_CLICK)
        {
            sys_state.menu_selection =
                (sys_state.menu_selection + 1) % (uint8_t)app_settings_count();
            lcd_draw_settings_view_screen(sys_state.menu_selection);
        }
        return;
    }

    switch (event_info->event)
    {
    case BUTTON_EVENT_CLICK:
        // Reset flashed info if any
        if (sys_state.value_edit_mode)
        {
            switch (config->edit_type)
            {
            case VALUE_EDIT_NUMERIC:
                ESP_LOGI("VALUE EDIT", "Increasing numeric value");
                increase_value(false, false);
                break;
            case VALUE_EDIT_SELECT:
                if (config->max_selection > 0)
                {
                    config->selection_index =
                        (config->selection_index + 1) % config->max_selection;
                    config->current_value = (float)config->selection_index;
                }
                break;
            case VALUE_EDIT_BOOL:
                config->current_value = (config->current_value != 0.0f) ? 0.0f : 1.0f;
                break;
            case VALUE_EDIT_LIST:
                config->list_index =
                    (config->list_index + 1) % config->list_size;
                break;
            default:
                break;
            }
            lcd_show_value_edit_screen();
        }
        else if (sys_state.menu_state == MENU_DIAGNOSTIC &&
                 !sys_state.in_detail_view)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &n);
            if (!items || n == 0)
                break;
            sys_state.menu_selection =
                (sys_state.menu_selection == 0) ? n - 1
                                                : sys_state.menu_selection - 1;
            sys_state.detail_parent_selection = sys_state.menu_selection;
            sys_state.detail_parent_menu = MENU_DIAGNOSTIC;
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
        }
        else if (sys_state.menu_state == MENU_FACTORY_RESET && !sys_state.in_detail_view)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(MENU_FACTORY_RESET, &n);
            if (!items || n == 0)
                break;
            sys_state.menu_selection =
                (sys_state.menu_selection == 0) ? n - 1
                                                : sys_state.menu_selection - 1;
            sys_state.detail_parent_selection = sys_state.menu_selection;
            sys_state.detail_parent_menu = MENU_FACTORY_RESET;
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
        }
        else if (!sys_state.in_detail_view && sys_state.menu_state != MENU_NONE)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state, &n);
            if (items && n > 0)
            {
                sys_state.menu_selection =
                    (sys_state.menu_selection == 0) ? n - 1
                                                    : sys_state.menu_selection - 1;
                show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            }
        }
        break;

    case BUTTON_EVENT_DOUBLE_CLICK:
        if (sys_state.value_edit_mode)
        {
            increase_value(false, true);
            lcd_show_value_edit_screen();
        }
        else if (sys_state.in_detail_view &&
                 sys_state.menu_state == MENU_DIAGNOSTIC)
        {
            sys_state.menu_selection = 0;
            sys_state.detail_parent_selection = 0;
            lcd_draw_diagnostics_screen(0);
        }
        else if (sys_state.in_detail_view)
        {
            exit_detail_view();
        }
        else if (sys_state.menu_state != MENU_NONE)
        {
            sys_state.menu_selection = 0;
            show_menu_screen(sys_state.menu_state, 0);
        }
        break;

    case BUTTON_EVENT_LONG_PRESS:
        if (sys_state.value_edit_mode)
        {
            sys_state.hold_start_time = current_time;
            sys_state.repeat_count = 0;
            sys_state.fast_increment_active = false;
            increase_value(false, false);
            lcd_show_value_edit_screen();
        }
        else if (sys_state.in_detail_view &&
                 sys_state.menu_state == MENU_DIAGNOSTIC)
        {
            sys_state.menu_selection = 0;
            sys_state.detail_parent_selection = 0;
            lcd_draw_diagnostics_screen(0);
        }
        else if (sys_state.in_detail_view)
        {
            exit_detail_view();
        }
        else if (sys_state.menu_state != MENU_NONE)
        {
            sys_state.menu_selection = 0;
            show_menu_screen(sys_state.menu_state, 0);
        }
        break;

    case BUTTON_EVENT_REPEAT:
        /* Hold-repeat is reserved for accelerated value editing. A menu move
         * requires a discrete click, preventing a retained press from
         * continuously scrolling the menu. */
        if (sys_state.value_edit_mode)
        {
            if (sys_state.hold_start_time == 0)
                sys_state.hold_start_time = current_time;

            int64_t held_ms = current_time - sys_state.hold_start_time;
            bool fast, big;
            get_accel_step(held_ms, &fast, &big);

            sys_state.repeat_count++;
            sys_state.fast_increment_active = fast;
            increase_value(fast, big);
            lcd_show_value_edit_screen();
        }
        break;

    case BUTTON_EVENT_RELEASE:
        sys_state.repeat_count = 0;
        sys_state.fast_increment_active = false;
        sys_state.hold_start_time = 0;
        break;

    default:
        break;
    }

    sys_state.last_activity_time = current_time;
    sys_state.last_increment_time = current_time;
}

// Down button event handler - Advanced value decrease
/* ── handle_down_button_event() ─────────────────────────────────────────── */
void handle_down_button_event(button_event_info_t *event_info,
                              void *user_data)
{
    if (!sys_state.system_ready)
        return;

    if (event_info->event == BUTTON_EVENT_PRESS)
    {
        post_button_click_event();
    }
    int64_t current_time = event_info->timestamp_us / 1000;
    value_edit_context_t *config = get_current_value_config();

    if (sys_state.menu_state == MENU_FACTORY_RESET &&
        atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_RESET_PIN_ENTRY)
    {
        if (event_info->event == BUTTON_EVENT_CLICK)
        {
            factory_reset_handle_pin_entry(&sys_lcd.factory_reset, BTN_DOWN);
        }
        return;
    }

    // When the user is in security mode
    if (sys_state.menu_state == MENU_SECURITY &&
        atomic_load(&sys_lcd.security.phase) == SECURITY_PHASE_PIN_FLOW)
    {
        if (event_info->event == BUTTON_EVENT_CLICK)
        {
            xSemaphoreTake(change_pin_mutex, portMAX_DELAY);
            change_pin_handle_button(&change_pin_ctx, BTN_DOWN);
            xSemaphoreGive(change_pin_mutex);
        }
        return;
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_PASSWORD) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            handle_wifi_password_char(false);
        }
        return;
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_NETWORK_DETAILS) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            uint8_t page = 0U;
            LCD_LOCK();
            page = sys_lcd.wifi_network_detail.page;
            LCD_UNLOCK();
            lcd_update_wifi_network_detail_page((uint8_t)((page + 1U) % 3U));
        }
        return;
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_SCAN) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            handle_wifi_scan_move(false);
        }
        return;
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_CLIENTS) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            handle_wifi_clients_move(false);
        }
        return;
    }

    if (sys_lcd.screen == LCD_SCREEN_WIFI_STATUS) {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            uint8_t page;
            LCD_LOCK();
            page = sys_lcd.wifi_status.page;
            LCD_UNLOCK();
            lcd_update_wifi_status_page((uint8_t)(page + 2U));
        }
        return;
    }

    // View Settings screen -- scroll through g_settings[] with wraparound
    if (sys_state.menu_state == MENU_SYSTEM_INFO)
    {
        if (event_info->event == BUTTON_EVENT_CLICK)
        {
            sys_state.menu_selection =
                (sys_state.menu_selection == 0)
                    ? (uint8_t)app_settings_count() - 1
                    : sys_state.menu_selection - 1;
            lcd_draw_settings_view_screen(sys_state.menu_selection);
        }
        return;
    }

    switch (event_info->event)
    {
    case BUTTON_EVENT_CLICK:
        if (sys_state.value_edit_mode)
        {
            switch (config->edit_type)
            {
            case VALUE_EDIT_NUMERIC:
                decrease_value(false, false);
                break;
            case VALUE_EDIT_SELECT:
                if (config->max_selection > 0)
                {
                    config->selection_index =
                        (config->selection_index > 0)
                            ? config->selection_index - 1
                            : config->max_selection - 1;
                    config->current_value = (float)config->selection_index;
                }
                break;
            case VALUE_EDIT_BOOL:
                config->current_value = (config->current_value != 0.0f) ? 0.0f : 1.0f;
                break;
            case VALUE_EDIT_LIST:
                config->list_index =
                    (config->list_index > 0)
                        ? config->list_index - 1
                        : config->list_size - 1;
                break;
            default:
                break;
            }
            lcd_show_value_edit_screen();
        }
        else if (sys_state.menu_state == MENU_DIAGNOSTIC &&
                 !sys_state.in_detail_view)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &n);
            if (!items || n == 0)
                break;
            sys_state.menu_selection =
                (sys_state.menu_selection + 1) % n;
            sys_state.detail_parent_selection = sys_state.menu_selection;
            sys_state.detail_parent_menu = MENU_DIAGNOSTIC;
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
        }
        else if (sys_state.menu_state == MENU_FACTORY_RESET && !sys_state.in_detail_view)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(MENU_FACTORY_RESET, &n);
            if (!items || n == 0)
                break;
            sys_state.menu_selection =
                (sys_state.menu_selection + 1) % n;
            sys_state.detail_parent_selection = sys_state.menu_selection;
            sys_state.detail_parent_menu = MENU_FACTORY_RESET;
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
        }
        else if (!sys_state.in_detail_view && sys_state.menu_state != MENU_NONE)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state, &n);
            if (items && n > 0)
            {
                sys_state.menu_selection =
                    (sys_state.menu_selection + 1) % n;
                show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            }
        }
        break;

    case BUTTON_EVENT_DOUBLE_CLICK:
        if (sys_state.value_edit_mode)
        {
            decrease_value(false, true);
            lcd_show_value_edit_screen();
        }
        else if (sys_state.in_detail_view &&
                 sys_state.menu_state == MENU_DIAGNOSTIC)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &n);
            if (items && n > 0)
            {
                sys_state.menu_selection = n - 1;
                sys_state.detail_parent_selection = n - 1;
                lcd_draw_diagnostics_screen(sys_state.menu_selection);
            }
        }
        else if (sys_state.in_detail_view)
        {
            exit_detail_view();
        }
        else if (sys_state.menu_state != MENU_NONE)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state, &n);
            if (items && n > 0)
            {
                sys_state.menu_selection = n - 1;
                show_menu_screen(sys_state.menu_state, n - 1);
            }
        }
        break;

    case BUTTON_EVENT_LONG_PRESS:
        if (sys_state.value_edit_mode)
        {
            sys_state.hold_start_time = current_time;
            sys_state.repeat_count = 0;
            sys_state.fast_increment_active = false;
            decrease_value(false, false);
            lcd_show_value_edit_screen();
        }
        else if (sys_state.in_detail_view &&
                 sys_state.menu_state == MENU_DIAGNOSTIC)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(MENU_DIAGNOSTIC, &n);
            if (items && n > 0)
            {
                sys_state.menu_selection = n - 1;
                sys_state.detail_parent_selection = n - 1;
                lcd_draw_diagnostics_screen(sys_state.menu_selection);
            }
        }
        else if (sys_state.in_detail_view)
        {
            exit_detail_view();
        }
        else if (sys_state.menu_state != MENU_NONE)
        {
            int n = 0;
            const menu_item_t *items = get_menu_items(sys_state.menu_state, &n);
            if (items && n > 0)
            {
                sys_state.menu_selection = n - 1;
                show_menu_screen(sys_state.menu_state, n - 1);
            }
        }
        break;

    case BUTTON_EVENT_REPEAT:
        /* Hold-repeat is reserved for accelerated value editing. A menu move
         * requires a discrete click, preventing a retained press from
         * continuously scrolling the menu. */
        if (sys_state.value_edit_mode)
        {
            if (sys_state.hold_start_time == 0)
                sys_state.hold_start_time = current_time;

            int64_t held_ms = current_time - sys_state.hold_start_time;
            bool fast, big;
            get_accel_step(held_ms, &fast, &big);

            sys_state.repeat_count++;
            sys_state.fast_increment_active = fast;
            decrease_value(fast, big);
            lcd_show_value_edit_screen();
        }
        break;

    case BUTTON_EVENT_RELEASE:
        sys_state.repeat_count = 0;
        sys_state.fast_increment_active = false;
        sys_state.hold_start_time = 0;
        break;

    default:
        break;
    }

    sys_state.last_activity_time = current_time;
    sys_state.last_increment_time = current_time;
}

/* ── handle_back_button_event() ─────────────────────────────────────────── */
void handle_back_button_event(button_event_info_t *event_info,
                              void *user_data)
{
    if (!sys_state.system_ready)
        return;

    if (event_info->event == BUTTON_EVENT_PRESS)
    {
        post_button_click_event();
    }

    if (event_info->event == BUTTON_EVENT_CLICK && ota_confirmation_is_pending()) {
        (void)app_services_cancel_update();
        show_menu_screen(MENU_OTA, sys_state.menu_selection);
        return;
    }

    // User is in Security mode

    if (sys_state.menu_state == MENU_FACTORY_RESET &&
        atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_RESET_PIN_ENTRY)
    {
        if (event_info->event == BUTTON_EVENT_CLICK) {
            return_factory_reset_to_menu();
        }
        return;
    }

    if (sys_state.menu_state == MENU_SECURITY)
    {

        if (atomic_load(&sys_lcd.security.phase) == SECURITY_PHASE_PIN_FLOW)

        {
            if (event_info->event != BUTTON_EVENT_CLICK)
            {
                return;
            }

            xSemaphoreTake(change_pin_mutex, portMAX_DELAY);
            const security_action_t security_action =
                atomic_load(&sys_lcd.security.action);
            bool flow_done = change_pin_handle_button(&change_pin_ctx, BTN_BACK);
            xSemaphoreGive(change_pin_mutex);

            if (flow_done)
            {
                ESP_LOGI("DISPLAY MODE", "DISPLAYING MODE");
                if (security_action == SECURITY_ACTION_OTA_AUTH) {
                    return_from_ota_auth(false);
                } else {
                    atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_IDLE);
                    atomic_store(&sys_lcd.security.action, SECURITY_ACTION_NONE);
                    sys_lcd.screen = LCD_SCREEN_SECURITY;
                }
            }
            return;
        }

        if (
            atomic_load(&sys_lcd.security.phase) == SECURITY_PHASE_VIEW_STATUS)
        {
            atomic_store(&sys_lcd.security.phase, SECURITY_PHASE_IDLE);
            atomic_store(&sys_lcd.security.action, SECURITY_ACTION_NONE);
            return;
        }
    }

    if (event_info->event == BUTTON_EVENT_CLICK &&
        sys_state.menu_state == MENU_OTA &&
        s_ota_feedback_active &&
        sys_lcd.screen == LCD_SCREEN_FLASH_MSG) {
        s_ota_feedback_active = false;
        show_menu_screen(MENU_OTA, sys_state.menu_selection);
        return;
    }

    if (event_info->event == BUTTON_EVENT_CLICK &&
        sys_state.menu_state == MENU_WIFI_SETTINGS &&
        s_wifi_settings_child_active &&
        (sys_lcd.screen == LCD_SCREEN_FLASH_MSG ||
         sys_lcd.screen == LCD_SCREEN_WIFI_CLIENTS)) {
        s_wifi_settings_child_active = false;
        show_menu_screen(MENU_WIFI_SETTINGS, sys_state.menu_selection);
        return;
    }

    if (event_info->event == BUTTON_EVENT_CLICK &&
        (app_services_wifi_forget_confirmation_pending() ||
         app_services_wifi_disconnect_confirmation_pending())) {
        app_services_wifi_cancel_forget_saved();
        app_services_wifi_cancel_disconnect();
        show_menu_screen(MENU_WIFI_SETTINGS, sys_state.menu_selection);
        return;
    }

    if (event_info->event == BUTTON_EVENT_CLICK &&
        (sys_lcd.screen == LCD_SCREEN_WIFI_PASSWORD ||
         sys_lcd.screen == LCD_SCREEN_WIFI_NETWORK_DETAILS)) {
        LCD_LOCK();
        sys_lcd.screen = LCD_SCREEN_WIFI_SCAN;
        LCD_UNLOCK();
        return;
    }

    if (event_info->event == BUTTON_EVENT_CLICK &&
        sys_lcd.screen == LCD_SCREEN_WIFI_SCAN) {
        if (app_services_wifi_scan_is_active()) {
            (void)app_services_wifi_scan_cancel();
        }
        show_menu_screen(MENU_WIFI_CONFIG, 1U);
        return;
    }

    if (event_info->event == BUTTON_EVENT_CLICK &&
        (sys_lcd.screen == LCD_SCREEN_WIFI_STATUS ||
         sys_lcd.screen == LCD_SCREEN_WIFI_CONNECTING)) {
        show_menu_screen(MENU_WIFI_CONFIG, sys_state.menu_selection);
        return;
    }

    // View Settings screen -- Back exits straight to Main Menu
    if (sys_state.menu_state == MENU_SYSTEM_INFO)
    {
        if (event_info->event == BUTTON_EVENT_CLICK)
        {
            sys_state.menu_state = MAIN_MENU;
            sys_state.menu_selection = 0;
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            show_menu_screen(MAIN_MENU, sys_state.menu_selection);
        }
        return;
    }

    switch (event_info->event)
    {
    case BUTTON_EVENT_CLICK:
        /* P1 cancel factory reset */
        ESP_LOGI("BACK BUT", "rEACHED HERE FOR BACK");
        if (sys_state.menu_state == MENU_FACTORY_RESET)
        {
            ESP_LOGI("FACTORY_RESET", "We are at factory reset screen");
            factory_reset_phase_t phase = atomic_load(&sys_lcd.factory_reset.phase);

            if (phase == FACTORY_PHASE_PROGRESS)
            {
                return;
            }

            if (phase == FACTORY_PHASE_CONFIRM || phase == FACTORY_PHASE_IDLE)
            {
                exit_factory_reset_to_previous_menu();
                return;
            }
        }

        else if (atomic_load(&sys_lcd.factory_reset.action) == FACTORY_ACTION_NONE && sys_state.menu_state == MAIN_MENU)
        {
            sys_lcd.screen = LCD_SCREEN_MAIN;
        }

        /* Cancel only the current option. Back from an option returns to the
         * settings list, not to the home page, and the edit session is fully
         * forgotten before the list is redrawn. */
        if (sys_state.value_edit_mode)
        {
            menu_state_t parent_menu = sys_state.menu_state;
            int parent_selection = sys_state.menu_selection;
            exit_value_edit_mode(false);
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            show_menu_screen(parent_menu, parent_selection);
            lcd_flash_info_to("Edit Cancelled  ", "                ",
                              600, LCD_SCREEN_MENU);
            return;
        }
        /* P3 exit detail view */
        if (sys_state.in_detail_view)
        {
            sys_state.in_detail_view = false;
            sys_state.inverter.inverter_state = sys_state.pre_detail_inverter_state;
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            return;
        }
        /* P4 exit confirm */
        if (sys_state.in_confirmation_screen)
        {
            sys_state.in_confirmation_screen = false;
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            show_menu_screen(sys_state.menu_state, sys_state.menu_selection);
            return;
        }
        /* P5 exit info screen */
        if (sys_state.in_info_screen)
        {
            sys_state.in_info_screen = false;
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            go_to_main_screen();
            return;
        }

        /* P6 pop history */
        menu_state_t prev;
        int prev_sel;
        if (pop_menu_history(&prev, &prev_sel))
        {
            sys_state.menu_state = prev;
            sys_state.menu_selection = prev_sel;
            sys_state.last_activity_time = esp_timer_get_time() / 1000;
            if (prev == MENU_NONE)
                go_to_main_screen();
            else
                show_menu_screen(prev, prev_sel);
            return;
        }
        /* P7 fallback */
        switch (sys_state.menu_state)
        {
        case MENU_FACTORY_RESET:
        case MENU_SETTINGS:
        case MENU_MONITORING:
        case MENU_DIAGNOSTIC:
        case MENU_WIFI_CONFIG:
        case MENU_WIFI_SETTINGS:
            sys_state.inverter.inverter_state = sys_state.inverter.previous_inverter_state;
            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
            sys_state.value_edit_mode = false;
            go_to_main_screen();
            break;
        case MENU_OTA:
            /* Firmware update is a child of Main Menu. If history was lost by
             * an OTA status transition, recover to that parent rather than
             * treating Back as a power/home operation. */
            sys_state.menu_state = MAIN_MENU;
            sys_state.menu_selection = 4U;
            sys_state.value_edit_mode = false;
            show_menu_screen(MAIN_MENU, sys_state.menu_selection);
            break;
        case MAIN_MENU:
            sys_state.menu_state = MENU_NONE;
            sys_state.menu_selection = 0;
            clear_menu_history();
            go_to_main_screen();
            break;
        case MENU_NONE:
            break;
        default:
            sys_state.menu_state = MAIN_MENU;
            sys_state.menu_selection = 0;
            clear_menu_history();
            show_menu_screen(MAIN_MENU, 0);
            break;
        }
        sys_state.last_activity_time = esp_timer_get_time() / 1000;
        break;

    default:
        break;
    }
}
