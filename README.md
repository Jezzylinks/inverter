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

## Wi-Fi runtime hardening

The Wi-Fi implementation is organized around a controller-owned lifecycle. The manager creates and owns the ESP-IDF network interfaces; scanning, monitoring, station mode, provisioning, captive DNS, and the provisioning HTTP server are started only through coordinated controller transitions. Partial startup failures roll back already-created components, while disabling Wi-Fi stops monitor, portal, scan, and manager activity in dependency order. This keeps the persistent menu preference separate from transient connection status and prevents an incomplete portal or scan from surviving a failed operation.

| Area | Safeguard |
|---|---|
| Provisioning portal | Credential submission is accepted only through a bounded `POST` form. Request parsing limits encoded body size and field lengths, HTML output is escaped, responses add browser security headers, and credential-save callbacks run asynchronously outside the HTTP request context. DNS and HTTP startup is transactional; either service is rolled back if the companion service cannot start. |
| Credentials and configuration | Station and AP configuration is validated before it is written to NVS and again after it is loaded. Malformed persisted data is replaced with safe defaults rather than passed to the Wi-Fi driver. The manager restarts DHCP after applying a valid configuration and applies the requested authentication mode rather than assuming an open or WPA mode. |
| Scanning and event delivery | Network scans are serialized by a mutex, have an idempotent lifecycle, and release driver state on all paths. Connection status is exposed as synchronized snapshots; retry policy is explicit, reconnect work is deferred from event callbacks, and user callbacks run after locks are released. JSON and WebSocket status endpoints use those snapshots instead of mutable manager-owned pointers. |
| Captive DNS and monitoring | DNS packets must contain exactly one complete question before a captive response is built; name and question offsets are bounds-checked. Stopping the DNS service closes its socket before waiting for its task. The RSSI and internet monitor publishes callbacks outside locks and exits cooperatively when signaled, so its resources are not deleted while it is still executing. |

> The provisioning portal is intentionally local to the installer AP and does not expose the PIN-protected remote-control API. It should still be used only during commissioning, with the generated AP password handled as an installation secret.

## Wokwi simulation

The repository contains one unified firmware image. Wokwi does not compile the firmware itself; the referenced PlatformIO `.bin` and `.elf` files must exist before starting the simulator. From the repository root, run:

```bash
pio run
```

Then start Wokwi from the VS Code Command Palette with **Wokwi: Start Simulator**. The configuration points to `.pio/build/esp32dev/firmware.bin` and `.pio/build/esp32dev/firmware.elf`. The diagram uses the `wokwi-lcd2004` component so the expanded four-row interface can be tested. After boot, open **Settings → LCD Geometry** and select `20x4` to enable the four-row runtime view.

## Unified 16×2 and 20×4 LCD support

The project now builds one firmware for both physical HD44780-compatible I2C LCD geometries. The default LCD mode is 16×2 and is persisted in the normal settings NVS namespace. Open **Settings → LCD Geometry**, choose `16x2` or `20x4`, and save the setting. The LCD task then reinitializes the row map, CGRAM character set, dirty-row cache, and screen rendering without requiring a different firmware binary. A factory reset returns the stored LCD mode to the default 16×2 setting.

| Selection | Active hardware behavior |
|---|---|
| `16x2` | Uses 16 columns and two physical rows. Menus retain the compact two-row layout, with the selection counter at the end of the second row, such as `3/8`. |
| `20x4` | Uses 20 columns and four physical rows. Menus display four options, scroll the visible window as Up/Down changes the selection, and keep the counter on row four, such as `5/8`. |

The firmware allocates buffers for the larger panel but writes only the active number of rows and columns. The 20×4 row map uses the standard HD44780 DDRAM offsets `{0x00, 0x40, 0x14, 0x54}`. The active mode is a user setting, not a separate build target; therefore the same application, Wi-Fi, OTA, battery, security, button, event, and protection subsystems run in either display mode.

### Menu behavior

In 16×2 mode, the selected item and the next item remain on the two available rows. The arrow follows the selected option, and the position indicator remains at the end of the visible second row. In 20×4 mode, four menu options are rendered at once. Up and Down move the selection through the complete menu; when the selected index moves outside the current four-row window, the window scrolls while the arrow follows the selected item. The final row reserves enough space for the counter, so values such as `3/4`, `5/8`, or `17/17` remain visible.

### Runtime screens and custom characters

The selected mode controls boot, main telemetry, standby, menu, detail, confirmation, value-edit, shutdown, fault, loading, diagnostics, Wi-Fi scanning, Wi-Fi connection, event, and factory-reset screens. The 20×4 mode exposes the additional telemetry and navigation rows, while the 16×2 mode keeps the compact established presentation.

CGRAM is reloaded whenever the LCD is initialized or the user changes geometry. The 16×2 set uses bar levels and battery bracket glyphs. The 20×4 set uses bar levels and the following Wi-Fi activity glyphs:

| Slot | Glyph | Meaning and use |
|---:|---|---|
| 0–2 | Bar levels | Battery, load, or signal presentation. |
| 3 | TX arrow | Data transmission activity. |
| 4 | RX arrow | Data reception activity. |
| 5 | Link glyph | Link negotiation or connection state. |
| 6 | Lock glyph | Protected provisioning/security state. |
| 7 | Alert glyph | Wi-Fi attention or failure state. |

### Hardware and switching workflow

The display geometry is a physical-module choice, while the firmware selection is a runtime setting. Install the correct 16×2 or 20×4 HD44780-compatible I2C panel, keep the validated I2C address, SDA/SCL pins, power, ground, contrast, and logic levels, then flash the same `esp32dev` firmware. On first boot, the default is 16×2. Navigate to **Settings → LCD Geometry**, select the panel installed on the inverter, and save. The display reinitializes and continues using the selected geometry after reboot because the choice is stored in NVS.

Use one build and upload command for both panels:

```bash
pio run
pio run --target upload
```

The only difference is the saved **LCD Geometry** setting. Do not select `20x4` while a 16×2 physical module is installed, because the firmware will address four physical rows. Likewise, select `20x4` when using the 20×4 module so the additional rows and advanced menu layout are enabled.

## Security notes

Panel security is enabled by default for new installations. The initial default PIN is provisioned only as a salted hash and is marked as requiring a change. Product commissioning should change that PIN before enabling remote access. Remote API clients must provide the PIN as six decimal digits in the `X-Inverter-PIN` header. Failed attempts are locked out temporarily, and disabling panel security is an explicit configuration choice rather than a startup side effect.

## Battery profile and persistence notes

Battery chemistry, system voltage, capacity, and protection thresholds are loaded before battery hardware initialization. Learned estimator state is persisted independently and is rejected if it has an incompatible version or unsafe numeric values. Confirm that the selected chemistry, voltage system, capacity, cutoff, recharge, charge-current, and discharge-current values match the physical battery bank before enabling inverter output.

## Deployment checklist

Before field deployment, verify the physical flash size, battery profile, GPIO mapping, relay polarity, ADC calibration, fan tachometer wiring, and the release server’s certificate chain. Run `pio run` from a clean checkout, retain the generated firmware and partition table together, and perform a controlled power-on self-test before connecting a live load.
