# ESP32 Inverter Web Server Diagnostics

## What changed

- The local HTTP dashboard/API/WebSocket listener is retained across temporary STA association or DHCP loss. An intentional Wi-Fi OFF, provisioning transition, or OTA suspension still uses the existing service teardown path.
- A failed local HTTP server start is retried up to three times by the background network-services worker. The button/LCD path does not start network services synchronously.
- HTTPD session-close callbacks remove WebSocket client records even when a browser or phone drops TCP without sending a WebSocket close frame. The callback remains active until both HTTPD instances have stopped.
- HTTPS is limited to two open sockets. The browser serializes REST requests on HTTPS so one slot can remain occupied by the dashboard WebSocket and the other can serve one REST request.
- The services API and dashboard now report HTTP and HTTPS separately. The Wi-Fi menu has a **Web Server** item that displays HTTP, HTTPS, WebSocket, and the active AP/STA address on the LCD.
- Common operating-system connectivity probes (`/ncsi.txt`, `/connecttest.txt`, `/generate_204`, and `/hotspot-detect.html`) are handled explicitly rather than generating repeated 404 responses.

## TLS log interpretation

- `mbedtls_ssl_handshake returned -0x7780` is `MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE`: the remote browser/client sent a fatal TLS alert. A frequent cause with local ESP32 dashboards is that the browser does not trust the self-signed server certificate, or the URL host does not match the certificate SAN. This is not proof that the ESP32 listener is down.
- `-0x7280` is `MBEDTLS_ERR_SSL_CONN_EOF`: the peer closed the TCP/TLS connection during the handshake. Browser navigation, cancelled requests, captive-network probes, and clients abandoning an untrusted certificate can produce this.
- `setsockopt: 22` is an `EINVAL` socket-option failure. Do not treat one occurrence as proof that the listener stopped. Correlate it with the socket fd, client close, and whether new HTTP requests can still be served.
- `-0x0050` is `MBEDTLS_ERR_NET_CONN_RESET`: the TCP connection was reset by the peer (the browser/phone or an intervening network path). Treat it as a client/network abort unless repeated tests show it occurs while the client is still actively completing the handshake.

The firmware deliberately does not disable certificate checks or weaken TLS to hide these errors.

## Create and trust the local HTTPS certificate

The certificate script now creates a local root CA and signs a P-256 ECDSA server certificate. The server certificate contains the AP address `192.168.4.1` as an IP SAN. Supply the exact device mDNS hostname to add its DNS SAN:

```powershell
.\\scripts\\generate_local_https_cert.ps1 -DeviceHostname "vonix-inverter-A1B2.local"
```

Install `certs/vonix_inverter_root_ca.crt` as a trusted CA certificate on the phone/computer used to browse the inverter. Then upload the filesystem image:

```powershell
pio run -e esp32dev -t uploadfs
```

Open the dashboard using a name/address present in the certificate SAN, such as `https://192.168.4.1/` or the exact `.local` hostname supplied to the script. A DHCP station IP is not automatically covered by the certificate; use the mDNS name or regenerate with a certificate that includes the intended IP SAN.

The CA private key (`certs/vonix_inverter_root_ca.key`) and device private key (`data/https_server.key`) must never be committed or shared. Keep a backup of the CA private key: regenerating it requires reinstalling the new CA certificate on every client.

## Runtime checks

1. Connect to the inverter AP and open the dashboard at the AP address. The dashboard must remain available without a router or internet connection.
2. In STA mode, disconnect the router or move out of range, then reconnect. The HTTP listener should remain running through the temporary link loss and become reachable again after DHCP returns.
3. In APSTA mode, verify the AP address remains the address shown on the LCD while STA internet status changes independently.
4. Open the dashboard over HTTPS after trusting the local CA. Confirm the `/ws` connection stays connected while REST data updates.
5. Close the browser abruptly and reconnect repeatedly. WebSocket slots must be released by the HTTPD close callback.
6. Toggle Wi-Fi OFF and ON from the existing menu. The normal owner-managed stop/start path must still stop and restore services.
7. Check Network Services in the dashboard and the **Wi-Fi → Web Server** LCD item. HTTP, HTTPS, and WebSocket states must agree with the server handles/routes actually registered.
8. Verify notification dismissal only hides notifications in the current browser; it must not clear active inverter faults or erase persistent fault history.

## Build and flash

```powershell
pio run -e esp32dev
pio run -e esp32dev -t upload
pio run -e esp32dev -t uploadfs
```

A firmware upload alone does not update the dashboard files in SPIFFS; run `uploadfs` after changing `data/` assets or local HTTPS certificate files.
