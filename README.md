# Advanced Inverter Firmware

This repository contains the ESP32 firmware for the advanced inverter controller. The current architecture is organized around a single application state, explicit subsystem ownership, event-driven coordination, and fail-safe startup behavior. Hardware-specific protection limits remain in the existing configuration headers and must be reviewed against the installed inverter before deployment.

## Build and flash

The project is built with PlatformIO and ESP-IDF 5.3 through the pinned `espressif32@6.8.1` platform. The environment targets an ESP32 Dev Module with a 4 MiB flash device and uses `partitions.csv`, which provides two 1.5 MiB OTA application slots plus NVS, OTA metadata, PHY data, and SPIFFS storage.

```bash
pio run
pio run --target upload
pio device monitor
```

The 4 MiB setting is intentional. Verify the physical module before flashing; do not use this partition table on a 2 MiB device. The build output reports the application size and partition budget so oversized releases are rejected before upload.

## Runtime architecture

| Subsystem | Refactor and safety behavior |
|---|---|
| Button controller | All five physical buttons are handled by one FreeRTOS task. GPIO ISRs only enqueue edge notifications; debouncing, click classification, long-press detection, repeat generation, statistics, and callback dispatch are centralized. |
| Security | PINs are stored as salted SHA-256 hashes. New devices provision the documented default PIN and mark it for mandatory change. Invalid PINs are rate-limited with lockout, comparisons are constant-time, and NVS writes are committed atomically. |
| Wi-Fi | The controller owns the Wi-Fi stack, event loop integration, reconnect state, scan subsystem, provisioning lifecycle, and credential storage. Station credentials are length-checked, weak non-empty WPA passwords are rejected, and no station or AP secret is compiled into the source. |
| HTTP API | JSON API endpoints require the local PIN through the `X-Inverter-PIN` header whenever panel security is enabled. Wildcard CORS is removed; the provisioning AP origin is explicitly allowed. Credential reset stops Wi-Fi before erasing stored credentials. |
| OTA | OTA accepts HTTPS URLs only, uses the ESP-IDF certificate bundle, runs in a dedicated task, supports cooperative cancellation, reports status/progress, and cleans up event handlers and buffers on every exit path. |
| OTA CSV | Release metadata can be fetched from a bounded HTTPS CSV document. The first valid row is selected using the format `version,url,sha256,size`; the URL and version are required, and all fields are length-checked. ESP-IDF image verification remains authoritative; the CSV SHA-256 field is release metadata and is not treated as a substitute for signed-image validation. |
| POST | LCD, ADC, and fan tests all run and report independently. Results include a failure bitmask and elapsed time. A failed fan preparation path explicitly disables the fan before returning. |
| Events | Subscriber queues are allocated transactionally, initialization is idempotent, teardown is available, and queue overflow is logged instead of silently disappearing. |
| Battery | Learned SOC/SOH state is accepted only when its version, size, finite-value, and range checks pass. Boot no longer overwrites the user’s battery profile; defaults are generated only when no valid profile exists. |
| Application modules | `main.c` now concentrates on boot, hardware, POST, and the top-level supervisory loop. `app_menu.c` owns menu tables, rendering, and history; `app_input.c` owns all Power, Enter/Menu, Up, Down, and Back semantics; `app_buttons.c` binds the five GPIOs to the one shared button task; and `app_services.c` owns menu-facing Wi-Fi and OTA intent. |

## OTA CSV manifest

The OTA manifest is a small, bounded CSV file served over HTTPS. A header is optional, blank lines and comment lines beginning with `#` are ignored, and the first valid data row is used.

```csv
version,url,sha256,size
1.4.0,https://updates.example.com/inverter-1.4.0.bin,0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef,1081344
```

The application layer stores its manifest source in NVS (`ota_manifest`) through `app_services_set_ota_manifest_url()`. It accepts only an HTTPS URL. After a Wi-Fi connection is available, the firmware checks that CSV on boot after a short delay and then at six-hour intervals. It compares numeric release versions and **only shows a screen notification** when a newer release exists; no background download is performed.

From the **Firmware Update** menu, select **Check for Update** to run an immediate check. Select **Install Available** only after a notification has been received. The device then shows `Enter=Yes Back=No`; pressing **Enter** starts the CSV-based OTA transaction, while **Back** or **Power** defers it safely. The update screen cannot start an image transfer without that explicit confirmation. A running transfer can be cancelled from the menu when the OTA transport reaches its cooperative cancellation point.

## Wi-Fi provisioning and control

The **WiFi Control** menu is the user-facing control plane. **WiFi On / Off** immediately starts or stops the controller and persists the requested state in NVS (`wifi_enabled`), so the same state is restored after reset. The remaining actions scan networks, connect saved credentials, disconnect without erasing them, start the provisioning AP, and show current Wi-Fi state. Scans and connection attempts are rejected with a screen message while Wi-Fi is disabled.

When no station credentials are present, the Wi-Fi controller enters provisioning mode. The AP SSID defaults to `INVERTER_SETUP`; its WPA2 password is generated from the device’s hardware random-number source when no explicit product password is configured. This avoids shipping a shared factory password. The generated password is persisted with the network configuration and should be communicated to the installer through the device commissioning process.

## Security notes

Panel security is enabled by default for new installations. The initial default PIN is provisioned only as a salted hash and is marked as requiring a change. Product commissioning should change that PIN before enabling remote access. Remote API clients must provide the PIN as six decimal digits in the `X-Inverter-PIN` header. Failed attempts are locked out temporarily, and disabling panel security is an explicit configuration choice rather than a startup side effect.

## Battery profile and persistence notes

Battery chemistry, system voltage, capacity, and protection thresholds are loaded before battery hardware initialization. Learned estimator state is persisted independently and is rejected if it has an incompatible version or unsafe numeric values. Confirm that the selected chemistry, voltage system, capacity, cutoff, recharge, charge-current, and discharge-current values match the physical battery bank before enabling inverter output.

## Deployment checklist

Before field deployment, verify the physical flash size, battery profile, GPIO mapping, relay polarity, ADC calibration, fan tachometer wiring, and the release server’s certificate chain. Run `pio run` from a clean checkout, retain the generated firmware and partition table together, and perform a controlled power-on self-test before connecting a live load.
