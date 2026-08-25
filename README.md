# Advanced Inverter Firmware

This repository contains the ESP32 firmware for the advanced inverter controller. The current architecture is organized around a single application state, explicit subsystem ownership, event-driven coordination, and fail-safe startup behavior. Hardware-specific protection limits remain in the existing configuration headers and must be reviewed against the installed inverter before deployment.

## Build and flash

The project is built with PlatformIO and ESP-IDF 5.3 through the pinned `espressif32@6.8.1` platform. The environment targets an ESP32 Dev Module with a 4 MiB flash device and uses `partitions.csv`, which provides two 1.5 MiB OTA application slots plus NVS, OTA metadata, PHY data, and SPIFFS storage. Wi-Fi credentials are supplied through the project Kconfig menu and are compiled into the firmware. The tracked `sdkconfig.defaults` file is sanitized and contains no real credentials; generated `sdkconfig` files are intentionally ignored by Git.

```bash
# Configure Wi-Fi credentials before the first build/flash.
idf.py -D SDKCONFIG=sdkconfig.esp32dev menuconfig
# Select: Inverter Wi-Fi Configuration

pio run
pio run --target upload
pio device monitor
```

The 4 MiB setting is intentional. Verify the physical module before flashing; do not use this partition table on a 2 MiB device. The build output reports the application size and partition budget so oversized releases are rejected before upload.

### Windows PlatformIO clean-build recovery

The `src/idf_component.yml` manifest declares the ESP-IDF mDNS dependency. PlatformIO's ESP-IDF component manager downloads and generates its `managed_components/` working directory from that manifest and the tracked `dependencies.lock` file. This generated directory is deliberately not versioned, so every developer receives component-manager output that matches the local PlatformIO and ESP-IDF installation.

If a Windows build reports a missing `component_requires.temp.cmake` file, close any active PlatformIO/ESP-IDF build terminals, open a terminal at the repository root, and remove the generated build and dependency directories before rebuilding:

```powershell
Remove-Item -Recurse -Force .pio, managed_components -ErrorAction SilentlyContinue
pio pkg update -e esp32dev
pio run -e esp32dev
```

Do not create or commit `component_requires.temp.cmake`; it is an internal CMake configuration file that PlatformIO regenerates. Run the command from the repository root, not from `.pio` or a copied source subdirectory. If the error remains after this clean build, update PlatformIO Core and retry with the pinned `espressif32@6.8.1` project configuration.

### Mock-feedback UI test build

For mobile UI integration testing before a dedicated inverter-output feedback GPIO is installed, build the explicitly named **test-only** profile:

```bash
pio run -e esp32dev-ui-mock
pio run -e esp32dev-ui-mock --target upload
```

This profile sets `physical_feedback_supported=true` and mirrors the relay command into `physical_feedback_active`, allowing the authenticated mobile control flow to be exercised when preflight interlocks are ready. Every REST and WebSocket payload also sets `physical_feedback_mocked=true`; both mobile clients display this as mock feedback rather than electrical confirmation. It does **not** bypass the existing PIN check, startup/shutdown state machine, interlocks, or fault protections. Because it is not a measurement, do not treat the profile as a production safety build and return to `pio run` before normal deployment.

## Runtime architecture

