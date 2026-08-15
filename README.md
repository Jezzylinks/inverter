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
| Security | PINs are stored as salted SHA-256 hashes. New devices, including a missing settings namespace, provision default PIN `0000` and mark it for mandatory change. Five incorrect attempts trigger a temporary lockout, comparisons are constant-time, and NVS writes are committed atomically. |
| Wi-Fi | The controller owns the Wi-Fi stack, event loop integration, reconnect state, scan subsystem, provisioning lifecycle, and credential storage. Station credentials are length-checked, weak non-empty WPA passwords are rejected, and no station or AP secret is compiled into the source. Menu enable/disable actions update the LCD immediately; disabling Wi-Fi also disables automatic reconnect. |
| HTTP API | JSON API endpoints require the local PIN through the `X-Inverter-PIN` header whenever panel security is enabled. Wildcard CORS is removed; the provisioning AP origin is explicitly allowed. Credential reset stops Wi-Fi before erasing stored credentials. |
| OTA | OTA accepts HTTPS URLs only, uses the ESP-IDF certificate bundle, runs in a dedicated task, supports cooperative cancellation, reports status/progress, and cleans up event handlers and buffers on every exit path. |
| OTA CSV | Release metadata can be fetched from a bounded HTTPS CSV document. The first valid row is selected using the format `version,url,sha256,size`; the URL and version are required, and all fields are length-checked. ESP-IDF image verification remains authoritative; the CSV SHA-256 field is release metadata and is not treated as a substitute for signed-image validation. |
| POST | LCD, ADC, and fan tests all run and report independently. Results include a failure bitmask and elapsed time. A failed fan preparation path explicitly disables the fan before returning. |
| Events | Subscriber queues are allocated transactionally, initialization is idempotent, teardown is available, and queue overflow is logged instead of silently disappearing. |
| Battery | Learned SOC/SOH state is accepted only when its version, size, finite-value, and range checks pass. Boot no longer overwrites the user’s battery profile; defaults are generated only when no valid profile exists. The selected 12/24/48 V system is stored under the canonical NVS key and resynchronizes protection thresholds and display state after validation. |
| Application modules | `main.c` now concentrates on boot, hardware, POST, and the top-level supervisory loop. `app_menu.c` owns menu tables, rendering, and history; `app_input.c` owns all Power, Enter/Menu, Up, Down, and Back semantics; `app_buttons.c` binds the five GPIOs to the one shared button task; and `app_services.c` owns menu-facing Wi-Fi and OTA intent. |
| Runtime hardening | Every firmware-owned long-running task registers with the shared task-watchdog helper and feeds it during work and bounded waits. Button clicks are delivered directly to the buzzer subscriber for reliable low-latency feedback; held Up/Down input emits a limit tone whenever a numeric boundary is reached, including repeated boundary hits. The inverter status LED is restored after higher-priority LED patterns while the inverter remains active. |
| Protected actions | Factory-reset input and confirmation share one authoritative LCD context, so the verified PIN advances to the selected action rather than a stale prompt. Firmware installation requires a verified panel PIN before the final OTA confirmation; automatic availability checks remain read-only. |

## OTA CSV manifest

The OTA manifest is a small, bounded CSV file served over HTTPS. A header is optional, blank lines and comment lines beginning with `#` are ignored, and the first valid data row is used.

```csv
version,url,sha256,size
1.4.0,https://updates.example.com/inverter-1.4.0.bin,0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef,1081344
```

The application layer stores its manifest source in NVS (`ota_manifest`) through `app_services_set_ota_manifest_url()`. It accepts only an HTTPS URL. After a Wi-Fi connection is available, the firmware checks that CSV on boot after a short delay and then at six-hour intervals. It compares numeric release versions and **only shows a screen notification** when a newer release exists; no background download is performed.

From the **Firmware Update** menu, select **Check for Update** to run an immediate check. Select **Install Available** only after a notification has been received. The device then shows `Enter=Yes Back=No`; pressing **Enter** starts the CSV-based OTA transaction, while **Back** or **Power** defers it safely. The update screen cannot start an image transfer without that explicit confirmation and a verified panel PIN when security is enabled. A running transfer can be cancelled from the menu when the OTA transport reaches its cooperative cancellation point. The startup and loading screens use a stable bar row; the filled progress block advances without the previous blank-row overwrite that caused visible blinking.

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

