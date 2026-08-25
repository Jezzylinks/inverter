# Inverter Firmware `main.c` Refactor Report

## Scope and objective

The repository was inspected before modification. The refactor was intentionally structural: the ESP-IDF/PlatformIO entry point was reduced to startup orchestration while existing subsystem implementations, protocols, GPIO assignments, persistence keys, task behavior, and user-facing flows were preserved. The repository is the public ESP32 inverter firmware project at [Jezzylinks/inverter][1].

> **Behavioral outcome:** No intentional runtime behavior changes. This is a structural refactor.

## 1. Before

The original `src/main.c` contained **7,254 lines** and combined the application entry point with battery profile generation, NVS/settings persistence, ADC sampling and calibration, protection checks, relay control, inverter startup/shutdown, LCD/menu rendering, value editing, error logging, deep-sleep handling, watchdog setup, and miscellaneous compatibility helpers. The existing Wi-Fi, OTA, MQTT, NTP, server, security, POST, LCD, button, battery, and event modules were already present, but the entry-point translation unit still owned most of the application runtime implementation.

The most significant dependency concerns were the large shared-state surface and the number of subsystem functions declared or consumed through `main.c`. In particular, other modules referenced the global `sys_state`, synchronization handles, and inverter/settings functions that had historically been defined in the entry-point file. The startup sequence also had to preserve ordering across event initialization, mutex creation, rendering setup, NVS/settings/security loading, service startup, hardware initialization, task creation, ADC warm-up, POST, OTA validation, and startup-screen release.

| Concern found in the original file | Ownership decision |
| --- | --- |
| `app_main()` and the foreground startup loop | Extracted to `app_init.c`; `main.c` now only delegates to `app_init()`. |
| POST result display helper used only during boot | Moved with the startup coordinator to `app_init.c`. |
| Global runtime state and existing subsystem implementation | Retained in `app_runtime.c` to avoid a risky behavior rewrite or circular dependency cascade. |
| Existing Wi-Fi, OTA, server, MQTT, NTP, security, UI, event, POST, battery, and utility modules | Reused unchanged; no duplicate subsystem modules were created. |
| `nvs_initialized` | Remains private to the runtime module; startup now uses the existing `nvs_is_initialized()` API instead of reaching into that private variable. |
| ADC warm-up event bit | Published as `APP_EVENT_ADC_READY` through `app_runtime.h`, shared by the ADC producer and startup coordinator. |

## 2. After

`src/main.c` is now **6 lines** and contains only the ESP-IDF entry point:

```c
#include "app/app_init.h"

void app_main(void)
{
    app_init();
}
```

The extracted startup coordinator is `src/app_init.c` with **244 lines**, and its public declaration is `include/app/app_init.h`. The previous runtime implementation is now `src/app_runtime.c` with **7,020 lines**. This keeps the behavior-heavy implementation out of the entry point while avoiding an uncontrolled rewrite of safety-sensitive firmware.

| File | Responsibility |
| --- | --- |
| `src/main.c` | ESP-IDF entry point only; delegates to `app_init()`. |
| `src/app_init.c` | Ordered application startup, task creation, ADC warm-up/POST coordination, OTA running-image validation, startup UI release, and foreground maintenance loop. |
| `include/app/app_init.h` | Public `app_init()` declaration. |
| `src/app_runtime.c` | Existing non-startup runtime implementation and shared-state owner, moved from `main.c` without domain logic changes. |
| `include/app/app_runtime.h` | Narrow application lifecycle declarations, shared synchronization handles required by existing modules, `sys_state`, and `APP_EVENT_ADC_READY`. |
| `include/lcd/lcd_writer.h` | Ownership comment updated from `main.c` to `app_runtime.c`. |
| `tools/test_firmware_contracts.py` | ADC warm-up structural contract now checks `app_init.c`, the module that owns that branch. |

### Header organization

All project-owned headers now live under `include/` and are grouped by responsibility. The source tree contains implementation files only; no project-owned `.h` files remain under `src/`.

| Include namespace | Contents |
| --- | --- |
| `include/app/` | Application entry, runtime, input, menu, services, and button interfaces. |
| `include/battery/`, `include/cloud/`, `include/diagnostics/`, `include/events/` | Battery, cloud, telemetry/diagnostics, and event interfaces. |
| `include/hardware/`, `include/inverter/`, `include/lcd/`, `include/ota/`, `include/post/`, `include/security/` | Hardware, inverter, LCD, update, self-test, and security interfaces. |
| `include/server/` | JSON, mDNS, MQTT, NTP, signal, web, WebSocket, and network-service interfaces. |
| `include/system/`, `include/utility/`, `include/wifi/` | System-wide definitions, utility interfaces, and Wi-Fi interfaces. |

The component registration now exposes `../include` through `src/CMakeLists.txt`, and all project include directives use the canonical namespace paths. Documentation and the host-side contract test were updated accordingly.

### Functions moved from `main.c`

The startup function `app_main()` and the boot-only `post_show_result_and_notify()` helper moved into `app_init.c`. The runtime functions were not blindly redistributed across dozens of new files because the inspection showed extensive cross-references and shared state; keeping them together in `app_runtime.c` was the safer incremental boundary for this pass.