| Subsystem | Refactor and safety behavior |
|---|---|
| Button controller | All five physical buttons are handled by one FreeRTOS task. GPIO ISRs only enqueue edge notifications; debouncing, click classification, long-press detection, repeat generation, statistics, and callback dispatch are centralized. |
| Security | PINs are stored as salted SHA-256 hashes. New devices, including a missing settings namespace, provision default PIN `0000` and mark it for mandatory change. Five incorrect attempts trigger a temporary lockout, comparisons are constant-time, and NVS writes are committed atomically. |
| Wi-Fi | The controller owns the Wi-Fi stack, event loop integration, reconnect state, monitor, and compile-time credential application. STA/AP credentials, AP channel, and authentication mode come from the `Inverter Wi-Fi Configuration` Kconfig menu; runtime provisioning is disabled by default and no credentials are loaded from NVS. Menu enable/disable actions update the LCD immediately; disabling Wi-Fi also disables automatic reconnect. |
| HTTP API | JSON API endpoints require the local PIN through the `X-Inverter-PIN` header whenever panel security is enabled. Wildcard CORS is removed. Runtime Wi-Fi scan, credential-connect, and credential-reset endpoints return an explicit disabled response unless the opt-in provisioning Kconfig switch is enabled. |
| OTA | OTA accepts only HTTPS CSV manifests, uses the ESP-IDF certificate bundle, performs checks asynchronously, runs verified downloads in a dedicated task, supports cooperative cancellation, reports preparation/progress/verification states, verifies exact image size and SHA-256 while writing the next partition, rejects equal or older releases, and switches boot partitions only after verification. ESP-IDF rollback is enabled and the new image is marked valid only after NVS, LCD/input startup, and POST health checks pass; Wi-Fi and Internet availability are not required for validation. Direct unverified URL updates are rejected. |
| OTA CSV | Release metadata is fetched from a bounded HTTPS CSV document. The first valid row uses `version,url,sha256,size`; version, HTTPS URL, 64-hex SHA-256, and positive image size are mandatory. Versions use three or four numeric components (`MAJOR.MINOR.PATCH` or `MAJOR.MINOR.PATCH.BUILD`), with an optional leading `v`; malformed, equal, or older versions are rejected. A digest or size mismatch aborts the update and leaves the running partition unchanged. |
| POST | LCD, ADC, and fan tests all run and report independently. Results include a failure bitmask and elapsed time. A failed fan preparation path explicitly disables the fan before returning. |
| Events | Subscriber queues are allocated transactionally, initialization is idempotent, teardown is available, and queue overflow is logged instead of silently disappearing. |
| Battery | Learned SOC/SOH state is accepted only when its version, size, finite-value, and range checks pass. Boot no longer overwrites the user’s battery profile; defaults are generated only when no valid profile exists. The selected 12/24/48 V system is stored under the canonical NVS key and resynchronizes protection thresholds and display state after validation. |
| Application modules | `main.c` now concentrates on boot, hardware, POST, and the top-level supervisory loop. `app_menu.c` owns menu tables, rendering, and history; `app_input.c` owns all Power, Enter/Menu, Up, Down, and Back semantics; `app_buttons.c` binds the five GPIOs to the one shared button task; and `app_services.c` owns menu-facing Wi-Fi and OTA intent. |
| Runtime hardening | Wi-Fi and network-service tasks deliberately do not register with the shared task-watchdog helper. Their waits are bounded, reconnect work is cancellable, and Wi-Fi operations have explicit terminal timeouts. Unrelated firmware tasks continue to use the shared watchdog. Button clicks are routed through the central event dispatcher before reaching the buzzer subscriber; held Up/Down input emits a limit tone whenever a numeric boundary is reached, including repeated boundary hits. The inverter status LED is restored after higher-priority LED patterns while the inverter remains active. |
| Protected actions | Factory-reset input and confirmation share one authoritative LCD context, so the verified PIN advances to the selected action rather than a stale prompt. Firmware installation requires a verified panel PIN before the final OTA confirmation; automatic availability checks remain read-only. |

## Network services and local API

The normal dashboard, REST API, WebSocket endpoint, mDNS advertisements, NTP client, and MQTT integration share one HTTP-server lifecycle owned by `network_services`. Each optional service reports failure independently so a missing MQTT broker, mDNS conflict, or NTP synchronization problem does not prevent the local dashboard and API from starting. The runtime provisioning captive portal remains a separate, explicitly opt-in compatibility service because it owns its own captive-DNS and provisioning routes; it is not started by the normal dashboard lifecycle.

All versioned API routes are served from the shared HTTP server and require the `X-Inverter-PIN` header whenever panel security is enabled. JSON responses use actual system-state, ADC, battery-estimator, Wi-Fi-monitor, network-service, and OTA-coordinator data. Unsupported measurements are returned as `null` or with an explicit unavailable reason rather than being fabricated.

