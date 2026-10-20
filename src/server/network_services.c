#include "server/network_services.h"
#include "system/core_affinity.h"

#include <string.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "server/json/json_api_server.h"
#include "server/mdns/mdns_service.h"
#include "server/ntp/ntp_client.h"
#include "server/mqtt/mqtt_client_manager.h"
#include "server/websocket/websocket_server.h"
#include "server/web/web_dashboard_server.h"
#include "utility/quiet_hours.h"
#include "wifi/wifi_config.h"
#include "wifi/wifi_events.h"
#include "wifi/wifi_controller.h"
#include "wifi/wifi_manager.h"
#include "wifi/wifi_dns_server.h"

static void log_ota_heap(const char *stage);

#define NETWORK_SERVICES_TAG "NET_SERVICES"
#define NETWORK_HTTP_PORT 80U
#define NETWORK_HTTP_STACK_SIZE 8192U
#define NETWORK_HTTP_MIN_LARGEST_BLOCK (NETWORK_HTTP_STACK_SIZE + 4096U)
#define NETWORK_HTTP_MIN_FREE_HEAP 16384U
#define NETWORK_SYNC_TASK_STACK_SIZE 8192U
#define NETWORK_SYNC_RETRY_DELAY_MS 1000U
/* JSON API (64 handlers incl. OPTIONS) + dashboard (3) + WebSocket (1) = 68.
 * Each spare slot costs one pointer, so keep generous headroom: when this was
 * 64 the last handlers failed with ESP_ERR_HTTPD_HANDLERS_FULL, the WebSocket
 * registration failed, start was torn down and retried on every Wi-Fi status
 * change, leaking heap until allocations failed. */
#define NETWORK_HTTP_MAX_URI_HANDLERS 96U
static esp_err_t network_services_start_station_services(void);
static esp_err_t network_services_stop_station_services(void);

static SemaphoreHandle_t s_mutex;
/* Serialises network_services_start() / network_services_stop() end to end.
 * Without it two sync tasks could both pass the "not running" check and call
 * httpd_start() at once; the loser failed with listen() errno 112 (address in
 * use) and the services ended up half started. */
static SemaphoreHandle_t s_lifecycle_mutex;
static httpd_handle_t s_http_server;
static network_mqtt_config_t s_mqtt_config;
static bool s_initialized;
static bool s_running;
static bool s_mdns_running;
static bool s_captive_dns_running;
static bool s_ntp_running;
static bool s_websocket_running;
static bool s_dashboard_running;
static bool s_station_ready;
static bool s_internet_ready;
static bool s_station_services_running;
static bool s_suspended_for_ota;
static bool s_sync_scheduled;
static bool s_sync_retry_pending;
static TickType_t s_sync_retry_after_tick;
/* Detect status updates that arrive while the sync worker is already running. */
static uint32_t s_status_generation;
/* Set atomically by network_services_begin_teardown() before any Wi-Fi
 * disable sequence.  Prevents network_services_sync_task from calling
 * network_services_start() after teardown has been initiated, which would
 * race esp_wifi_stop() destroying the lwIP TCP/IP mailbox and cause:
 *   assert failed: tcpip_callback ... (Invalid mbox)
 * Cleared only by network_services_init() on re-initialisation. */
static volatile bool s_teardown_pending;

static void network_services_sync_task(void *arg);
static bool network_services_spawn_sync_task(void);
static void network_services_mark_sync_retry(void);

static void services_lock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void services_unlock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

