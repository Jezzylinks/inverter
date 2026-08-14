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

The repository’s `wokwi.toml` is configured for the advanced 20×4 simulation. Wokwi does not compile the firmware itself; the referenced PlatformIO `.bin` and `.elf` files must exist before starting the simulator. From the repository root, build the exact environment referenced by the configuration:

```bash
pio run -e esp32dev_20x4
```

Then start Wokwi from the VS Code Command Palette with **Wokwi: Start Simulator**. The configuration points to `.pio/build/esp32dev_20x4/firmware.bin` and `.pio/build/esp32dev_20x4/firmware.elf`, while `diagram.json` uses the `wokwi-lcd2004` component. If you build only `esp32dev`, those 20×4 files will not be generated and Wokwi will report that it cannot find the firmware or ELF file.

For a 16×2 simulation, change the two paths in `wokwi.toml` to `.pio/build/esp32dev/firmware.bin` and `.pio/build/esp32dev/firmware.elf`, change the diagram component to `wokwi-lcd1602`, and build with:

```bash
pio run -e esp32dev
```

## Dual 16×2 and 20×4 LCD support

The firmware supports two HD44780-compatible I2C LCD geometries selected at build time. The default `esp32dev` environment builds the compact 16×2 interface, while `esp32dev_20x4` enables the advanced 20-column, 4-row interface through `LCD_GEOMETRY_20X4=1`. The geometry is centralized in `src/lcd_config.h`, so row buffers, dirty-row caching, CGRAM initialization, menu formatting, event notices, security screens, and diagnostic output all use the selected line width instead of assuming 16 characters.

| Target | LCD geometry | Build command | Intended presentation |
|---|---:|---|---|
| `esp32dev` | 16×2 | `pio run` | Compact two-line status and menu view for constrained panels. |
| `esp32dev_20x4` | 20×4 | `pio run -e esp32dev_20x4` | Detailed telemetry, navigation guidance, Wi-Fi activity, boot/shutdown progress, and expanded diagnostics. |

The two environments share the same ESP32 board, partition table, I2C LCD driver, and application behavior. Only the display geometry and geometry-specific rendering are changed. The 20×4 environment uses `sdkconfig.esp32dev_20x4`, which preserves the project’s NimBLE provisioning headers and settings. Build the desired environment before flashing; do not enable both geometry values in the same binary.

### 20×4 screen layout

The 20×4 interface is designed to expose more context without removing the compact 16×2 fallback. The main screen uses sub-pages for output, battery, and system telemetry. Output pages show inverter/AC state, load, battery voltage and SOC, while system pages show operating mode and page navigation. Standby shows AC state, battery voltage, SOC, health context, and the power-button instruction. Menus display four selectable options at once. Up and Down change the selected item; once the selection moves beyond the visible window, the four-row window scrolls to keep it visible. The selection counter, such as `3/4` or `5/17`, remains on the fourth row. The 16×2 target retains its established two-row menu behavior. Detail and value-edit screens use their lower rows for units, range context, and button hints.

Boot and shutdown are expanded into four-row progress views. POST, factory reset, fault, loading, event notification, error-log, settings-detail, and diagnostic screens use the extra rows when those states are active. Wi-Fi scanning can show four networks at once, and Wi-Fi connection progress uses a dedicated activity screen rather than overwriting the main telemetry view.

### Custom characters

The HD44780 CGRAM set is initialized after every LCD initialization or reinitialization. The compact 16×2 build reserves its custom-character slots for the battery bracket and bar-graph glyphs. The 20×4 build uses the five Wi-Fi activity glyphs below together with a compact bar graph, leaving the battery display in plain text so that the larger screen can devote CGRAM capacity to connection feedback.

| Slot | 20×4 glyph | Meaning and use |
|---:|---|---|
| 0–2 | Bar levels | Battery/load or signal-strength bar graph levels. |
| 3 | TX arrow | Data is being sent during Wi-Fi connection or provisioning activity. |
| 4 | RX arrow | Data is being received during Wi-Fi connection or provisioning activity. |
| 5 | Link glyph | Link negotiation or connection state. |
| 6 | Lock glyph | Protected provisioning/security state. |
| 7 | Alert glyph | Wi-Fi error, timeout, or attention state. |

The connection screen animates the TX/RX/LINK indicators and the trailing dots in `CONNECTING...`. These glyphs are presentation-only; Wi-Fi state remains owned by the controller and is reported to the LCD through the existing render-state path.

### Hardware configuration

Use an HD44780-compatible LCD with the project’s existing I2C backpack and wiring. The display geometry is a physical-module choice: install a 16×2 module for the default target or a 20×4 module for the advanced target. Keep the I2C address, SDA/SCL assignments, power, ground, and contrast adjustment identical to the validated installation. A 20×4 module must be configured as `esp32dev_20x4`; compiling the default target against a 20×4 panel will leave the controller using the wrong row map, and compiling the 20×4 target for a 16×2 panel will address rows that do not exist.

The 20×4 driver uses the standard HD44780 four-row DDRAM offsets `{0x00, 0x40, 0x14, 0x54}`. Verify the backpack’s I2C address and logic-level compatibility on the bench before connecting the display to an installed inverter. The display is not a substitute for the inverter’s protection system: confirm battery chemistry, selected 12/24/48 V system, ADC calibration, relay polarity, and fault thresholds before enabling output.

### Switching builds

To build the compact display, use:

```bash
pio run -e esp32dev
pio run -e esp32dev --target upload
```

To build the advanced display, use:

```bash
pio run -e esp32dev_20x4
pio run -e esp32dev_20x4 --target upload
```

The build-time selector can also be used with a custom environment by defining `LCD_GEOMETRY_20X4=1`. Leave the flag undefined or set it to `0` for 16×2. Do not edit the shared LCD source to change geometry manually; select the PlatformIO environment so the matching row map, buffers, custom-character set, and startup/reinitialization path are compiled together.

## Security notes

Panel security is enabled by default for new installations. The initial default PIN is provisioned only as a salted hash and is marked as requiring a change. Product commissioning should change that PIN before enabling remote access. Remote API clients must provide the PIN as six decimal digits in the `X-Inverter-PIN` header. Failed attempts are locked out temporarily, and disabling panel security is an explicit configuration choice rather than a startup side effect.

## Battery profile and persistence notes

Battery chemistry, system voltage, capacity, and protection thresholds are loaded before battery hardware initialization. Learned estimator state is persisted independently and is rejected if it has an incompatible version or unsafe numeric values. Confirm that the selected chemistry, voltage system, capacity, cutoff, recharge, charge-current, and discharge-current values match the physical battery bank before enabling inverter output.

## Deployment checklist

Before field deployment, verify the physical flash size, battery profile, GPIO mapping, relay polarity, ADC calibration, fan tachometer wiring, and the release server’s certificate chain. Run `pio run` from a clean checkout, retain the generated firmware and partition table together, and perform a controlled power-on self-test before connecting a live load.
