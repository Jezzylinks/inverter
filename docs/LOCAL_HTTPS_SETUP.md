# Local HTTPS for the inverter provisioning interface

This branch keeps the HTTP listener on port 80 only for captive-portal detection and a landing page. Credential saving and credential erasure are rejected over HTTP and must be submitted over HTTPS. The same provisioning handlers are registered on HTTPS port 443.

## Generate a device-local certificate

Do not commit the generated private key. Each inverter should have its own key and certificate.

From the repository root in PowerShell, run:

```powershell
New-Item -ItemType Directory -Force data | Out-Null
openssl req -x509 -newkey rsa:2048 -sha256 -nodes `
  -keyout data/https_server.key `
  -out data/https_server.crt `
  -days 3650 `
  -subj "/CN=192.168.4.1" `
  -addext "subjectAltName=IP:192.168.4.1,DNS:solar-inverter.local"
```

The ESP32 expects these files in its SPIFFS filesystem at:
- `/spiffs/https_server.crt`
- `/spiffs/https_server.key`

The firmware mounts the existing `spiffs` partition without formatting it, reads both PEM files into RAM, and unmounts the partition again (unless another module such as the dashboard already owns the mount, in which case it reads the files from that mount and leaves it alone). The TLS server keeps its own copy of the PEM data, so the firmware wipes and frees its copies as soon as the server has started. If the files are missing, invalid, or TLS cannot start, the HTTP landing page says that secure setup is unavailable (it no longer links to a dead HTTPS page), nothing sensitive is accepted over HTTP, and the logs explain why HTTPS did not start. No default/shared private key is included in this repository.

Runtime provisioning (and therefore this server) only runs when `CONFIG_INVERTER_WIFI_RUNTIME_PROVISIONING=y`, and the provisioning access point requires a WPA2/WPA3 password of at least 8 characters.

## Build and upload

```powershell
pio run -e esp32dev
pio run -e esp32dev -t uploadfs
pio run -e esp32dev -t upload
pio device monitor -b 115200
```

After boot, connect to the inverter AP and explicitly open:

`https://192.168.4.1/`

The HTTP landing page remains at `http://192.168.4.1/` for captive portal discovery. It does not accept credential-save or credential-reset POST requests. This avoids sending the PIN or Wi-Fi password over plaintext HTTP.

## Browser trust warning

The generated certificate is self-signed. TLS encrypts the connection, but a browser will normally warn that it cannot verify the device identity. Do not treat clicking through the warning as equivalent to a trusted certificate. For production deployment, provision a certificate whose identity matches the URL and whose issuing CA is trusted by the client device. The certificate SAN above matches the AP IPv4 address only.

## Memory and validation notes

ESP-IDF HTTPS server sockets have a substantial RAM cost. This implementation limits HTTPS to one simultaneous open socket and uses a dedicated TLS server stack. The inverter must be built and tested on the physical ESP32 with its normal Wi-Fi, ADC, LCD, and control workload active. Check startup logs for:
- `HTTPS provisioning server started on port 443`
- `HTTPS unavailable` or `HTTPS server start failed`

## Provisioning PIN lockout

Web provisioning uses its own PIN lockout scope (`SECURITY_LOCKOUT_PROVISIONING`), separate from the front-panel PIN entry, so failed attempts over Wi-Fi cannot lock the local user out of the panel. As before, if panel security is explicitly disabled the PIN check is bypassed for this scope.

## Form encoding

The setup form is `application/x-www-form-urlencoded`. The server now decodes `+` and `%XX` for the SSID and password before validating and saving them, so names and passwords containing spaces or symbols are stored exactly as typed. Test with an SSID containing a space and a password containing `@ & + %`.

Do not merge this branch until the PlatformIO build succeeds and a device test confirms HTTPS access, credential save/reset over HTTPS, HTTP rejection of sensitive POSTs, captive-portal behavior, heap stability, and no regression to the already-working OTA flow.

The existing HTTPS OTA client and its certificate verification configuration are not changed by this feature.

## Socket and TLS hardening

The HTTP landing/captive-portal listener is limited to three open sockets; HTTPS allows two so one idle TLS client does not monopolize provisioning. LWIP is configured for 16 sockets to accommodate both listeners and DNS. The `/scan` and `/status` endpoints reject plain HTTP and are registered only on HTTPS. Scan requests are serialized and rate-limited to one per five seconds. The certificate generator uses ECDSA P-256 with an IP subject alternative name and explicit CA:false, digital-signature, and server-auth extensions; it expires after 825 days. TLS configuration disables legacy RSA key exchange, static ECDH key exchanges, and renegotiation while retaining ECDHE-ECDSA. Because mbedTLS settings affect the OTA client too, a successful PlatformIO build and full OTA regression test are required before merging.

`sdkconfig.defaults` only takes effect when a local sdkconfig is generated. `sdkconfig` and `sdkconfig.*` are git-ignored, so on a machine that has built this project before, delete `sdkconfig.esp32dev` (or `sdkconfig`) after pulling, otherwise `LWIP_MAX_SOCKETS=16` and the mbedTLS changes are silently ignored. CI builds from a clean checkout and is not affected. The firmware also has a compile-time check that fails the build if `CONFIG_LWIP_MAX_SOCKETS` is smaller than the provisioning socket budget (12). The `build/config/sdkconfig.h` committed in this repository is stale `idf.py` output, not the PlatformIO configuration.

Both provisioning servers register exactly 13 URI handlers, which equals `max_uri_handlers`. Raise that limit before adding a route.