## Hardening behavior

The factory-reset PIN flow accepts `0000` on a new or recovered installation, then requires the user to choose a replacement PIN. The replacement is persisted as a salted hash in NVS and remains effective after reboot. Five incorrect PIN attempts activate the configured temporary lockout; a correct PIN clears the attempt counter.

The voltage-system setting is persisted using the same canonical NVS key used by the battery state. On boot, the value is validated first and then the battery protection thresholds, runtime battery state, and LCD presentation are synchronized again, so selecting 24 V or 48 V is not silently displayed as 12 V after restart.

Wi-Fi menu actions are intentionally asynchronous from the user-interface perspective. The LCD displays `Starting Wi-Fi` or `Wi-Fi Disabled` before the controller begins its transition. Turning Wi-Fi off disables automatic reconnect before stopping the manager, preventing a reconnect worker from undoing the user’s request.

The shared task-watchdog helper uses a 15-second timeout and bounded queue/sleep intervals so all firmware-owned tasks can feed the watchdog even while idle. The optional NimBLE host task remains managed by the NimBLE port because its event loop is third-party-owned; the firmware-owned provisioning and callback paths remain bounded and watchdog-covered.

## Wokwi simulation

The repository contains one shared firmware environment. Wokwi does not compile the firmware itself; the referenced PlatformIO `.bin` and `.elf` files must exist before starting the simulator. From the repository root, run:

```bash
pio run
```

Then start Wokwi from the VS Code Command Palette with **Wokwi: Start Simulator**. The configuration points to `.pio/build/esp32dev/firmware.bin` and `.pio/build/esp32dev/firmware.elf`. The checked-in diagram uses the `wokwi-lcd2004` component for the 20×4 configuration. Set `MENU_CONFIG_LCD_20X4` to `1` in `src/menu_config.h` before building when using that diagram.

## Compile-time 16×2 and 20×4 LCD selection

The project has one shared codebase and one PlatformIO environment. The physical LCD is selected before compilation in:

```text
src/menu_config.h
```

The controlling line is:

```c
#define MENU_CONFIG_LCD_20X4 0
```

Use `0` for a 16×2 LCD and `1` for a 20×4 LCD. After changing the value, build the same environment:

```bash
pio run
pio run --target upload
```

The value in `menu_config.h` is compiled throughout the entire firmware. It determines `LCD_ROWS`, `LCD_COLS`, `LCD_LINE_SIZE`, HD44780 row addressing, driver bounds, dirty-row caching, screen layouts, menu rendering, selection counters, and the CGRAM custom-character set. There is no runtime LCD Geometry setting, no LCD geometry NVS value, and no separate 20×4 PlatformIO environment.

| `MENU_CONFIG_LCD_20X4` | Physical LCD | Result |
|---:|---|---|
| `0` | 16×2 | Two physical rows. The compact dashboard uses explicit Enter-driven pages; the Wi-Fi indicator and signal bars remain visible without timed home-screen rotation. |
| `1` | 20×4 | Four physical rows. The stable dashboard shows inverter state, Wi-Fi signal, PV/load/grid values, battery SOC, operating mode, and battery-system voltage in one view. |

### Selecting 16×2 before compiling

Open `src/menu_config.h` and leave or set:

```c
#define MENU_CONFIG_LCD_20X4 0
```

Then run `pio run` and upload the generated firmware. The firmware uses two rows and 16 columns throughout the complete boot-to-shutdown UI.

### Selecting 20×4 before compiling

Open `src/menu_config.h` and change the value to:

```c
#define MENU_CONFIG_LCD_20X4 1
```

Then run `pio run` and upload the generated firmware. The firmware uses four rows and 20 columns throughout the complete UI. In the menu, the visible selection window contains four options. If the selected option moves beyond the current window, the rows scroll and the arrow follows it; the position counter remains on row four.

### Screens and custom characters

