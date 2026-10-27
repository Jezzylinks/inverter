# Local HTTPS for the inverter dashboard and provisioning interface

The normal local dashboard, JSON API, and WebSocket are served on HTTPS port 443 as well as the existing HTTP port 80. HTTP remains available for compatibility and captive-portal discovery; it is not automatically redirected to HTTPS. HTTPS startup is optional and does not prevent the HTTP dashboard or inverter control from starting if the device-local certificate/key are missing or invalid.

The separate captive provisioning portal also offers HTTPS on port 443 when runtime provisioning is enabled. It uses the same certificate/key files and only accepts credential-changing operations over HTTPS.

## Generate a device-local certificate

Do not commit the generated private key. Each inverter should have its own key and certificate.

From the repository root in PowerShell, read the unique hostname from the inverter LCD or `GET /api/v1/wifi` response (append `.local` if the API returns only the bare hostname), then run the script with that exact name. For example:

```powershell
./scripts/generate_local_https_cert.ps1 -DeviceHostname "solar-inverter-A1B2.local"
```

Replace `solar-inverter-A1B2.local` with this inverter's actual MAC-suffixed hostname. The script always adds the provisioning AP IP SAN from `WIFI_HTTPS_DEFAULT_IP` and adds the exact DNS SAN when `-DeviceHostname` is supplied. Without the parameter, the certificate matches only the provisioning AP IP, so a browser accessing the station dashboard by `.local` name will report an identity mismatch. Generate a different key/certificate for each inverter; do not share a private key between devices.

The equivalent OpenSSL command is (ECDSA P-256, valid for 825 days):

```powershell
New-Item -ItemType Directory -Force data | Out-Null
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -sha256 -nodes `
  -keyout data/https_server.key `
  -out data/https_server.crt `
  -days 825 `
  -subj "/CN=192.168.4.1" `
  -addext "subjectAltName=IP:192.168.4.1,DNS:solar-inverter-A1B2.local" `
  -addext "basicConstraints=critical,CA:FALSE" `
  -addext "keyUsage=critical,digitalSignature" `
  -addext "extendedKeyUsage=serverAuth"
```

The IP SAN is for direct AP access at `https://192.168.4.1/`; the DNS SAN is for the normal dashboard at `https://<actual-device-hostname>.local/`. The latter only matches when the DNS SAN is generated for that exact device hostname.

The certificate and key must be in the root of the SPIFFS filesystem image as `https_server.crt` and `https_server.key`. The provisioning listener reads them via `/spiffs/https_server.crt` and `/spiffs/https_server.key`; the dashboard listener reads the same files via the dashboard's existing `/www` mount.

The firmware does not format SPIFFS to create certificates. It reads the PEM files into RAM and wipes/frees its temporary copies after the TLS server starts. If files are missing, invalid, or TLS cannot start, HTTPS is skipped with a diagnostic log; the existing HTTP dashboard remains available, and the provisioning landing page clearly reports when secure setup is unavailable. No default/shared private key is included in this repository.

The separate captive provisioning portal only runs when `CONFIG_INVERTER_WIFI_RUNTIME_PROVISIONING=y`, and its access point requires a WPA2/WPA3 password of at least 8 characters. The normal dashboard HTTPS listener does not depend on that option.

## Build and upload

```powershell
pio run -e esp32dev
pio run -e esp32dev -t uploadfs
pio run -e esp32dev -t upload
pio device monitor -b 115200
```

After boot, use one of these local addresses:

- Station/LAN: `https://<actual-device-hostname>.local/` (requires the matching DNS SAN and working mDNS).
- Direct access point: `https://192.168.4.1/` (requires the device AP to be active).

The HTTP dashboard remains available at `http://<device-ip>/`. During captive provisioning, the HTTP landing page remains at `http://192.168.4.1/` and sensitive credential-save/reset operations are rejected over plaintext HTTP.

## Browser trust warning

The generated certificate is self-signed. TLS encrypts the connection, but a browser will normally warn that it cannot verify the device identity. Do not treat clicking through the warning as equivalent to a trusted certificate. For production deployment, provision a certificate whose identity matches the URL and whose issuing CA is trusted by the client device. The AP IP SAN matches `192.168.4.1`; the optional DNS SAN must exactly match the device hostname used in the URL.

## Memory and validation notes

ESP-IDF HTTPS server sockets have a substantial RAM cost. The dashboard HTTPS listener allows two simultaneous client sockets and uses a dedicated TLS server stack; the HTTP listener is limited to three clients so MQTT/NTP and captive DNS retain socket headroom. The inverter must be built and tested on the physical ESP32 with its normal Wi-Fi, ADC, LCD, and control workload active. Check startup logs for:
- `HTTPS dashboard/API/WebSocket started on port 443`
- `HTTPS provisioning server started on port 443` (only when runtime provisioning is enabled)
- `HTTPS dashboard unavailable` or `HTTPS dashboard TLS startup failed` if certificate loading or TLS startup fails

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
