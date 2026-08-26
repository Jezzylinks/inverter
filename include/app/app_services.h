#ifndef APP_SERVICES_H
#define APP_SERVICES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ota/ota_service.h"
#include "wifi/wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Application-facing network and OTA coordinator.
 *
 * The lower-level Wi-Fi and OTA services retain ownership of transport,
 * credentials, TLS, and image validation. This layer owns user intent,
 * persistent configuration, and menu-safe state transitions.
 */
#define APP_OTA_MANIFEST_URL_MAX OTA_MAX_URL_LENGTH

typedef enum {
    APP_OTA_IDLE = 0,
    APP_OTA_CHECKING,
    APP_OTA_AVAILABLE,
    APP_OTA_CONFIRMING,
    APP_OTA_PREPARING,
    APP_OTA_DOWNLOADING,
    APP_OTA_VERIFYING,
    APP_OTA_COMPLETE,
    APP_OTA_ERROR,
    APP_OTA_CANCELLED,
} app_ota_state_t;

typedef struct {
    app_ota_state_t state;
    char installed_version[OTA_MAX_VERSION_LENGTH];
    char available_version[OTA_MAX_VERSION_LENGTH];
    char error_detail[OTA_MAX_VERSION_LENGTH];
    int progress_percent;
    bool auto_check_enabled;
    bool update_available;
    bool confirmation_pending;
    bool cancel_confirmation_pending;
} app_ota_status_t;

/** Initialize Wi-Fi and OTA application coordination after system settings load. */
esp_err_t app_services_init(void);

/** Queue user Wi-Fi intent for asynchronous controller start/stop and persistence. */
esp_err_t app_services_set_wifi_enabled(bool enabled);
bool app_services_wifi_enabled(void);

/** Menu-safe Wi-Fi operations. */
esp_err_t app_services_wifi_scan(void);
esp_err_t app_services_wifi_scan_cancel(void);
bool app_services_wifi_scan_is_active(void);
void app_services_show_wifi_network_details(uint8_t selected_index);
esp_err_t app_services_wifi_connect_selected(uint8_t selected_index);
esp_err_t app_services_wifi_submit_password(void);
esp_err_t app_services_wifi_connect_network(const char *ssid,
                                             const char *password);
esp_err_t app_services_wifi_connect_network_with_rssi(const char *ssid,
                                                      const char *password,
                                                      int8_t rssi);
esp_err_t app_services_wifi_connect_saved(void);
esp_err_t app_services_wifi_reconnect(void);
esp_err_t app_services_wifi_disconnect(void);
esp_err_t app_services_wifi_start_provisioning(void);
void app_services_show_wifi_status(void);
const char *app_services_wifi_saved_network_label(void);
bool app_services_wifi_forget_confirmation_pending(void);
esp_err_t app_services_wifi_request_forget_saved(void);
esp_err_t app_services_wifi_confirm_forget_saved(void);
void app_services_wifi_cancel_forget_saved(void);
bool app_services_wifi_disconnect_confirmation_pending(void);
esp_err_t app_services_wifi_request_disconnect(void);
esp_err_t app_services_wifi_confirm_disconnect(void);
void app_services_wifi_cancel_disconnect(void);
bool app_services_wifi_dhcp_enabled(void);
esp_err_t app_services_wifi_toggle_dhcp(void);
const char *app_services_wifi_mode_name(void);
const char *app_services_wifi_connect_action_label(void);
const char *app_services_wifi_secondary_action_label(void);
bool app_services_wifi_can_manage_clients(void);
bool app_services_wifi_is_ap_only(void);
void app_services_show_ap_clients(void);
esp_err_t app_services_get_ap_clients(wifi_ap_client_info_t clients[],
                                      size_t capacity,
                                      size_t *count);
esp_err_t app_services_disconnect_ap_client(const uint8_t mac[6]);
esp_err_t app_services_disconnect_ap_client_at(uint8_t index);

/**
 * Persist the HTTPS CSV manifest URL used for update availability checks.
 * Passing an empty string disables automatic checks but keeps manual menu
 * feedback deterministic.
 */
esp_err_t app_services_set_ota_manifest_url(const char *url);
esp_err_t app_services_get_ota_manifest_url(char *buffer, size_t buffer_len);

/** Start an asynchronous check of the configured CSV manifest. */
esp_err_t app_services_check_for_update(bool user_initiated);

/** Move an available update into a user-confirmation prompt. */
esp_err_t app_services_request_update_confirmation(void);

/** Start the CSV-based update only after the explicit confirmation action. */
esp_err_t app_services_confirm_update(void);

/** Request, confirm, or cancel an OTA cancellation prompt. */
esp_err_t app_services_request_cancel_update(void);
esp_err_t app_services_confirm_cancel_update(void);
void app_services_cancel_cancel_update(void);
bool app_services_ota_cancel_confirmation_pending(void);

/** Cancel an in-progress OTA transaction or install confirmation. */
esp_err_t app_services_cancel_update(void);

/** Copy menu-visible update state. */
void app_services_get_ota_status(app_ota_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* APP_SERVICES_H */
