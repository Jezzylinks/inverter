/**
 * @file wifi_dns_server.c
 * @brief Captive Portal DNS Server
 */

#include "wifi_dns_server.h"
#include "task_watchdog.h"
#include <string.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#define DNS_SERVER_PORT 53
#define DNS_MAX_PACKET_SIZE 512
#define DNS_TASK_STACK_SIZE 4096
#define DNS_TASK_PRIORITY 4

/*
 * Captive portal IP
 * (SoftAP default)
 */
#define CAPTIVE_IP0 192
#define CAPTIVE_IP1 168
#define CAPTIVE_IP2 4
#define CAPTIVE_IP3 1

static const char *TAG = "WIFI_DNS_SERVER";

static size_t dns_skip_name(const uint8_t *packet,
                            size_t offset,
                            size_t length);

static esp_err_t dns_build_response(uint8_t *packet,
                                    size_t *length,
                                    size_t max_length);

/*----------------------------------------------------------
 * Static Variables
 *---------------------------------------------------------*/

static TaskHandle_t s_dns_task = NULL;

static int s_socket = -1;

static bool s_running = false;

/*----------------------------------------------------------
 * DNS Protocol Structures
 *---------------------------------------------------------*/

typedef struct __attribute__((packed))
{
    uint16_t id;

    uint16_t flags;

    uint16_t questions;

    uint16_t answers;

    uint16_t authority;

    uint16_t additional;

} dns_header_t;

/*
 * Resource Record
 */
typedef struct __attribute__((packed))
{
    uint16_t type;

    uint16_t class_;

    uint32_t ttl;

    uint16_t length;

} dns_rr_t;

/*----------------------------------------------------------
 * Forward Declarations
 *---------------------------------------------------------*/

static void dns_server_task(void *arg);

static esp_err_t dns_socket_create(void);

static void dns_socket_destroy(void);

/*----------------------------------------------------------
 * Create UDP Socket
 *---------------------------------------------------------*/