| API group | Canonical routes | Purpose |
|---|---|---|
| Device status | `GET /api/v1/status`, `/api/v1/system` | High-level readiness, inverter state, faults, firmware, hardware, uptime, heap, reset reason, LCD geometry, and NTP time. The status response retains flat legacy Wi-Fi fields while also exposing a nested `wifi` object. |
| Telemetry | `GET /api/v1/inverter`, `/api/v1/battery`, `/api/v1/solar`, `/api/v1/load`, `/api/v1/grid` | Inverter, battery, solar/PV, load, and grid data from shared firmware measurements. Battery current and grid power remain `null` when no dedicated measurement exists. |
| Wi-Fi | `GET /api/v1/wifi`, `/api/v1/wifi/scan`, `POST /api/v1/wifi/connect`, `/api/v1/wifi/disconnect`, `/api/v1/wifi/reset`, `GET /api/v1/wifi/config` | Wi-Fi status and opt-in runtime provisioning operations. The former short routes such as `/api/v1/scan` and `/api/v1/connect` remain compatibility aliases. |
| Services | `GET /api/v1/services` | HTTP, dashboard, WebSocket, mDNS, NTP/time-sync, and MQTT state. |
| MQTT | `/api/v1/mqtt/config`, `/api/v1/mqtt/connect`, `/api/v1/mqtt/disconnect`, `/api/v1/mqtt/publish`, `/api/v1/mqtt/subscribe` | Existing authenticated MQTT configuration and control operations. |
| OTA | `GET /api/v1/ota`, `POST /api/v1/ota/check`, `/api/v1/ota/start`, `/api/v1/ota/confirm`, `/api/v1/ota/cancel` | Read OTA state, request an asynchronous manifest check, start an explicitly requested update, or cancel a pending/running operation through `app_services`. |
| Cloud reporting | `GET /api/v1/cloud`, `POST /api/v1/cloud/config` | PIN-protected direct HTTPS reporting configuration and non-secret enrollment/reporting status. |

The WebSocket endpoint is `/ws`. A client authenticates with `{"cmd":"authenticate","pin":"0000"}`, subscribes with `{"cmd":"subscribe"}`, and requests a one-client response with `{"cmd":"get_status"}`. The server sends Wi-Fi `status` events, typed `device` events with inverter/battery/solar/load telemetry, and typed `ota` events with state and progress. Only authenticated clients that explicitly subscribed receive asynchronous broadcasts; request/response messages are sent only to the requesting socket.

### Direct HTTPS cloud reporting

The firmware can be enrolled to report a bounded telemetry snapshot directly to the cloud backend over HTTPS, avoiding an always-on MQTT gateway. The local PIN-protected configuration endpoint stores the cloud origin, device hardware ID, one-time enrollment code, and reporting period in NVS; the enrollment code is exchanged for a device-only reporting token, which is never returned by local API responses. See [`docs/cloud_https_reporting.md`](docs/cloud_https_reporting.md) for the enrollment flow, payload, alert boundary, and safe remote-control requirements.

### REST and WebSocket verification

The repository includes `tools/verify_api.py`, a dependency-free, read-only verification client. It checks the protected REST authentication gate, validates the JSON contracts for status, system, inverter, battery, solar, load, grid, Wi-Fi, services, and OTA endpoints, then performs a raw WebSocket RFC 6455 handshake and verifies authentication, subscription, one-client status delivery, and `get_status` request/response behavior. It does not scan networks, mutate Wi-Fi credentials, publish MQTT messages, or start/cancel OTA operations.

Run the local hardware-independent self-test with:

```bash
python3 tools/verify_api.py --self-test
```

For a flashed device on the local network, supply the device URL and panel PIN either as arguments or environment variables:

```bash
python3 tools/verify_api.py --base-url http://192.168.4.1 --pin 1234
INVERTER_URL=http://inverter.local INVERTER_PIN=1234 python3 tools/verify_api.py
```

Use `--rest-only` or `--ws-only` to isolate a failing layer. The full suite requires the device’s HTTP server and WebSocket server to be running, valid panel authentication, and network reachability from the test host. A missing PIN intentionally verifies the HTTP 401 gate and exits nonzero with instructions for the full authenticated run.


