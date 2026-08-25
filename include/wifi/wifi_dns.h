/**
 * @file wifi_dns.h
 * @brief DNS Configuration
 */

#ifndef WIFI_DNS_H
#define WIFI_DNS_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_err.h"
#include "esp_netif.h"

    /*----------------------------------------------------------
     * Initialization
     *---------------------------------------------------------*/
    esp_err_t wifi_dns_init(void);

    /*----------------------------------------------------------
     * DNS Configuration
     *---------------------------------------------------------*/
    esp_err_t wifi_dns_set_server(
        esp_netif_t *netif,
        esp_ip4_addr_t dns);

    esp_err_t wifi_dns_get_server(
        esp_netif_t *netif,
        esp_ip4_addr_t *dns);

    esp_err_t wifi_dns_restore_default(
        esp_netif_t *netif);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_DNS_H */