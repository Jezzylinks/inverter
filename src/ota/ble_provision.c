/**
 * @file ble_provision.c
 * @brief Bluetooth LE Provisioning (NimBLE-based)
 * Alternative to AP mode for WiFi setup
 */

#include "ble_provision.h"
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "wifi/wifi_config.h"
#include "wifi/wifi_storage.h"

static const char *TAG = "BLE_PROVISION";

#define BLE_PROV_SVC_UUID 0x180A
#define BLE_PROV_CHAR_SSID 0x2A3D
#define BLE_PROV_CHAR_PASS 0x2A3E
#define BLE_PROV_CHAR_STATUS 0x2A3F
#define BLE_PROV_CHAR_CMD 0x2A40

#define BLE_PROV_DEVICE_NAME "INVERTER-SETUP"
#define BLE_PROV_APPEARANCE 0x0000

static ble_provision_complete_callback_t s_complete_cb = NULL;
static bool s_advertising = false;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_status_attr_handle = 0;

static char s_ssid[33] = {0};
static char s_password[65] = {0};
static bool s_credentials_received = false;

/*----------------------------------------------------------
 * GATT Service Definition
 *---------------------------------------------------------*/
static int ble_prov_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatt_svr_svcs[] __attribute__((unused)) = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLE_PROV_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(BLE_PROV_CHAR_SSID),
                .access_cb = ble_prov_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(BLE_PROV_CHAR_PASS),
                .access_cb = ble_prov_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = BLE_UUID16_DECLARE(BLE_PROV_CHAR_STATUS),
                .access_cb = ble_prov_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_attr_handle,
            },
            {
                .uuid = BLE_UUID16_DECLARE(BLE_PROV_CHAR_CMD),
                .access_cb = ble_prov_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {0}},
    },
    {0}};

/*----------------------------------------------------------
 * GATT Access Handler
 *---------------------------------------------------------*/
static int ble_prov_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t uuid16 = ble_uuid_u16(ctxt->chr->uuid);

    switch (uuid16)
    {
    case BLE_PROV_CHAR_SSID:
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
        {
            uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
            if (om_len > sizeof(s_ssid) - 1)
            {
                om_len = sizeof(s_ssid) - 1;
            }
            ble_hs_mbuf_to_flat(ctxt->om, s_ssid, om_len, NULL);
            s_ssid[om_len] = '\0';
            ESP_LOGI(TAG, "SSID received via BLE: %s", s_ssid);
        }
        else if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        {
            int rc = os_mbuf_append(ctxt->om, s_ssid, strlen(s_ssid));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        break;

    case BLE_PROV_CHAR_PASS:
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
        {
            uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
            if (om_len > sizeof(s_password) - 1)
            {
                om_len = sizeof(s_password) - 1;
            }
            ble_hs_mbuf_to_flat(ctxt->om, s_password, om_len, NULL);
            s_password[om_len] = '\0';
            ESP_LOGI(TAG, "Password received via BLE (len=%d)", om_len);
        }
        break;

    case BLE_PROV_CHAR_STATUS:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        {
            const char *status = s_credentials_received ? "ready" : "waiting";
            int rc = os_mbuf_append(ctxt->om, status, strlen(status));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        break;

    case BLE_PROV_CHAR_CMD:
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
        {
            char cmd[32] = {0};
            uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
            if (om_len > sizeof(cmd) - 1)
            {
                om_len = sizeof(cmd) - 1;
            }
            ble_hs_mbuf_to_flat(ctxt->om, cmd, om_len, NULL);
            cmd[om_len] = '\0';

            if (strcmp(cmd, "connect") == 0)
            {
                if (s_ssid[0] != '\0')
                {
                    s_credentials_received = true;
                    ESP_LOGI(TAG, "Connect command received, saving credentials");

                    /* Save credentials */
                    wifi_credentials_t creds;
                    memset(&creds, 0, sizeof(creds));
                    strncpy(creds.ssid, s_ssid, sizeof(creds.ssid) - 1);
                    strncpy(creds.password, s_password, sizeof(creds.password) - 1);

                    esp_err_t err = wifi_storage_save_credentials(&creds);
                    if (err == ESP_OK)
                    {
                        /* Notify status change */
                        if (s_status_attr_handle != 0)
                        {
                            const char *status = "saved";
                            struct os_mbuf *om = ble_hs_mbuf_from_flat(status, strlen(status));
                            ble_gatts_notify_custom(conn_handle, s_status_attr_handle, om);
                        }

                        /* Stop BLE and trigger callback */
                        ble_provision_stop();

                        if (s_complete_cb)
                        {
                            s_complete_cb();
                        }
                    }
                }
            }
            else if (strcmp(cmd, "reset") == 0)
            {
                memset(s_ssid, 0, sizeof(s_ssid));
                memset(s_password, 0, sizeof(s_password));
                s_credentials_received = false;
                ESP_LOGI(TAG, "Credentials reset via BLE");
            }
        }
        break;
    }

    return 0;
}

/*----------------------------------------------------------
 * BLE Event Callbacks
 *---------------------------------------------------------*/
static int __attribute__((unused)) ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0)
        {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE client connected");
        }
        else
        {
            ESP_LOGE(TAG, "BLE connection failed: %d", event->connect.status);
            ble_provision_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE client disconnected");
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_provision_start_advertising();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "BLE advertising complete");
        if (s_advertising)
        {
            ble_provision_start_advertising();
        }
        break;

    default:
        break;
    }

    return 0;
}