## OTA CSV manifest

The OTA manifest is a small, bounded CSV file served over HTTPS. A header is optional, blank lines and comment lines beginning with `#` are ignored, and the first valid data row is used.

```csv
version,url,sha256,size
1.4.0,https://updates.example.com/inverter-1.4.0.bin,0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef,1081344
```

The application layer stores its manifest source in NVS (`ota_manifest`) through `app_services_set_ota_manifest_url()`. It accepts only an HTTPS URL and revalidates the value when loaded, checked, and installed. When no runtime URL is stored, the default is configurable through `CONFIG_INVERTER_OTA_MANIFEST_URL`, whose default remains the trusted GitHub release endpoint. After a Wi-Fi connection is available, the firmware checks that CSV on boot after a short delay and then at six-hour intervals. It first verifies station connectivity and then verifies Internet reachability through the bounded monitor; DHCP alone is not treated as Internet access. Automatic checks remain quiet when the current version is installed, while manual checks use a dedicated asynchronous worker and show **Checking**, **Firmware Current**, **Update Available**, or a retryable **Update Failed** state. No background image download is performed.

From the **Firmware Update** menu, select **Check for Update** to start the asynchronous check. Select **Install <version>** only after an update is available; **Retry** performs a fresh manifest check after a failure. The device then shows `Enter=Yes Back=No`; pressing **Enter** starts the CSV-based OTA transaction, while **Back** or **Power** defers it safely. The update screen cannot start an image transfer without that explicit confirmation and a verified panel PIN when security is enabled. A running transfer can be cancelled from the menu through `Cancel update? / ENTER=Yes BACK=No`; confirmation requests cooperative cancellation, aborts the active ESP-IDF OTA transaction, and keeps the current firmware without rebooting. Dedicated 20×4 and 16×2 renderers show checking, preparing, downloading progress, verification, success, cancellation, failure, and rollback recovery states without adding a second OTA operation state machine.

## Wi-Fi compile-time configuration and control

The repository includes a sanitized `sdkconfig.defaults` seed containing the non-secret project settings. Before building or flashing, run `idf.py -D SDKCONFIG=sdkconfig.esp32dev menuconfig` and open **Inverter Wi-Fi Configuration**. Set the Station SSID and password for normal STA operation. Set an AP password of at least eight characters to enable the optional AP; leaving the AP password empty disables AP mode. The AP is never started as an open network. Configure the AP channel and WPA2/WPA3 authentication choice in the same menu.

The **WiFi Control** menu is architecture-aware. Its first action is an immediate **WiFi: ON/OFF** toggle; enabling the radio starts the selected architecture without blocking the menu and does not initiate a station association. In STA and APSTA modes, the third action is **Connect** until the configured station is associated, then it becomes **Disconnect**. In AP and APSTA modes, **AP Clients** lists the connected client MAC addresses and Enter deauthenticates the selected client. The AP client limit is capped at four. The status view reports the selected mode, station state, IP, RSSI, and internet availability; AP-only status does not pretend that a station is connected. Wi-Fi intent (`wifi_enabled`) may still be persisted in NVS, but credentials and AP identity are not.

The operation architecture is selected at compile time in **Inverter Wi-Fi Configuration** through `INVERTER_WIFI_MODE_STA`, `INVERTER_WIFI_MODE_AP`, or `INVERTER_WIFI_MODE_APSTA`. STA starts only the station architecture and connects only after an explicit Connect action. AP starts the protected inverter network and local network services without a station monitor. APSTA keeps the AP active while the station remains **Not connected** until explicitly requested. The AP password must be at least eight characters; an open AP is never started.

Runtime provisioning is controlled by `CONFIG_INVERTER_WIFI_RUNTIME_PROVISIONING` and defaults to disabled. When disabled, the captive portal, scan-and-enter-password UI, BLE credential path, and REST credential mutation operations are unavailable. Enabling that compatibility option is an explicit product decision and does not change the default compile-time credential path.

## Wi-Fi runtime hardening