static esp_err_t dns_socket_create(void)
{
    struct sockaddr_in server_addr;

    memset(&server_addr,
           0,
           sizeof(server_addr));

    server_addr.sin_family = AF_INET;

    server_addr.sin_port = htons(DNS_SERVER_PORT);

    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    s_socket = socket(AF_INET,
                      SOCK_DGRAM,
                      IPPROTO_UDP);

    if (s_socket < 0)
    {
        ESP_LOGE(TAG,
                 "socket() failed (%d)",
                 errno);

        return ESP_FAIL;
    }

    /* Set socket timeout to allow periodic s_running checks */
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(s_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (bind(s_socket,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        ESP_LOGE(TAG,
                 "bind() failed (%d)",
                 errno);

        close(s_socket);

        s_socket = -1;

        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "DNS socket created");

    return ESP_OK;
}

/*----------------------------------------------------------
 * Destroy Socket
 *---------------------------------------------------------*/

static void dns_socket_destroy(void)
{
    if (s_socket >= 0)
    {
        shutdown(s_socket,
                 SHUT_RDWR);

        close(s_socket);

        s_socket = -1;
    }
}

/*----------------------------------------------------------
 * DNS Server Task
 *---------------------------------------------------------*/

static void dns_server_task(void *arg)
{
    task_watchdog_register("dns_server_task");
    (void)arg;

    uint8_t buffer[DNS_MAX_PACKET_SIZE];

    struct sockaddr_in client_addr;

    socklen_t client_len;

    if (dns_socket_create() != ESP_OK)
    {
        s_running = false;
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG,
             "DNS server started");

    while (s_running)
    {
        task_watchdog_feed();
        client_len = sizeof(client_addr);

        int len =
            recvfrom(s_socket,
                     buffer,
                     sizeof(buffer),
                     0,
                     (struct sockaddr *)&client_addr,
                     &client_len);

        if (len < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                /* Timeout - check s_running and continue */
                continue;
            }
            ESP_LOGW(TAG, "recvfrom error: %d", errno);
            continue;
        }

        if (len == 0)
        {
            continue;
        }

        /* Validate minimum DNS header size */
        if ((size_t)len < sizeof(dns_header_t))
        {
            ESP_LOGW(TAG, "DNS packet too small: %d bytes", len);
            continue;
        }

        size_t packet_len = (size_t)len;

        if (dns_build_response(buffer,
                               &packet_len,
                               sizeof(buffer)) == ESP_OK)
        {
            sendto(s_socket,
                   buffer,
                   packet_len,
                   0,
                   (struct sockaddr *)&client_addr,
                   client_len);
        }
    }

    dns_socket_destroy();

    ESP_LOGI(TAG,
             "DNS server stopped");

    s_dns_task = NULL;

    vTaskDelete(NULL);
}

/*----------------------------------------------------------
 * Initialize
 *---------------------------------------------------------*/

esp_err_t wifi_dns_server_init(void)
{
    s_running = false;

    s_socket = -1;

    s_dns_task = NULL;

    ESP_LOGI(TAG,
             "DNS server initialized");

    return ESP_OK;
}

esp_err_t wifi_dns_server_deinit(void)
{
    return wifi_dns_server_stop();
}

/*----------------------------------------------------------
 * Start DNS Server
 *---------------------------------------------------------*/

esp_err_t wifi_dns_server_start(void)
{
    if (s_running)
    {
        return ESP_OK;
    }

    s_running = true;

    BaseType_t ret =
        xTaskCreate(
            dns_server_task,
            "dns_server",
            DNS_TASK_STACK_SIZE,
            NULL,
            DNS_TASK_PRIORITY,
            &s_dns_task);

    if (ret != pdPASS)
    {
        s_running = false;

        return ESP_FAIL;
    }

    return ESP_OK;
}

/*----------------------------------------------------------
 * Stop DNS Server
 *---------------------------------------------------------*/

esp_err_t wifi_dns_server_stop(void)
{
    if (!s_running)
    {
        return ESP_OK;
    }

    s_running = false;
    dns_socket_destroy();

    /* Wait up to 2 seconds for task to exit. Closing the socket unblocks recvfrom. */
    if (s_dns_task != NULL)
    {
        int wait_count = 20;
        while (s_dns_task != NULL && wait_count-- > 0)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    dns_socket_destroy();

    return ESP_OK;
}

/*----------------------------------------------------------
 * Status
 *---------------------------------------------------------*/

bool wifi_dns_server_is_running(void)
{
    return s_running;
}

/*----------------------------------------------------------
 * Skip DNS Encoded Name
 *---------------------------------------------------------*/

static size_t dns_skip_name(const uint8_t *packet,
                            size_t offset,
                            size_t length)
{
    while (offset < length)
    {
        uint8_t label_len = packet[offset];

        /*
         * End of name
         */
        if (label_len == 0)
        {
            offset++;

            break;
        }

        /*
         * Compression pointer
         */
        if ((label_len & 0xC0) == 0xC0)
        {
            offset += 2;

            break;
        }

        offset += label_len + 1;
    }

    return offset;
}

/*----------------------------------------------------------
 * Build DNS Response
 *---------------------------------------------------------*/

static esp_err_t dns_build_response(uint8_t *packet,
                                    size_t *length,
                                    size_t max_length)
{
    dns_header_t *hdr =
        (dns_header_t *)packet;

    /*
     * Standard response
     */
    hdr->flags = htons(0x8180);

    hdr->answers = htons(1);

    hdr->authority = 0;

    hdr->additional = 0;

    /*
     * Locate end of Question
     */
    size_t offset =
        sizeof(dns_header_t);

    offset = dns_skip_name(packet, offset, *length);

    /* A single, uncompressed or compressed question must include QTYPE/QCLASS. */
    if (*length < sizeof(dns_header_t) || ntohs(hdr->questions) != 1U ||
        offset > *length || offset + 4U > *length) {
        return ESP_ERR_INVALID_SIZE;
    }
    offset += 4U;

    /* Validate we have room for the answer. */
    if (offset + 16U > max_length)
    {
        ESP_LOGW(TAG, "DNS response too large for buffer");
        return ESP_FAIL;
    }

    /*
     * Answer Name
     * (Pointer to Question)
     */
    packet[offset++] = 0xC0;
    packet[offset++] = 0x0C;

    /*
     * TYPE = A
     */
    packet[offset++] = 0x00;
    packet[offset++] = 0x01;

    /*
     * CLASS = IN
     */
    packet[offset++] = 0x00;
    packet[offset++] = 0x01;

    /*
     * TTL = 60 seconds
     */
    packet[offset++] = 0x00;
    packet[offset++] = 0x00;
    packet[offset++] = 0x00;
    packet[offset++] = 0x3C;

    /*
     * IPv4 length
     */
    packet[offset++] = 0x00;
    packet[offset++] = 0x04;

    /*
     * 192.168.4.1
     */
    packet[offset++] = CAPTIVE_IP0;
    packet[offset++] = CAPTIVE_IP1;
    packet[offset++] = CAPTIVE_IP2;
    packet[offset++] = CAPTIVE_IP3;

    *length = offset;

    return ESP_OK;
}