The compile-time selection controls boot, main telemetry, standby, menu, detail, confirmation, value-edit, shutdown, fault, loading, diagnostics, Wi-Fi scanning, Wi-Fi connection, event, security, and factory-reset screens. The 20×4 build exposes the full stable dashboard; the 16×2 build retains compact Enter-driven dashboard pages. Home and standby screens do not auto-rotate: the center/Enter button advances pages, while Up/Down remains reserved for menu navigation and value editing.

The 20×4 home dashboard is arranged as:

```text
INV 5.0kW ON  W||||
PV:2.45kW BAT:100%
LD:1.35kW GR:0.00kW
SOLAR PRIORITY 48V
```

`W||||` is the live Wi-Fi logo/signal indicator. The PV value is derived from the available DC input voltage/current telemetry; grid power remains `0.00kW` until a dedicated grid-power measurement is available, rather than displaying a fabricated value.

CGRAM is initialized with the set matching the compiled geometry. The 16×2 build uses bar levels and battery bracket glyphs. The 20×4 build uses bar levels and Wi-Fi activity glyphs:

| Slot | 20×4 glyph | Meaning and use |
|---:|---|---|
| 0–2 | Bar levels | Battery, load, or signal presentation. |
| 3 | TX arrow | Data transmission activity. |
| 4 | RX arrow | Data reception activity. |
| 5 | Link glyph | Link negotiation or connection state. |
| 6 | Lock glyph | Protected provisioning/security state. |
| 7 | Alert glyph | Wi-Fi attention or failure state. |

### Hardware configuration

Install the physical LCD that matches the value compiled in `src/menu_config.h`. Keep the existing validated I2C address, SDA/SCL pins, power, ground, contrast, and logic-level configuration. If the physical panel changes, change `MENU_CONFIG_LCD_20X4`, rebuild, and upload again. Do not select `0` for a 20×4 panel or `1` for a 16×2 panel, because the firmware will use the wrong row count and column width.

The 20×4 driver uses the standard HD44780 DDRAM offsets `{0x00, 0x40, 0x14, 0x54}`. For Wokwi, the diagram component must also match the compiled choice: use `wokwi-lcd1602` for a 16×2 simulation and `wokwi-lcd2004` for a 20×4 simulation. Wokwi project configuration details are documented in the [Wokwi project configuration guide](https://docs.wokwi.com/vscode/project-config) and the [Wokwi 20×4 LCD reference](https://docs.wokwi.com/parts/wokwi-lcd2004).

## Security notes

Panel security is enabled by default for new installations. The initial default PIN is `0000`, provisioned only as a salted hash and marked as requiring a change. If the settings namespace or PIN material is missing or corrupt, the firmware safely reprovisions `0000` and again requires a change. Factory reset and firmware installation both verify this same persisted security PIN; they cannot use a separate or stale factory PIN. Each protected option allows five incorrect attempts, then applies its own 30-second lockout. The LCD counts down from 30 seconds to 0, and Back cancels the current option and returns to its parent page. Successful verification resets that option’s timer. Remote API clients must provide the PIN as four decimal digits in the `X-Inverter-PIN` header, and disabling panel security is an explicit configuration choice rather than a startup side effect.

## Battery profile and persistence notes

Battery chemistry, system voltage, capacity, and protection thresholds are loaded before battery hardware initialization. The battery ADC first converts the sensed divider voltage, then applies the selected-system multiplier (`12/12`, `24/12`, or `48/12`) before publishing pack voltage to SOC, protection, runtime estimation, and the LCD. The battery-monitoring task does not overwrite this value with a fixed 12 V conversion. Learned estimator state is persisted independently and is rejected if it has an incompatible version or unsafe numeric values. Confirm that the selected chemistry, voltage system, capacity, cutoff, recharge, charge-current, and discharge-current values match the physical battery bank before enabling inverter output.

The multiplier is a software scaling step after the physical divider. It does not make a 12 V resistor divider electrically safe for direct 24 V or 48 V input. The divider and any protection components must be designed so the ESP32 ADC pin remains within its safe voltage range for the highest selected battery system.

## Deployment checklist

Before field deployment, verify the physical flash size, battery profile, GPIO mapping, relay polarity, ADC calibration, fan tachometer wiring, and the release server’s certificate chain. Run `pio run` from a clean checkout, retain the generated firmware and partition table together, and perform a controlled power-on self-test before connecting a live load.