The Wi-Fi implementation is organized around an event-driven controller-owned lifecycle. The manager creates and owns the ESP-IDF network interfaces, applies Kconfig credentials, and skips STA connection cleanly when the Station SSID is empty. Reconnect delays use capped exponential backoff with cancellable 250 ms slices. In AP+STA mode, the AP channel must follow the associated STA channel; the firmware logs a warning if the preferred AP channel differs.

| Area | Safeguard |
|---|---|
| Compile-time credentials | Kconfig strings are length-checked at compile time against ESP32 SSID/password limits. An AP password must be empty or at least eight characters; an AP with an empty password is disabled rather than opened. Generated `sdkconfig` files are ignored so real credentials are not committed. |
| Configuration and state | NVS retains operational settings such as DHCP and reconnect policy, but the default STA/AP credential source is `sdkconfig.h`. Empty STA configuration is reported clearly and skipped. |
| Scanning and event delivery | Runtime scanning and password entry are removed from the default UI. Connection status is exposed as synchronized snapshots; reconnect work is deferred from event callbacks, uses capped exponential backoff, and user callbacks run after locks are released. JSON and WebSocket status endpoints use those snapshots instead of mutable manager-owned pointers. |
| Captive DNS and monitoring | Captive DNS and provisioning HTTP are not started when runtime provisioning is disabled. The internet monitor avoids blocking DNS resolution, uses bounded ping waits, and exits cooperatively when signaled. Wi-Fi paths intentionally remain outside the application task-watchdog registry. |

> The default firmware does not expose a provisioning portal or runtime credential-entry flow. Configure credentials with `idf.py menuconfig` before flashing and treat the generated `sdkconfig` files as secrets.

## P0/P1 hardening behavior

The ADC path now records bounded telemetry health for each voltage channel, rejects non-finite or physically implausible samples, separates AC-voltage telemetry from inverter-output telemetry, and requires fresh battery telemetry before inverter startup. If required battery telemetry becomes invalid while the inverter is active, the firmware immediately sets the commanded output and current limit to zero, opens the power relay, records a critical diagnostic snapshot, and latches the inverter in a fault state until a deliberate recovery path is used.

Settings persistence retains the existing per-key compatibility format but adds a versioned generation marker and deterministic fingerprint. A partially written settings set is rejected on the next boot and replaced with validated defaults rather than being silently treated as complete. Persistent diagnostics record boot count, reset reason, last fault flags, battery voltage, and fault timestamp. The watchdog supervisor records task heartbeats and stack margins in addition to the ESP-IDF 15-second task watchdog.

The REST API retains read-only status/configuration reporting and requires the same panel PIN when security is enabled. Runtime scan, connect, and credential-reset operations are explicitly disabled by default. Failed web PIN attempts use the general security scope and its lockout policy. The repository also includes host-side firmware contract tests and GitHub Actions validation for both LCD selectors, static analysis, and whitespace checks.

## Hardening behavior

The factory-reset PIN flow accepts `0000` on a new or recovered installation, then requires the user to choose a replacement PIN. The replacement is persisted as a salted hash in NVS and remains effective after reboot. Five incorrect PIN attempts activate the configured temporary lockout; a correct PIN clears the attempt counter.

The voltage-system setting is persisted using the same canonical NVS key used by the battery state. On boot, the value is validated first and then the battery protection thresholds, runtime battery state, and LCD presentation are synchronized again, so selecting 24 V or 48 V is not silently displayed as 12 V after restart.

Wi-Fi menu actions are intentionally asynchronous from the user-interface perspective. The LCD displays `Starting Wi-Fi`, `Wi-Fi Disabled`, or `Wi-Fi Not Config / Use menuconfig` before the controller begins or rejects its transition. Turning Wi-Fi off disables automatic reconnect before stopping the manager, preventing a reconnect worker from undoing the user’s request.

The Wi-Fi and network-service paths are intentionally excluded from the shared task-watchdog registry because watchdog interaction previously caused harmful resets during startup. They instead use bounded waits, cancellation notifications, timeout screens, and capped reconnect backoff. The shared watchdog remains available to unrelated firmware-owned tasks.

## Reusable ADC driver