/*----------------------------------------------------------
 * Start Advertising
 *---------------------------------------------------------*/
esp_err_t ble_provision_start_advertising(void)
{
#if !WIFI_RUNTIME_PROVISIONING_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)BLE_PROV_DEVICE_NAME;
    fields.name_len = strlen(BLE_PROV_DEVICE_NAME);
    fields.name_is_complete = 1;
    fields.appearance = BLE_PROV_APPEARANCE;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to set adv fields: %d", rc);
        return ESP_FAIL;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 0x20;
    adv_params.itvl_max = 0x40;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to start advertising: %d", rc);
        return ESP_FAIL;
    }

    s_advertising = true;
    ESP_LOGI(TAG, "BLE advertising started");

    return ESP_OK;
#endif
}

/*----------------------------------------------------------
 * NimBLE Host Task
 *---------------------------------------------------------*/
static void __attribute__((unused)) ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/*----------------------------------------------------------
 * Initialize
 *---------------------------------------------------------*/
esp_err_t ble_provision_init(void)
{
#if !WIFI_RUNTIME_PROVISIONING_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = nimble_port_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NimBLE port init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Initialize NimBLE configuration */
    ble_hs_cfg.sync_cb = NULL;
    ble_hs_cfg.reset_cb = NULL;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Set device name */
    ble_svc_gap_device_name_set(BLE_PROV_DEVICE_NAME);
    ble_svc_gap_device_appearance_set(BLE_PROV_APPEARANCE);

    /* Add GATT services */
    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "GATT count cfg failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "GATT add svcs failed: %d", rc);
        return ESP_FAIL;
    }

    /* Start NimBLE host task */
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE provisioning initialized");

    return ESP_OK;
#endif
}

/*----------------------------------------------------------
 * Start
 *---------------------------------------------------------*/
esp_err_t ble_provision_start(ble_provision_complete_callback_t callback)
{
#if !WIFI_RUNTIME_PROVISIONING_ENABLED
    (void)callback;
    return ESP_ERR_NOT_SUPPORTED;
#else
    s_complete_cb = callback;
    s_credentials_received = false;
    memset(s_ssid, 0, sizeof(s_ssid));
    memset(s_password, 0, sizeof(s_password));

    return ble_provision_start_advertising();
#endif
}

/*----------------------------------------------------------
 * Stop
 *---------------------------------------------------------*/
esp_err_t ble_provision_stop(void)
{
#if !WIFI_RUNTIME_PROVISIONING_ENABLED
    return ESP_OK;
#else
    s_advertising = false;

    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE)
    {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    ble_gap_adv_stop();

    ESP_LOGI(TAG, "BLE provisioning stopped");

    return ESP_OK;
#endif
}

/*----------------------------------------------------------
 * Deinitialize
 *---------------------------------------------------------*/
esp_err_t ble_provision_deinit(void)
{
#if !WIFI_RUNTIME_PROVISIONING_ENABLED
    return ESP_OK;
#else
    ble_provision_stop();
    nimble_port_deinit();

        return ESP_OK;
#endif
}
