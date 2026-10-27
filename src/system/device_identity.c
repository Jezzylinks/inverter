#include "system/device_identity.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_mac.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "wifi/wifi_config.h"
#define DEVICE_HOSTNAME_CAPACITY 32U
static char s_hostname[DEVICE_HOSTNAME_CAPACITY] = WIFI_HOSTNAME;
static bool s_initialized;
static portMUX_TYPE s_identity_lock = portMUX_INITIALIZER_UNLOCKED;
const char *device_identity_hostname(void)
{
    char generated[DEVICE_HOSTNAME_CAPACITY];
    uint8_t mac[6] = {0};
    portENTER_CRITICAL(&s_identity_lock);
    if (s_initialized) {
        portEXIT_CRITICAL(&s_identity_lock);
        return s_hostname;
    }
    portEXIT_CRITICAL(&s_identity_lock);
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        (void)snprintf(generated, sizeof(generated), "%s-%02X%02X",
                       WIFI_HOSTNAME, mac[4], mac[5]);
    } else {
        (void)snprintf(generated, sizeof(generated), "%s", WIFI_HOSTNAME);
    }
    /* Concurrent startup callers may calculate the same value; publish once. */
    portENTER_CRITICAL(&s_identity_lock);
    if (!s_initialized) {
        (void)snprintf(s_hostname, sizeof(s_hostname), "%s", generated);
        s_initialized = true;
    }
    portEXIT_CRITICAL(&s_identity_lock);
    return s_hostname;
}