static void log_service_heap(const char *phase, const char *service)
{
    ESP_LOGI(NETWORK_SERVICES_TAG,
             "HEAP[%s:%s] free=%u largest=%u min_ever=%u",
             phase, service,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

static void network_services_mark_sync_retry(void)
{
    services_lock();
    if (!s_teardown_pending) {
        s_sync_retry_pending = true;
        s_sync_retry_after_tick = xTaskGetTickCount() +
                                  pdMS_TO_TICKS(NETWORK_SYNC_RETRY_DELAY_MS);
    }
    services_unlock();
}

static bool network_services_spawn_sync_task(void)
{
    /* Clear old retry state before the worker can preempt this caller. The
     * worker may immediately fail its heap gate and set a fresh retry. */
    services_lock();
    s_sync_retry_pending = false;
    services_unlock();

    if (xTaskCreatePinnedToCore(network_services_sync_task,
                                "net_services_sync",
                                NETWORK_SYNC_TASK_STACK_SIZE, NULL, 4U,
                                NULL, APP_CORE_SYSTEM) == pdPASS) {
        return true;
    }

    services_lock();
    s_sync_scheduled = false;
    if (!s_teardown_pending) {
        s_sync_retry_pending = true;
        s_sync_retry_after_tick = xTaskGetTickCount() +
                                  pdMS_TO_TICKS(NETWORK_SYNC_RETRY_DELAY_MS);
    }
    services_unlock();
    log_service_heap("TASK_CREATE_FAILED", "net_services_sync");
    ESP_LOGW(NETWORK_SERVICES_TAG,
             "Unable to schedule network-service sync; retry will be attempted");
    return false;
}

static void mqtt_status_callback(mqtt_status_t status)
{
    ESP_LOGI(NETWORK_SERVICES_TAG, "MQTT status: %d", (int)status);
    if (status == MQTT_STATUS_CONNECTED) {
        network_mqtt_config_t config;
        if (network_services_get_mqtt_config(&config) == ESP_OK &&
            config.subscribe_topic[0] != '\0') {
            (void)mqtt_client_subscribe(config.subscribe_topic, config.qos);
        }
    }
}

static void network_services_sync_task(void *arg)
{
    (void)arg;

    /* Synchronize local/AP services and station/internet services separately.
     * AP readiness is sufficient for the local HTTP/WebSocket/mDNS layer, but
     * it must never be treated as station/internet readiness for NTP or MQTT. */
    if (s_teardown_pending) {
        services_lock();
        s_sync_scheduled = false;
        s_sync_retry_pending = false;
        services_unlock();
        vTaskDelete(NULL);
        return;
    }

    services_lock();
    const uint32_t status_generation = s_status_generation;
    const bool station_ready = s_station_ready;
    const bool internet_ready = s_internet_ready;
    const bool local_running = s_running;
    const bool station_services_running = s_station_services_running;
    services_unlock();

    const wifi_mode_t mode = wifi_manager_get_mode();
    const bool station_capable = mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA;
    const bool ap_capable = mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA;
    const bool ap_ready = ap_capable && wifi_events_is_ap_active();
    const bool local_ready = station_ready || ap_ready;

    if (local_ready && !local_running) {
        const esp_err_t start_err = network_services_start();
        if (start_err != ESP_OK && start_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(NETWORK_SERVICES_TAG,
                     "Local service start deferred (%s); scheduling retry",
                     esp_err_to_name(start_err));
            network_services_mark_sync_retry();
        }
    } else if (!local_ready && local_running) {
        (void)network_services_stop();
    }

    /* AP/APSTA owns the captive-DNS side of the local portal.  The
     * normal HTTP/API/dashboard server remains shared with the local service
     * layer, so we start only the DNS redirector here rather than creating a
     * second HTTP server on port 80. */
    services_lock();
    const bool captive_dns_running = s_captive_dns_running;
    services_unlock();
    if (ap_ready && !captive_dns_running) {
        log_service_heap("START_BEFORE", "captive_dns");
        const esp_err_t dns_err = wifi_dns_server_start();
        log_service_heap("START_AFTER", "captive_dns");
        if (dns_err == ESP_OK) {
            services_lock();
            s_captive_dns_running = true;
            services_unlock();
            ESP_LOGI(NETWORK_SERVICES_TAG, "Captive portal DNS started for AP");
        } else {
            ESP_LOGW(NETWORK_SERVICES_TAG,
                     "Captive portal DNS start failed: %s; scheduling retry",
                     esp_err_to_name(dns_err));
            network_services_mark_sync_retry();
        }
    } else if (!ap_ready && captive_dns_running) {
        log_service_heap("STOP_BEFORE", "captive_dns");
        (void)wifi_dns_server_stop();
        log_service_heap("STOP_AFTER", "captive_dns");
        services_lock();
        s_captive_dns_running = false;
        services_unlock();
        ESP_LOGI(NETWORK_SERVICES_TAG, "Captive portal DNS stopped");
    }

    /* NTP/MQTT are upstream services, not merely station-IP services.
     * Keep them stopped until the monitor confirms outbound connectivity;
     * losing internet must not stop the local AP/web control path above. */
    if (station_capable && station_ready && internet_ready &&
        !station_services_running) {
        (void)network_services_start_station_services();
    } else if ((!station_capable || !station_ready || !internet_ready) &&
               station_services_running) {
        (void)network_services_stop_station_services();
    }

    bool rerun = false;
    services_lock();
    if (s_status_generation != status_generation && !s_teardown_pending) {
        /* A Wi-Fi/internet event arrived during this pass. Keep the admission
         * flag set and run another pass so that the newest state is applied. */
        rerun = true;
    } else {
        s_sync_scheduled = false;
    }
    services_unlock();

    if (rerun) {
        (void)network_services_spawn_sync_task();
    }
    vTaskDelete(NULL);
}

static void network_wifi_status_callback(const wifi_status_t *status)
{
    if (status == NULL || !s_initialized || s_mutex == NULL) {
        return;
    }
    const wifi_mode_t mode = wifi_manager_get_mode();
    /* In APSTA, the public state enum can represent AP_ACTIVE while the STA
     * is connected (or CONNECTED while the AP is also active). Use the
     * independent interface flags rather than treating the enum as exclusive. */
    const bool station_ready = status->connected && status->got_ip;
    const bool provisioning = wifi_controller_get_state() == WIFI_CONTROLLER_PROVISIONING;
    const bool ap_ready = !provisioning &&
                          (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) &&
                          wifi_events_is_ap_active();
    const bool local_ready = !provisioning && (station_ready || ap_ready);
    bool mdns_running = false;
    services_lock();
    mdns_running = s_mdns_running;
    services_unlock();
    if (mdns_running) {
        const char *state = local_ready ? "connected" : "offline";
        (void)mdns_service_update_status(state, status->rssi);
    }
    bool schedule = false;
    services_lock();
    s_station_ready = !provisioning && station_ready;
    s_internet_ready = !provisioning && station_ready &&
                       status->internet_available;
    ++s_status_generation;
    if (!s_sync_scheduled) {
        s_sync_scheduled = true;
        schedule = true;
    }
    services_unlock();
    if (schedule) {
        (void)network_services_spawn_sync_task();
    }
}

static void mqtt_message_callback(const mqtt_message_t *message)
{
    if (message == NULL) {
        return;
    }
    ESP_LOGI(NETWORK_SERVICES_TAG, "MQTT message received: topic_len=%d data_len=%d",
             message->topic_len, message->data_len);
}

static void cleanup_http_services(void)
{
    log_service_heap("STOP_BEFORE", "dashboard");
    (void)web_dashboard_server_unregister(s_http_server);
    s_dashboard_running = false;
    (void)web_dashboard_server_deinit();
    log_service_heap("STOP_AFTER", "dashboard");

    log_service_heap("STOP_BEFORE", "websocket");
    (void)websocket_server_unregister(s_http_server);
    s_websocket_running = false;
    (void)websocket_server_deinit();
    log_service_heap("STOP_AFTER", "websocket");

    log_service_heap("STOP_BEFORE", "json_api");
    (void)json_api_server_stop();
    log_service_heap("STOP_AFTER", "json_api");

    if (s_http_server != NULL) {
        log_service_heap("STOP_BEFORE", "httpd");
        const esp_err_t stop_err = httpd_stop(s_http_server);
        if (stop_err == ESP_OK) {
            s_http_server = NULL;
        } else {
            ESP_LOGW(NETWORK_SERVICES_TAG,
                     "HTTP server stop failed (%s); retaining handle for retry",
                     esp_err_to_name(stop_err));
        }
        log_service_heap("STOP_AFTER", "httpd");
    }
}

static esp_err_t start_mqtt_if_enabled(void)
{
    network_mqtt_config_t config;
    services_lock();
    config = s_mqtt_config;
    services_unlock();
    if (!config.enabled || config.broker_url[0] == '\0') {
        return ESP_OK;
    }

    mqtt_config_t mqtt = {
        .broker_url = config.broker_url,
        .client_id = config.client_id,
        .username = config.username[0] != '\0' ? config.username : NULL,
        .password = config.password[0] != '\0' ? config.password : NULL,
        .keepalive_sec = config.keepalive_sec,
    };
    esp_err_t err = mqtt_client_init(&mqtt);
    if (err == ESP_OK) {
        (void)mqtt_client_register_status_callback(mqtt_status_callback);
        (void)mqtt_client_register_message_callback(mqtt_message_callback);
        err = mqtt_client_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(NETWORK_SERVICES_TAG, "MQTT startup failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t network_services_start_station_services(void)
{
    if (!s_initialized || s_teardown_pending) {
        return ESP_ERR_INVALID_STATE;
    }

    services_lock();
    if (!s_internet_ready) {
        services_unlock();
        ESP_LOGI(NETWORK_SERVICES_TAG,
                 "Station services deferred: internet is not available");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_station_services_running) {
        services_unlock();
        return ESP_OK;
    }
    services_unlock();

    esp_err_t first_err = ESP_OK;
    log_service_heap("START_BEFORE", "sntp");
    const esp_err_t ntp_err = ntp_client_init(NULL);
    log_service_heap("START_AFTER", "sntp");
    if (ntp_err == ESP_OK) {
        services_lock();
        s_ntp_running = true;
        services_unlock();
        quiet_hours_sntp_init();
    } else {
        ESP_LOGW(NETWORK_SERVICES_TAG,
                 "NTP startup failed; station services continue: %s",
                 esp_err_to_name(ntp_err));
        first_err = ntp_err;
    }

    log_service_heap("START_BEFORE", "mqtt");
    const esp_err_t mqtt_err = start_mqtt_if_enabled();
    log_service_heap("START_AFTER", "mqtt");
    if (mqtt_err != ESP_OK && first_err == ESP_OK) {
        first_err = mqtt_err;
    }

    services_lock();
    s_station_services_running = true;
    services_unlock();
    return first_err;
}

static esp_err_t network_services_stop_station_services(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    services_lock();
    const bool running = s_station_services_running;
    const bool ntp_running = s_ntp_running;
    s_station_services_running = false;
    s_ntp_running = false;
    services_unlock();

    if (!running) {
        return ESP_OK;
    }

    esp_err_t first_err = ESP_OK;
    log_service_heap("STOP_BEFORE", "mqtt");
    (void)mqtt_client_disconnect();
    (void)mqtt_client_deinit();
    log_service_heap("STOP_AFTER", "mqtt");
    if (ntp_running) {
        log_service_heap("STOP_BEFORE", "sntp");
        const esp_err_t err = ntp_client_deinit();
        log_service_heap("STOP_AFTER", "sntp");
        if (err != ESP_OK) {
            first_err = err;
        }
    }
    return first_err;
}

esp_err_t network_services_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_lifecycle_mutex == NULL) {
        s_lifecycle_mutex = xSemaphoreCreateMutex();
    }
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = mqtt_config_load(&s_mqtt_config);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }
    s_http_server = NULL;
    s_running = false;
    s_mdns_running = false;
    s_captive_dns_running = false;
    s_ntp_running = false;
    s_websocket_running = false;
    s_dashboard_running = false;
    s_station_ready = false;
    s_internet_ready = false;
    s_station_services_running = false;
    s_sync_scheduled = false;
    s_sync_retry_pending = false;
    s_sync_retry_after_tick = 0U;
    s_status_generation = 0U;
    s_teardown_pending = false;
    const esp_err_t dns_init_err = wifi_dns_server_init();
    if (dns_init_err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return dns_init_err;
    }
    const esp_err_t callback_err = wifi_events_register_status_callback(network_wifi_status_callback);
    if (callback_err != ESP_OK) {
        (void)wifi_dns_server_deinit();
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return callback_err;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t network_services_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    (void)network_services_stop();
    (void)wifi_dns_server_stop();
    (void)wifi_dns_server_deinit();
    (void)wifi_events_unregister_status_callback(network_wifi_status_callback);
    services_lock();
    s_initialized = false;
    services_unlock();
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    return ESP_OK;
}

static esp_err_t network_services_start_locked(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Refuse to start if a Wi-Fi disable sequence has set s_teardown_pending.
     * At this point esp_wifi_stop() may already be running or may have already
     * destroyed the lwIP TCP/IP mailbox; calling httpd_start() or any other
     * network API would invoke tcpip_callback() on an invalid mbox and assert. */
    if (s_teardown_pending) {
        ESP_LOGW(NETWORK_SERVICES_TAG,
                 "network_services_start() suppressed: teardown in progress");
        return ESP_ERR_INVALID_STATE;
    }
    services_lock();
    if (s_running) {
        services_unlock();
        return ESP_OK;
    }
    services_unlock();

    /* If a previous httpd_stop() failed, keep and retry the handle instead
     * of losing it and leaving port 80 occupied forever. */
    if (s_http_server != NULL) {
        log_service_heap("RECOVER_BEFORE", "httpd");
        const esp_err_t stale_stop_err = httpd_stop(s_http_server);
        log_service_heap("RECOVER_AFTER", "httpd");
        if (stale_stop_err != ESP_OK) {
            ESP_LOGW(NETWORK_SERVICES_TAG,
                     "Stale HTTP server still owns port %u (%s); retrying later",
                     (unsigned)NETWORK_HTTP_PORT,
                     esp_err_to_name(stale_stop_err));
            return stale_stop_err;
        }
        s_http_server = NULL;
    }

    const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    log_service_heap("START_BEFORE", "httpd");
    if (free_heap < NETWORK_HTTP_MIN_FREE_HEAP ||
        largest_block < NETWORK_HTTP_MIN_LARGEST_BLOCK) {
        ESP_LOGW(NETWORK_SERVICES_TAG,
                 "HTTP server start deferred: free=%u largest=%u; need free>=%u largest>=%u",
                 (unsigned)free_heap, (unsigned)largest_block,
                 (unsigned)NETWORK_HTTP_MIN_FREE_HEAP,
                 (unsigned)NETWORK_HTTP_MIN_LARGEST_BLOCK);
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = NETWORK_HTTP_PORT;
    http_config.max_uri_handlers = NETWORK_HTTP_MAX_URI_HANDLERS;
    http_config.stack_size = NETWORK_HTTP_STACK_SIZE;
    http_config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = ESP_FAIL;
    /* The listening socket of a just-stopped server can take a moment to be
     * released by lwIP (listen() errno 112). Retry briefly before giving up. */
    for (unsigned attempt = 0U; attempt < 4U; ++attempt) {
        err = httpd_start(&server, &http_config);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(NETWORK_SERVICES_TAG, "httpd_start attempt %u failed: %s",
                 attempt + 1U, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(250));
        if (s_teardown_pending) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    log_service_heap("START_AFTER", "httpd");
    if (err != ESP_OK) {
        return err;
    }
    s_http_server = server;

    log_service_heap("START_BEFORE", "dashboard");
    err = web_dashboard_server_init();
    if (err == ESP_OK) {
        err = web_dashboard_server_register(server);
        if (err == ESP_OK) {
            s_dashboard_running = true;
        } else {
            (void)web_dashboard_server_deinit();
        }
    }
    log_service_heap("START_AFTER", "dashboard");
    if (err != ESP_OK) {
        ESP_LOGW(NETWORK_SERVICES_TAG, "Dashboard unavailable: %s", esp_err_to_name(err));
    }

    log_service_heap("START_BEFORE", "json_api");
    err = json_api_server_start(server);
    log_service_heap("START_AFTER", "json_api");
    if (err != ESP_OK) {
        cleanup_http_services();
        return err;
    }
    log_service_heap("START_BEFORE", "websocket");
    err = websocket_server_init();
    if (err == ESP_OK) {
        err = websocket_server_register(server);
    }
    log_service_heap("START_AFTER", "websocket");
    if (err != ESP_OK) {
        cleanup_http_services();
        return err;
    }
    s_websocket_running = true;

    log_service_heap("START_BEFORE", "mdns");
    err = mdns_service_init(WIFI_HOSTNAME);
    log_service_heap("START_AFTER", "mdns");
    if (err == ESP_OK) {
        s_mdns_running = true;
    } else {
        ESP_LOGW(NETWORK_SERVICES_TAG, "mDNS startup failed: %s", esp_err_to_name(err));
    }

    /* Local services are architecture-neutral: they can run on STA, AP, or
     * APSTA once a usable local interface exists. The sync worker manages
     * NTP/MQTT separately after the monitor confirms upstream connectivity. */
    services_lock();
    s_running = true;
    services_unlock();

    ESP_LOGI(NETWORK_SERVICES_TAG, "Local network services started for %s mode",
             wifi_manager_get_mode() == WIFI_MODE_STA ? "STA" :
             wifi_manager_get_mode() == WIFI_MODE_AP ? "AP" : "APSTA");
    log_ota_heap("after start");
    return ESP_OK;
}

esp_err_t network_services_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lifecycle_mutex != NULL &&
        xSemaphoreTake(s_lifecycle_mutex, pdMS_TO_TICKS(20000)) != pdTRUE) {
        ESP_LOGW(NETWORK_SERVICES_TAG, "network_services_start(): lifecycle busy");
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = network_services_start_locked();
    if (s_lifecycle_mutex != NULL) {
        xSemaphoreGive(s_lifecycle_mutex);
    }
    return err;
}

void network_services_allow_start(void)
{
    /* Clear the teardown flag so network_services_start() can run again
     * after a previous Wi-Fi disable cycle set s_teardown_pending.
     * Called from execute_wifi_toggle() on the enable path, before
     * wifi_controller_start(), so the flag is clear by the time Wi-Fi
     * connects and the sync task fires network_services_start(). */
    s_teardown_pending = false;
}

static esp_err_t network_services_stop_locked(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    /* Signal teardown intent before touching any service state.
     * network_services_sync_task and network_services_start() check this
     * flag and refuse to start services while it is set, preventing the
     * race where a Wi-Fi-event-driven sync task calls network_services_start()
     * → httpd_start() → tcpip_callback() after esp_wifi_stop() has already
     * destroyed the lwIP TCP/IP mailbox, causing the 'Invalid mbox' assert. */
    s_teardown_pending = true;
    services_lock();
    const bool was_running = s_running;
    const bool mdns_running = s_mdns_running;
    s_running = false;
    s_mdns_running = false;
    s_websocket_running = false;
    s_sync_retry_pending = false;
    services_unlock();

    (void)network_services_stop_station_services();

    services_lock();
    const bool captive_dns_running = s_captive_dns_running;
    s_captive_dns_running = false;
    services_unlock();
    if (captive_dns_running) {
        (void)wifi_dns_server_stop();
    }

    if (!was_running) {
        log_service_heap("STOP_AFTER", "local_services");
        return ESP_OK;
    }

    log_service_heap("STOP_BEFORE", "mdns");
    if (mdns_running) {
        (void)mdns_service_deinit();
    }
    log_service_heap("STOP_AFTER", "mdns");
    cleanup_http_services();
    log_service_heap("STOP_AFTER", "local_services");
    ESP_LOGI(NETWORK_SERVICES_TAG, "Local network services stopped");
    return ESP_OK;
}

esp_err_t network_services_stop(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    /* Raise the teardown flag before waiting, so a start that is queued behind
     * us (or runs right after) refuses instead of racing the teardown. */
    s_teardown_pending = true;
    if (s_lifecycle_mutex != NULL &&
        xSemaphoreTake(s_lifecycle_mutex, pdMS_TO_TICKS(20000)) != pdTRUE) {
        ESP_LOGW(NETWORK_SERVICES_TAG, "network_services_stop(): lifecycle busy");
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = network_services_stop_locked();
    if (s_lifecycle_mutex != NULL) {
        xSemaphoreGive(s_lifecycle_mutex);
    }
    return err;
}

/* Heap trace around the OTA suspend/resume cycle. Free heap after a stop was
 * ~12 KB lower on the second cycle than the first, so compare these lines
 * between cycles: if the "after stop" numbers do not return to the same level,
 * a service leaks across stop/start. */
static void log_ota_heap(const char *stage)
{
    ESP_LOGI(NETWORK_SERVICES_TAG,
             "HEAP[%s] free=%u largest=%u min_ever=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

esp_err_t network_services_suspend_for_ota(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    ESP_LOGI(NETWORK_SERVICES_TAG,
             "Suspending local network services for firmware download");
    log_ota_heap("before stop");
    const esp_err_t err = network_services_stop();
    s_suspended_for_ota = true;
    log_ota_heap("after stop");
    return err;
}

void network_services_resume_after_ota(void)
{
    if (!s_initialized || !s_suspended_for_ota) {
        return;
    }
    s_teardown_pending = false;
    ESP_LOGI(NETWORK_SERVICES_TAG,
             "Resuming local network services after firmware download");
    log_ota_heap("before resume");
    bool schedule = false;
    services_lock();
    ++s_status_generation;
    if (!s_sync_scheduled) {
        s_sync_scheduled = true;
        schedule = true;
    }
    services_unlock();

    if (schedule) {
        /* Clear suspension before task creation so a worker may reconcile
         * immediately. Restore it if allocation fails; the main loop retries. */
        s_suspended_for_ota = false;
        if (!network_services_spawn_sync_task()) {
            s_suspended_for_ota = true;
            ESP_LOGW(NETWORK_SERVICES_TAG,
                     "Network-service resume deferred; retry pending");
            return;
        }
    } else {
        s_suspended_for_ota = false;
    }
}

/* Retry from app_main's existing loop: no dedicated recovery task is needed. */
void network_services_retry_pending(void)
{
    bool retry = false;
    bool resume_retry = false;
    services_lock();
    if (s_initialized && !s_teardown_pending && s_sync_retry_pending &&
        !s_sync_scheduled &&
        (int32_t)(xTaskGetTickCount() - s_sync_retry_after_tick) >= 0) {
        s_sync_scheduled = true;
        s_sync_retry_pending = false;
        resume_retry = s_suspended_for_ota;
        if (resume_retry) {
            s_suspended_for_ota = false;
        }
        retry = true;
    }
    services_unlock();

    if (retry && !network_services_spawn_sync_task() && resume_retry) {
        s_suspended_for_ota = true;
    }
}

bool network_services_is_running(void)
{
    services_lock();
    const bool running = s_running;
    services_unlock();
    return running;
}

void network_services_get_status(network_services_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (!s_initialized) {
        return;
    }
    services_lock();
    status->http_running = s_http_server != NULL;
    status->dashboard_running = s_dashboard_running;
    status->websocket_running = s_websocket_running;
    status->mdns_running = s_mdns_running;
    status->ntp_running = s_ntp_running;
    status->ntp_time_set = s_ntp_running && ntp_client_time_is_set();
    status->mqtt_configured = s_mqtt_config.enabled && s_mqtt_config.broker_url[0] != '\0';
    services_unlock();
    status->mqtt_connected = mqtt_client_is_connected();
}

esp_err_t network_services_get_mqtt_config(network_mqtt_config_t *config)
{
    if (config == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    services_lock();
    *config = s_mqtt_config;
    services_unlock();
    return ESP_OK;
}

esp_err_t network_services_set_mqtt_config(const network_mqtt_config_t *config)
{
    if (!s_initialized || !mqtt_config_validate(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t save_err = mqtt_config_save(config);
    if (save_err != ESP_OK) {
        return save_err;
    }

    services_lock();
    s_mqtt_config = *config;
    const bool running = s_running;
    services_unlock();
    if (running) {
        (void)mqtt_client_disconnect();
        (void)mqtt_client_deinit();
        (void)start_mqtt_if_enabled();
    }
    return ESP_OK;
}

esp_err_t network_services_mqtt_connect(void)
{
    if (!network_services_is_running()) {
        return ESP_ERR_INVALID_STATE;
    }
    network_mqtt_config_t config;
    if (network_services_get_mqtt_config(&config) != ESP_OK ||
        !config.enabled || config.broker_url[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    return start_mqtt_if_enabled();
}

esp_err_t network_services_mqtt_disconnect(void)
{
    return mqtt_client_disconnect();
}

int network_services_mqtt_publish(const char *topic, const char *data, int qos, bool retain)
{
    char default_topic[NETWORK_MQTT_TOPIC_MAX] = {0};
    if (topic == NULL || topic[0] == '\0') {
        network_mqtt_config_t config;
        if (network_services_get_mqtt_config(&config) != ESP_OK) {
            return -1;
        }
        strncpy(default_topic, config.publish_topic, sizeof(default_topic) - 1U);
        topic = default_topic;
    }
    if (data == NULL || topic[0] == '\0' || qos < 0 || qos > 2) {
        return -1;
    }
    return mqtt_client_publish(topic, data, qos, retain ? 1 : 0);
}

int network_services_mqtt_subscribe(const char *topic, int qos)
{
    char default_topic[NETWORK_MQTT_TOPIC_MAX] = {0};
    if (topic == NULL || topic[0] == '\0') {
        network_mqtt_config_t config;
        if (network_services_get_mqtt_config(&config) != ESP_OK) {
            return -1;
        }
        strncpy(default_topic, config.subscribe_topic, sizeof(default_topic) - 1U);
        topic = default_topic;
    }
    if (topic[0] == '\0' || qos < 0 || qos > 2) {
        return -1;
    }
    return mqtt_client_subscribe(topic, qos);
}