The ESP-IDF ADC unit, calibration, multisampling, and resource-cleanup primitives are available as a standalone module in [`src/adc/adc.c`](src/adc/adc.c) with its public interface in [`include/adc/adc.h`](include/adc/adc.h). The inverter-specific `adc_task()`, channel mapping, divider scaling, telemetry health, filtering, and safety policy are isolated in [`src/adc/inverter_adc.c`](src/adc/inverter_adc.c), declared by [`include/adc/inverter_adc.h`](include/adc/inverter_adc.h). See [`docs/adc_driver.md`](docs/adc_driver.md) for copy-and-integrate instructions for subsequent ESP32 projects.

## Wokwi simulation

The repository contains one shared firmware environment. Wokwi does not compile the firmware itself; the referenced PlatformIO `.bin` and `.elf` files must exist before starting the simulator. From the repository root, run:

```bash
pio run
```

Then start Wokwi from the VS Code Command Palette with **Wokwi: Start Simulator**. The configuration points to `.pio/build/esp32dev/firmware.bin` and `.pio/build/esp32dev/firmware.elf`. The checked-in diagram uses the `wokwi-lcd2004` component for the 20×4 configuration. Set `MENU_CONFIG_LCD_20X4` to `1` in `include/lcd/menu_config.h` before building when using that diagram.

## Compile-time 16×2 and 20×4 LCD selection

The project has one shared codebase and one PlatformIO environment. The physical LCD is selected before compilation in:

```text
include/lcd/menu_config.h
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

Open `include/lcd/menu_config.h` and leave or set:

```c
#define MENU_CONFIG_LCD_20X4 0
```

Then run `pio run` and upload the generated firmware. The firmware uses two rows and 16 columns throughout the complete boot-to-shutdown UI.

### Selecting 20×4 before compiling

Open `include/lcd/menu_config.h` and change the value to:

```c
#define MENU_CONFIG_LCD_20X4 1
```

Then run `pio run` and upload the generated firmware. The firmware uses four rows and 20 columns throughout the complete UI. In the menu, the visible selection window contains four options. If the selected option moves beyond the current window, the rows scroll and the arrow follows it; the position counter remains on row four.

### Screens and custom characters

The compile-time selection controls boot, main telemetry, standby, menu, detail, confirmation, value-edit, shutdown, fault, loading, diagnostics, Wi-Fi scanning, Wi-Fi connection, event, security, and factory-reset screens. The 20×4 build exposes the full stable dashboard; the 16×2 build retains compact Enter-driven dashboard pages. Home and standby screens do not auto-rotate: the center/Enter button advances pages, while Up/Down remains reserved for menu navigation and value editing. Coded protection and system failures use `SYSTEM ERROR` on row one and `Error code: 0x...` on row two of a 20×4 LCD.

The 20×4 home dashboard is arranged as follows. The bottom row reports the live ADC-derived battery pack voltage, so the selected 24 V or 48 V scaling is visible on the home screen rather than only on the battery sub-page:

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

Install the physical LCD that matches the value compiled in `include/lcd/menu_config.h`. Keep the existing validated I2C address, SDA/SCL pins, power, ground, contrast, and logic-level configuration. If the physical panel changes, change `MENU_CONFIG_LCD_20X4`, rebuild, and upload again. Do not select `0` for a 20×4 panel or `1` for a 16×2 panel, because the firmware will use the wrong row count and column width.

The 20×4 driver uses the standard HD44780 DDRAM offsets `{0x00, 0x40, 0x14, 0x54}`. For Wokwi, the diagram component must also match the compiled choice: use `wokwi-lcd1602` for a 16×2 simulation and `wokwi-lcd2004` for a 20×4 simulation. Wokwi project configuration details are documented in the [Wokwi project configuration guide](https://docs.wokwi.com/vscode/project-config) and the [Wokwi 20×4 LCD reference](https://docs.wokwi.com/parts/wokwi-lcd2004).

#### Hardware GPIO assignment

The following table is the authoritative wiring reference for the firmware. **GPIO13 belongs exclusively to the buzzer. Do not connect the LCD backlight to GPIO13.** Move the physical LCD-backlight PWM wire to **GPIO25**.

| Hardware function | GPIO | Firmware definition / note |
|---|---:|---|
| Buzzer | **13** | `GPIO_BUZZER`; passive-piezo PWM output; unchanged and exclusive |
| LCD backlight PWM | **25** | `GPIO_LCD_BACKLIGHT`; LEDC low-speed timer 1, channel 3 |
| LCD power enable | 27 | `GPIO_LCD_POWER` |
| LCD I2C SDA | 21 | `GPIO_I2C_SDA` |
| LCD I2C SCL | 22 | `GPIO_I2C_SCL`; current `GPIO_NEPA_INPUT` alias also uses this pin |
| Power button | 16 | `GPIO_BUTTON_POWER` |
| Enter/Menu button | 19 | `GPIO_BUTTON_ENTER_MENU` |
| Up button | 17 | `GPIO_BUTTON_UP` |
| Down button | 5 | `GPIO_BUTTON_DOWN` |
| Back button | 18 | `GPIO_BUTTON_BACK` |
| Status LED | 14 | `GPIO_STATUS_LED` |
| Error LED | 26 | `GPIO_ERROR_LED` |
| Power relay | 12 | `GPIO_POWER_RELAY` |
| Fan PWM | 33 | `GPIO_FAN`; also used by the configured fan ADC channel |
| Fan tachometer | 35 | `GPIO_FAN_TACH`; input-only tachometer signal |
| Low-battery ADC | 34 | ADC input; input-only |
| Over/under-voltage ADC | 36 | ADC input; input-only |
| Inverter-output-voltage ADC | 32 | ADC input |

The active LEDC ownership is intentionally separated: the buzzer retains low-speed **timer 0, channel 0** on GPIO13; the status and error LEDs use timer 0, channels 1 and 2; and the LCD backlight uses low-speed **timer 1, channel 3** on GPIO25. The generic fan controller is configurable and has no active initialization call site in the current firmware. The Wokwi diagram follows the buzzer and status-LED assignments; its I2C LCD model does not expose a separate external backlight pin, so the GPIO25 change applies to the physical hardware wiring.

## Security notes

Panel security is enabled by default for new installations. The initial default PIN is `0000`, provisioned only as a salted hash and marked as requiring a change. If the settings namespace or PIN material is missing or corrupt, the firmware safely reprovisions `0000` and again requires a change. Factory reset and firmware installation both verify this same persisted security PIN; they cannot use a separate or stale factory PIN. Each protected option allows five incorrect attempts, then applies its own 30-second lockout. The LCD counts down from 30 seconds to 0, and Back cancels the current option and returns to its parent page. Successful verification resets that option’s timer. Remote API clients must provide the PIN as four decimal digits in the `X-Inverter-PIN` header, and disabling panel security is an explicit configuration choice rather than a startup side effect.

## Battery profile and persistence notes

Battery chemistry, system voltage, capacity, and protection thresholds are loaded before battery hardware initialization. The battery ADC first converts the sensed divider voltage, then applies the selected-system multiplier (`12/12`, `24/12`, or `48/12`) before publishing pack voltage to SOC, protection, runtime estimation, and the LCD. The battery-monitoring task does not overwrite this value with a fixed 12 V conversion. Learned estimator state is persisted independently and is rejected if it has an incompatible version or unsafe numeric values. Confirm that the selected chemistry, voltage system, capacity, cutoff, recharge, charge-current, and discharge-current values match the physical battery bank before enabling inverter output.

The multiplier is a software scaling step after the physical divider. It does not make a 12 V resistor divider electrically safe for direct 24 V or 48 V input. The divider and any protection components must be designed so the ESP32 ADC pin remains within its safe voltage range for the highest selected battery system.

## Deployment checklist

Before field deployment, verify the physical flash size, battery profile, GPIO mapping, relay polarity, ADC calibration, fan tachometer wiring, and the release server’s certificate chain. Run `pio run` from a clean checkout, retain the generated firmware and partition table together, and perform a controlled power-on self-test before connecting a live load.
