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

The firmware mounts the existing `spiffs` partition without formatting it. If the files are missing, invalid, or TLS cannot start, the captive HTTP landing page remains available and logs explain why HTTPS did not start. No default/shared private key is included in this repository.

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

Do not merge this branch until the PlatformIO build succeeds and a device test confirms HTTPS access, credential save/reset over HTTPS, HTTP rejection of sensitive POSTs, captive-portal behavior, heap stability, and no regression to the already-working OTA flow.

The existing HTTPS OTA client and its certificate verification configuration are not changed by this feature.