### Globals and APIs

The existing global instances remain single-owner definitions in `app_runtime.c`, so link-time ownership did not change. `sys_event_group`, `sys_state_mutex`, `change_pin_mutex`, `lcd_task_handle`, and `sys_state` are explicitly declared through `app_runtime.h` for the startup coordinator. The private `nvs_initialized` flag is not exported; `app_init.c` calls the existing `nvs_is_initialized()` API. New public interfaces are `app_init()` and the `app_runtime.h` lifecycle declarations, including `APP_EVENT_ADC_READY`.

## 3. Architecture and initialization order

The resulting dependency flow is:

```text
main.c
  └── app_init()
        ├── system_events_init()
        ├── event_dispatcher_init()
        ├── create event group and synchronization handles
        ├── lcd_writer_init()
        ├── nvs_init() → system_diagnostics_init() → init_system_state()
        ├── init_menu_system() → security_init() → fault_log_init()
        ├── app_services_init() → existing Wi-Fi/OTA/server coordination
        ├── cloud_reporting_init()
        ├── init_hardware() → restore_from_deep_sleep()
        ├── start ADC, LCD, event, buzzer, LED, monitor, logger, and protection tasks
        ├── app_buttons_init()
        ├── wait for APP_EVENT_ADC_READY
        ├── post_run_all() or terminal ADC-timeout fault path
        ├── ota_service_validate_running_app()
        ├── lcd_startup_release() and lcd watchdog initialization
        └── foreground update_lcd_activity_state()/handle_menu_timeout() loop

app_runtime.c
  └── existing runtime implementation and single-owner shared state
        ├── inverter and relay control
        ├── ADC telemetry and protection producers
        ├── settings/NVS compatibility APIs
        ├── LCD/menu/value-edit compatibility APIs
        └── deep sleep, diagnostics, error, and utility helpers
```

The original startup order was preserved. The refactor only changed which translation unit contains the orchestration code. The ADC timeout branch still marks the POST as failed, records `POST_FAILURE_ADC`, inhibits inverter output, displays `SENSOR STARTUP` / `ADC TIMEOUT`, and prevents a failed image from being marked healthy for OTA rollback purposes.

## 4. Build and test verification

| Check | Result |
| --- | --- |
| `pio run -e esp32dev` | **Passed**; firmware linked successfully. Reported RAM usage: 63,712 / 327,680 bytes (19.4%). Flash usage: 1,405,968 / 1,572,864 bytes (89.4%). |
| `pio run -e esp32dev-ui-mock` | **Passed**; firmware linked successfully. Reported RAM usage: 63,712 / 327,680 bytes (19.4%). Flash usage: 1,405,976 / 1,572,864 bytes (89.4%). |
| `python3 tools/test_firmware_contracts.py` | **Passed; 9 tests**. |
| `pio test -e esp32dev` | No test suites were discovered in `test/`; PlatformIO reported “Nothing to build.” The host-side contract suite above was run successfully. |
| `git diff --check` | Passed with no whitespace errors. |

No hardware was connected or exercised in the sandbox. Build success therefore verifies compilation and linking only; it does not verify electrical behavior.

## 5. Risk assessment

Physical ESP32/inverter testing remains necessary for GPIO relay behavior, LCD operation in both supported 16×2 and 20×4 configurations, button interrupts and debouncing, inverter startup/shutdown sequencing, protection thresholds, fan tachometer behavior, ADC calibration, deep-sleep wake sources, Wi-Fi reconnection, OTA rollback, MQTT and HTTP/WebSocket services, and FreeRTOS scheduling under real load. The refactor specifically preserved these paths rather than claiming they were hardware-validated.

The principal remaining architectural limitation is that `app_runtime.c` is a compatibility-oriented transitional module rather than a fully domain-split runtime. It is intentionally retained as one implementation unit for this safe pass because the repository currently has many direct references to shared state and legacy APIs. Further extraction should proceed incrementally by ownership—starting with settings/storage, ADC/telemetry, inverter control, and UI—using a build after each extraction and replacing direct shared-state access with narrowly scoped APIs.

## 6. Behavioral changes

**No intentional behavioral changes were made.** The only functional-equivalent interface adjustment is that the startup coordinator queries NVS readiness through the existing `nvs_is_initialized()` function instead of accessing the runtime module’s private `nvs_initialized` variable. A duplicate, unused POST display helper was removed from the runtime copy after the authoritative startup-owned copy was moved to `app_init.c`. No GPIO assignments, protocol endpoints, NVS keys, MQTT topics, task priorities, initialization order, LCD selection mechanism, protection thresholds, or security behavior were intentionally changed.

## References

[1]: https://github.com/Jezzylinks/inverter "Jezzylinks/inverter repository"
[2]: https://github.com/Jezzylinks/inverter/blob/master/src/main.c "Refactored entry point"
[3]: https://github.com/Jezzylinks/inverter/blob/master/src/app_init.c "Application initialization coordinator"
[4]: https://github.com/Jezzylinks/inverter/blob/master/src/app_runtime.c "Runtime implementation"
[5]: https://github.com/Jezzylinks/inverter/blob/master/tools/test_firmware_contracts.py "Firmware contract tests"
