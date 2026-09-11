# ESP32 Task Watchdog Audit and Refactor Report

**Repository:** `Jezzylinks/inverter`  
**Target:** ESP32 / ESP-IDF 5.4.1 through PlatformIO 6.8.1  
**Scope:** ESP-IDF Task Watchdog Timer (TWDT), application health monitoring, task lifetime, startup, shutdown, LCD watchdog integration, configuration, and validation.

## Executive Conclusion

The firmware had two watchdog concepts, but their ownership and state were not explicit. The central `task_watchdog` module maintained application heartbeats and usually subscribed tasks to the ESP-IDF TWDT. The LCD watchdog independently called ESP-IDF TWDT APIs for the same LCD task. This created duplicate registration, duplicate feeding, and inconsistent deletion paths.

The refactor makes `src/task_watchdog.c` the sole owner of ESP-IDF task-WDT registration, reset, and deletion. The LCD watchdog now owns only an LCD heartbeat counter and liveness check. The public health snapshot records whether a task is health-monitored, TWDT-subscribed, or both. A task cannot call the central TWDT feed successfully unless its subscription was verified.

The firmware builds successfully and all 24 repository contract tests pass. Physical-board runtime tests remain outstanding because no ESP32 board is attached to this environment.

## A. Root Causes and Fixes

| Problem | File and function | Cause | Effect | Fix |
|---|---|---|---|---|
| Duplicate LCD TWDT ownership | `src/lcd_watchdog.c`, `lcd_watchdog_init()`; `src/lcd_task.c`, `lcd_task()` | The LCD task registered through `task_watchdog_register()` and was later registered again by `lcd_watchdog_init()` from `app_main()`. | Duplicate `esp_task_wdt_add()` behavior depended on ESP-IDF error semantics and made cleanup ambiguous. | Removed all raw ESP-IDF TWDT calls from the LCD watchdog. LCD heartbeat initialization now occurs once from `lcd_task()` after central registration. |
| LCD feed hid TWDT errors | `src/lcd_watchdog.c`, `lcd_watchdog_feed()` | The function called `esp_task_wdt_reset()` directly and discarded its return value. | A missing subscription or unavailable TWDT could be mistaken for a successful LCD watchdog feed. | LCD feed is now heartbeat-only. The central watchdog feed performs the only TWDT reset and returns a checked boolean result. |
| Health registration was conflated with TWDT registration | `src/task_watchdog.c`, `task_watchdog_register()` | Any non-`ESP_OK` status led to an add attempt, and the health record was created even when TWDT registration failed. | A task could appear monitored in the application registry without actual ESP-IDF protection. | Registration now adds only after `ESP_ERR_NOT_FOUND`, treats `ESP_OK` as already subscribed, logs other status errors, and returns failure unless both TWDT subscription and health-record allocation succeed. |
| Feeds could occur without verified subscription | `src/task_watchdog.c`, `task_watchdog_feed()` | The function reset the TWDT unconditionally and suppressed some errors. | Unsubscribed tasks could call the API and errors could be hidden. | The feed checks the task record first. It calls `esp_task_wdt_reset()` only for a verified TWDT subscriber and returns `false` on failure. Normal successful feeds remain silent. |
| Invalid timeout detection | `src/app_runtime.c`, system fault polling | `esp_task_wdt_status()` was compared with `ESP_ERR_TIMEOUT`, although the ESP-IDF API reports subscription state rather than timeout state. | The test could not detect a TWDT timeout. | Replaced the comparison with `task_watchdog_all_healthy()` for application-level liveness polling. Actual TWDT timeout handling remains in the ESP-IDF TWDT path. |
| Raw duplicate registration helper | `src/app_runtime.c`, `register_task_to_wdt()` | A second raw `esp_task_wdt_add()` helper existed outside the watchdog abstraction. | Future callers could bypass central state and error handling. | Removed the unused helper. |
| Unchecked restart-path feed | `src/app_runtime.c`, `perform_system_restart()` | The restart path called `esp_task_wdt_reset()` directly. | The call had no explicit subscription contract or error handling. | Routed it through `task_watchdog_feed()`. |
| LCD watchdog initialized too late | `src/main.c`, `app_main()` | LCD heartbeat initialization occurred after readiness waiting, POST, and the visible-startup gate. | Early LCD task activity was outside the advertised LCD heartbeat lifecycle, and initialization reset live heartbeat state. | Initialization now occurs at the top of `lcd_task()` after central watchdog registration. |
| LCD checker was inert | `src/task_watchdog.c`, health supervisor | `lcd_watchdog_check()` had no production call site. | A documented LCD soft-stall detector never ran. | The independent health supervisor now invokes `lcd_watchdog_check()` periodically. |
| Watchdog initialization failure was not propagated | `src/app_runtime.c`, `init_watchdog()` | Initialization/reconfiguration errors were logged, but startup continued as if the requested policy existed. | The application could run with an unverified watchdog configuration. | `init_watchdog()` now returns `bool`; `app_main()` fails closed before startup feeds if configuration or app-task registration cannot be verified. |

## B. Final Watchdog State Model

The health snapshot now contains `registered`, `health_registered`, `twdt_subscribed`, and `mode` fields. The mode enumeration is explicit:

| Mode | Meaning | Feed API |
|---|---|---|
| `TASK_WATCHDOG_MODE_NONE` | No active record. | No feed is valid. |
| `TASK_WATCHDOG_MODE_TWDT` | Reserved state for a future TWDT-only record. | `task_watchdog_feed()` may feed TWDT; no health heartbeat is expected. |
| `TASK_WATCHDOG_MODE_HEALTH_ONLY` | Application heartbeat only; no ESP-IDF subscription. | `task_watchdog_health_feed()` only. |
| `TASK_WATCHDOG_MODE_TWDT_AND_HEALTH` | Verified ESP-IDF TWDT subscription and application heartbeat. | `task_watchdog_feed()` feeds both mechanisms. |

In the current implementation, normal `task_watchdog_register()` creates `TWDT_AND_HEALTH`, while `task_watchdog_register_health_only()` creates `HEALTH_ONLY`. A health-only task cannot accidentally call the ESP-IDF reset through the central feed API.

## C. Task Watchdog Map

The table below is derived from the repository's task creation and watchdog call sites. “Not registered” means intentionally outside both current watchdog registries, not an implicit TWDT subscription.

| Task | TWDT | Health | Feed method | Registration | Unregistration |
|---|---|---|---|---|---|
| `app_main` | Yes | Yes | `task_watchdog_feed()` | `init_watchdog()` then central register before first feed | Central unregister at normal exit; fatal startup path also unregisters |
| `lcd_task` | Yes | Yes | Central feed plus LCD heartbeat | At task entry; LCD heartbeat initialized immediately after | No normal permanent-task shutdown path; central owner must be used before deletion |
| `adc_task` | Yes | Yes | Central feed in acquisition loop | At task entry | Self-unregisters on terminal failure path |
| `watchdog_supervisor` | No | Yes | `task_watchdog_health_feed()` | At task entry | Long-lived task with no current stop API |
| `button_task` | Yes | Yes | Central feed in poll loop | At task entry | `button_controller_deinit()` unregisters before deletion |
| `event_dispatcher_task` | Yes | Yes | Central feed | At task entry | No visible normal deletion path |
| `buzzer_event_task` | Yes | Yes | Central feed | At task entry | No visible normal deletion path |
| `led_event_task` | Yes | Yes | Central feed | At task entry | No visible normal deletion path |
| `fault_log_event_task` | Yes | Yes | Central feed | At task entry | No visible normal deletion path |
| `monitor_event_task` | Yes | Yes | Central feed | At task entry | No visible normal deletion path |
| `protection_event_task` | Yes | Yes | Central feed | At task entry | No visible normal deletion path |
| `lcd_event_receiver_task` | Yes | Yes | Central feed | At task entry | Self-unregisters before self-delete |
| `ota_task` | Yes | Yes | Central feed through OTA download path | At task entry | Unregisters on observed terminal paths |
| `ota_auto_check_task` | Yes | Yes | Central feed around waits and operations | At task entry | Long-lived task; no visible stop API |
| `wifi_toggle_task` | Yes | Yes | Central feed around queue and Wi-Fi operations | At task entry | Long-lived task; no visible stop API |
| `display_timeout_task` | Yes | Yes | Central feed each one-second loop | At task entry | Long-lived task; no visible stop API |
| `power_task` | Yes | Yes | Central feed | At task entry | No visible normal deletion path |
| `diagnostic_update_task` | Yes | Yes | Central feed | At task entry | No visible normal deletion path |
| `thermal_monitoring_task` | Yes | Yes | Central feed | At task entry | No visible normal deletion path |
| `battery_monitoring_task` | Yes | Yes | Central feed | At task entry | No visible normal deletion path |
| `wifi_monitor_task` | No | No | Exit-only feed was removed from the valid coverage model; no registration | Never | Self-deletes without watchdog record |
| `wifi_reconnect_task` | No | No | None | Never | Self-deletes |
| `wifi_saved_callback_task` | No | No | None | Never | Self-deletes |
| `dns_server_task` | No | No | None | Never | Deletes on stop |
| `network_services_sync_task` | No | No | None | Never | Self-deletes |
| `cloud publish_task` | No | No | None | Never | Self-deletes |
| `wifi_scan_task` | No | No | None | Never | Self-deletes |
| `ota_manifest_check_task` | No | No | None | Never | Self-deletes |
| `app_wifi_operation_watch_task` | No | No | None found | Never | Long-lived; requires future explicit policy if it becomes safety-critical |

The unregistered network workers are intentionally not subscribed to the shared TWDT by this change. Their network operations must remain bounded by their own client/socket timeouts. Changing that policy would require a separate network-task lifecycle design rather than adding indiscriminate feeds.

## D. Startup Sequence

The final startup sequence is:

```text
ESP-IDF auto-initializes TWDT from build configuration
  → app_main configures/reconfigures the requested runtime policy
  → app_main verifies its TWDT subscription and creates its health record
  → basic synchronization, NVS, security, hardware, and LCD setup
  → ADC task creation
  → ADC initialization, channel setup, calibration, and valid/fresh samples
  → APP_EVENT_ADC_READY or APP_EVENT_ADC_FAILED
  → LCD controller/task creation and LCD readiness event
  → event consumers, buttons, and health supervisor
  → app_main waits for ADC/LCD readiness or failure
  → POST runs only after ADC_READY and LCD_READY
  → startup result is presented on the LCD
  → five-second minimum visible-startup gate completes
  → startup filter is released
  → normal operation
```

The watchdog refactor does not move POST ahead of ADC readiness. The ADC task remains responsible for publishing `APP_EVENT_ADC_READY` only after its valid sample path is established. The physical LCD remains configured at **25 kHz I²C**.

The app task is now registered with the application health registry before the first `task_watchdog_feed()` call. This removes the previous startup feed-before-health-registration gap.

## E. Blocking and Timeout Policy

The effective runtime ESP-IDF TWDT policy is configured by `init_watchdog()` as a 15-second timeout, all available idle cores, and the caller-selected panic behavior. The checked-in SDK defaults remain 5 seconds with automatic initialization and no `CONFIG_ESP_TASK_WDT_PANIC`; the application deliberately reconfigures the auto-initialized TWDT at runtime. This difference is documented rather than silently treated as equivalent.

The application health watchdog uses a 10-second stale threshold and a 5-second supervisor period. These values serve a different purpose from the ESP-IDF TWDT. The application layer reports heartbeat staleness; the ESP-IDF layer detects a task or idle-core failure at the system watchdog level.

The LCD heartbeat threshold is 5 seconds. It is not a per-task ESP-IDF TWDT timeout. The global TWDT remains owned and configured centrally.

Known bounded operations remain below the effective 15-second TWDT window, including the ADC startup deadline, LCD initialization path, fan POST timing, and the 10-second OTA manifest HTTP client timeout. Network operations remain the main residual timeout risk because TLS, socket, and chained service work can approach a watchdog boundary if future code removes existing bounds.

## F. API Changes

| API | Change |
|---|---|
| `task_watchdog_register()` | Now returns `bool` and records verified TWDT plus health state. |
| `task_watchdog_register_health_only()` | Now returns `bool` and records health-only state. |
| `task_watchdog_feed()` | Now returns `bool`; refuses to call `esp_task_wdt_reset()` unless subscription state is verified. |
| `task_watchdog_health_feed()` | Now returns whether a health record was updated. |
| `task_watchdog_snapshot_t` | Added `twdt_subscribed`, `health_registered`, and explicit `mode`. |
| `lcd_watchdog_init()` | No longer registers with ESP-IDF TWDT; initializes LCD heartbeat state only. |
| `lcd_watchdog_feed()` | No longer feeds ESP-IDF TWDT; increments only the LCD heartbeat. |
| `lcd_watchdog_deinit()` | No longer deletes the task from ESP-IDF TWDT; clears LCD heartbeat state. |
| `init_watchdog()` | Now returns `bool` and fails startup if configuration cannot be verified. |
| `register_task_to_wdt()` | Removed as an unused duplicate raw-registration helper. |

## G. Configuration and Files Modified

No SDK configuration values were changed. The repository currently has `CONFIG_ESP_TASK_WDT_EN=y`, `CONFIG_ESP_TASK_WDT_INIT=y`, a checked-in 5-second menuconfig timeout, and runtime reconfiguration to 15 seconds. The report records this intentional two-stage configuration for maintainers.

The modified files are:

| File | Reason |
|---|---|
| `include/system/task_watchdog.h` | Added explicit watchdog modes and state fields; changed registration/feed APIs to return verified status. |
| `src/task_watchdog.c` | Centralized subscription/reset/delete ownership, verified status handling, guarded feeds, health-state tracking, and LCD heartbeat supervision. |
| `include/lcd/lcd_watchdog.h` | Corrected the contract to describe heartbeat-only behavior rather than a false per-task TWDT timeout. |
| `src/lcd_watchdog.c` | Removed duplicate raw TWDT operations and retained only LCD heartbeat state/checking. |
| `src/lcd_task.c` | Initialized LCD heartbeat state after shared watchdog registration. |
| `src/main.c` | Registered `app_main` before early feeds, handled watchdog initialization failure, and removed late LCD watchdog initialization. |
| `include/app/app_runtime.h` | Changed `init_watchdog()` to return verification status. |
| `src/app_runtime.c` | Propagated watchdog initialization failure, replaced invalid timeout polling, routed restart feeding through the central API, and removed the duplicate raw registration helper. |
| `docs/watchdog_audit_report.md` | Added this audit, task map, architecture description, validation results, and remaining risks. |

The previously completed physical-LCD/startup changes were not altered by this watchdog refactor. In particular, the LCD remains at 25 kHz and the deterministic startup timing remains intact.

## H. Validation

| Validation | Result |
|---|---|
| `python3 tools/test_firmware_contracts.py` | **24 tests passed** |
| `~/.local/bin/pio run -e esp32dev` | **Passed** |
| Firmware image generation | **Passed for 4 MB flash** |
| `git diff --check` | **Passed** |
| Raw ESP-IDF TWDT audit | Registration/reset/delete calls are centralized in `src/task_watchdog.c`; runtime initialization remains intentionally in `init_watchdog()`. |
| LCD duplicate-registration audit | **Passed**; only `lcd_task()` initializes LCD heartbeat state. |
| LCD I²C configuration audit | **Passed**; `I2C_FREQ_HZ` remains 25,000 Hz. |
| Startup ordering audit | **Passed statically**; ADC readiness remains a prerequisite for POST. |

The available tests are static contract tests and a cross-compilation/build check. They do not execute on an attached ESP32.

## I. Runtime Test Plan and Current Status

| Test | Status | Evidence or limitation |
|---|---|---|
| Normal startup | Static pass | Build and source audit confirm ADC readiness gates POST; physical boot still required for runtime confirmation. |
| Normal TWDT feeding | Static pass | Central feed is subscription-gated and return-checked. Physical serial-log confirmation remains pending. |
| Health-only task | Static pass | Supervisor uses `task_watchdog_health_feed()` and cannot invoke central TWDT reset. |
| TWDT task | Static pass | Registered tasks use central feed after registration. |
| Registration failure | Static pass | Registration returns false unless TWDT subscription and health-record creation succeed. |
| Task deletion | Partial | Button, LCD event receiver, ADC, and OTA paths have explicit unregister paths; several permanent tasks lack a complete shutdown coordinator. |
| Long legitimate operation | Static risk review | Existing bounded operations fit the configured runtime window; physical stress testing is pending. |
| Genuine stall | Not executed | No artificial stall was left in production, and no attached board is available for a controlled test. |

## J. Remaining Risks

The watchdog supervisor is health-only. It can report stale records but cannot recover a stalled task and cannot detect its own stall through another supervisor. This is acceptable for diagnostics but is not an independent recovery mechanism.

Several long-lived tasks have no visible shutdown path. If the firmware later adds external deletion or restart of those tasks, the deletion path must call `task_watchdog_unregister_task(handle)` before `vTaskDelete(handle)`.

Some finite network and cloud workers are intentionally outside both watchdog layers. Their safety depends on bounded socket/client operations and module-level cancellation. A future policy change must not subscribe them without adding lifecycle and feed coverage.

The runtime policy intentionally overrides the checked-in menuconfig timeout and panic setting. Maintainers should keep the comments and generated SDK configuration aligned with the intended deployment policy during future ESP-IDF upgrades.

## References

[1]: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/wdts.html "ESP-IDF Watchdogs API Reference"

[2]: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/freertos.html "ESP-IDF FreeRTOS API Reference"


## K. Corrective Follow-up to Commit `8cdb33d`

A second source audit identified additional issues in the first refactor. These were corrected without introducing a second watchdog subsystem.

| Follow-up issue | Corrective change |
|---|---|
| Failed TWDT registration could be represented as a health-only record | TWDT registration now exits before health-record creation when subscription verification fails. Health-only registration is available only through its explicit API. |
| `esp_task_wdt_add()` success was treated as sufficient proof | Registration now checks status before add, calls add only for `ESP_ERR_NOT_FOUND`, then re-checks status and requires `ESP_OK`. |
| Registration and health-record allocation were not transactional | A failed record allocation returns failure; a newly added TWDT subscription is rolled back. No successful record is created for a failed TWDT request. |
| Task-handle reuse could invalidate stale cleanup | Each successful record receives a monotonically increasing generation. The generation-aware unregister API verifies both handle and generation before deletion and record removal. |
| Registry critical sections included stack inspection | `uxTaskGetStackHighWaterMark()` now runs outside the registry critical section; only state copies and updates are protected. |
| Diagnostic suppression state was unsynchronized | `s_last_feed_error_task` is read and updated under the same registry lock. Normal successful feeds remain silent and the first meaningful error per task is logged. |
| Supervisor did not explicitly handle health registration failure | The supervisor now checks health-only registration and terminates rather than operating without a health record. |
| TWDT-only records could be treated as stale health records | Supervisor and `task_watchdog_all_healthy()` now require `health_registered` before evaluating heartbeat age. |
| TWDT task registration callers ignored registration failure | Monitored task entry points now delete themselves immediately when central TWDT-plus-health registration fails; they do not continue under an assumed protection policy. |

The corrective implementation preserves the existing ADC-before-POST startup order, deterministic LCD startup timing, button behavior, network-task policy, and 25 kHz physical LCD I²C configuration.

### Corrective validation

| Check | Result |
|---|---|
| Contract tests | **PASS — 24 tests passed** |
| PlatformIO `esp32dev` build | **PASS** |
| Firmware image generation | **PASS** |
| `git diff --check` | **PASS** |
| Direct raw TWDT ownership audit | **PASS** — operational add/delete/reset/status calls are centralized in `src/task_watchdog.c`; runtime configuration remains in the application initialization boundary. |
| Health-only reset-path audit | **PASS** — supervisor uses health feed only; LCD heartbeat does not call TWDT reset. |
| Hardware runtime tests | **NOT VERIFIABLE** — no ESP32 board, LCD, ADC, Wi-Fi, or OTA target is attached. |


### Final ownership correction

The ESP-IDF calls `esp_task_wdt_init()`, `esp_task_wdt_reconfigure()`, `esp_task_wdt_status()`, `esp_task_wdt_add()`, `esp_task_wdt_delete()`, and `esp_task_wdt_reset()` are now operationally centralized in `src/task_watchdog.c`. `init_watchdog()` remains as the application compatibility boundary, but delegates to `task_watchdog_init()` and performs no direct ESP-IDF watchdog operation.

The generation-aware lifecycle API is `task_watchdog_unregister_task_generation(handle, generation)`. A record is removed only when the handle and generation both match. The unregistration path performs the state check before deletion and rechecks the generation before removing the record. A task-registration failure is handled at each monitored task entry point by terminating that task rather than continuing under an assumed TWDT policy.


### Final validation correction

The repository contract test `tools/test_firmware_contracts.py` was updated to assert the centralized `task_watchdog_init()` implementation rather than requiring raw reconfiguration calls in `src/app_runtime.c`. This preserves the contract's intent while matching the single-owner architecture.

The final corrective run produced **24 passing contract tests**, a successful `esp32dev` PlatformIO build, a generated 4 MB firmware image, and a clean `git diff --check`.


### Surgical follow-up

The external button-task deletion path now retains the task registration generation and calls `task_watchdog_unregister_task_generation()` before `vTaskDelete()`. This prevents a stale cleanup operation from removing a newer registration that happens to reuse the same FreeRTOS task handle. The lifecycle contract test was updated to assert this generation-aware cleanup.

Final surgical validation: **24 contract tests passed**, the `esp32dev` PlatformIO build passed, firmware image generation passed, and `git diff --check` passed. Physical runtime scheduling and handle-reuse tests remain not verifiable without an attached ESP32.


## Final reliability audit follow-up

### Baseline and existing protections verified

The inspected repository was at commit `814d3c8`, which includes the prior watchdog corrections after baseline commit `8cdb33d`. Existing protections verified in source include post-add TWDT status verification, rollback on failed verification or registry allocation, generation-aware unregistration, synchronized feed-error suppression, stack-watermark collection outside the registry lock, health-only supervisor filtering, and explicit app-main initialization failure handling.

### Remaining defect corrected

`record_health_registration()` previously reused an existing record and overwrote its mode. That allowed a duplicate or conflicting registration request to silently replace a task’s established watchdog state. It now rejects any registration when the current task already has a record. A TWDT request made by a health-only task is therefore explicitly established and verified before the duplicate check, then rolled back if the record cannot be created; the existing health-only record is never overwritten. A health-only request made by a TWDT task is rejected without removing TWDT protection. Generation zero is skipped on counter wrap.

### Reliability audit findings

The startup path waits with a bounded 10-second loop for `APP_EVENT_ADC_READY`/`APP_EVENT_ADC_FAILED` and LCD readiness/failure, feeds app-main only after its verified registration, and runs POST only when both ADC and LCD readiness bits are set. The ADC manager defines readiness only after driver initialization, channel configuration, calibration, and a fresh valid required measurement; failure publishes `APP_EVENT_ADC_FAILED`. NVS operations use the centralized storage manager for initialization/open/close, and persistence modules check set/commit results before reporting success. Network services remain optional and outside the shared TWDT by policy. No concrete LCD, button, ADC sampling, Wi-Fi, OTA, MQTT, NTP, partition, or configuration defect was found that justified a change.

### Validation

The duplicate-registration contract is covered by the repository test suite. Final static and build results are recorded below after execution. Physical reboot persistence, task-handle reuse under real scheduling, ADC/LCD hardware startup, and network restart tests require hardware and were not claimed as executed.


## L. Intentional waiting-task classification update

The repository-wide audit was repeated against the current task bodies. The following workers were removed from ESP-IDF TWDT because their normal execution model includes intentional waiting or potentially long network service operations:

| Task | Normal behavior | Long blocking/delay? | Safety critical? | TWDT | Health-only | Reason |
|---|---|---:|---:|---:|---:|---|
| `app_wifi_toggle_task` | Waits for a Wi-Fi toggle request, then starts/stops the network stack and services | Queue wait plus network/service operations | No | No | No | A service worker may legitimately wait for a request and its network transition is not a continuously executing safety loop. Existing Wi-Fi architecture intentionally keeps these service tasks outside shared TWDT. |
| `ota_auto_check_task` | Initial 30-second delay, then periodic OTA checks every `APP_OTA_CHECK_INTERVAL_MS` (six hours) | Yes; periodic sleeps exceed the 15-second TWDT window | No | No | No | Automatic update polling is a long-period network helper. Feeding during one-second delay slices would only disguise the intentional sleep and would not provide meaningful liveness monitoring. |

The following classes remain TWDT protected because they perform safety/control/event work and either use bounded waits or continue executing meaningful work within the configured window: `adc_task`, `button_task`, the event dispatcher and safety/event consumers, `buzzer_event_task`, `led_event_task`, `power_task`, `diagnostic_update_task`, `thermal_monitoring_task`, `battery_monitoring_task`, and `display_timeout_task`. `app_main`, `lcd_task`, and active `ota_task` are intentionally unmonitored because their execution includes startup, mutex, or network/flash operations that may exceed the TWDT window. The watchdog supervisor remains health-only. Short-lived Wi-Fi scan, network synchronization, manifest-check, and other network helper tasks remain unmonitored.

No TWDT timeout, startup ordering, LCD behavior, button timing, queue size, task priority, stack size, or core affinity was changed. No direct ESP-IDF TWDT API was added outside `src/task_watchdog.c`.

The regression suite now explicitly verifies that `app_wifi_toggle_task` and `ota_auto_check_task` contain neither central TWDT registration nor feeds, while preserving their queue/periodic-delay behavior.


## M. Validation for the intentional waiting-task update

| Check | Result |
|---|---|
| `python3 tools/test_firmware_contracts.py` | **PASS — 26 tests passed** |
| `git diff --check` | **PASS** |
| Direct ESP-IDF TWDT ownership scan | **PASS** for operational calls; only `src/task_watchdog.c` contains the APIs. An unrelated comment in `src/app_runtime.c` mentions one API name but does not call it. |
| `pio run -e esp32dev` | **NOT RUN — PlatformIO is not installed in this sandbox (`pio: command not found`)** |
| Native CMake/Ninja fallback | **NOT RUN — required build artifacts/toolchain are unavailable** |
| Hardware runtime validation | **NOT VERIFIABLE — no ESP32 board is attached** |

The earlier report entries documenting a successful build refer to the prior repository state and are retained as historical validation. The current watchdog-classification diff has been validated by the host contract suite and static source checks, but no new compilation claim is made for this environment.


## N. Deferred NVS recovery persistence

The startup log identified `main`/`app_main` as the overdue TWDT subscriber while the invalid settings transaction was being repaired. The synchronous recovery path previously called `save_settings()` from `load_settings()`. That path performs bulk NVS writes and a flash commit; a single storage operation can block longer than the effective 15-second TWDT window, so feeds before or after the call cannot guarantee protection.

Settings recovery now remains synchronous only for RAM state: settings are loaded, transaction metadata and ranges are validated, safe defaults are applied, and battery/protection thresholds are synchronized. When corrections are required, `load_settings()` sets a pending flag and returns without persisting to flash. `app_main()` starts the one-shot persistence worker only after the ADC/LCD prerequisite decision and POST processing have completed. This preserves safe runtime defaults and startup safety gates while moving potentially long flash persistence out of the subscribed startup task.

The `settings_persistence_task` is deliberately ordinary/unmonitored. Its normal operation is a potentially blocking NVS/flash transaction and it has no continuously executing safety role. An atomic pending/active state prevents duplicate scheduling. Creation failure and save failure are logged; successful completion is logged explicitly. No TWDT timeout or direct ESP-IDF TWDT ownership was changed.

The regression contracts verify that invalid recovery schedules rather than calls `save_settings()` synchronously, that the worker is not TWDT-subscribed, that duplicate scheduling is guarded atomically, and that the worker starts after the POST branch. The current environment supports the host contract tests and static checks; PlatformIO and hardware runtime validation remain environment-dependent.


## O. Long-blocking task classification update

The active OTA worker, LCD task, and `app_main` are now intentionally excluded from central TWDT subscription. `ota_task` can remain inside HTTPS manifest retrieval or firmware download operations for longer than the 15-second TWDT interval. `lcd_task` can wait on `sys_state_mutex` with `portMAX_DELAY`, and its independent LCD heartbeat remains responsible for display health. `app_main` performs startup initialization, POST, and service coordination whose duration is not bounded by the task watchdog; NVS recovery persistence is separately deferred until after POST.

The central TWDT remains enabled for continuously executing safety, control, ADC, event, button, buzzer, LED, monitoring, and display-timeout tasks that have bounded waits or regular meaningful progress. No TWDT timeout was increased, and no direct ESP-IDF TWDT calls were added outside `src/task_watchdog.c`.

The host contract suite now verifies that `app_main`, `lcd_task`, and `ota_task` have no central TWDT registration/feeds, while the OTA worker remains protected by its existing cancellation, HTTP/download timeout, verification, and failure-state handling.


## P. Typed startup initializer results

Startup initialization APIs now return `esp_err_t` rather than silently discarding status. This includes NVS, system diagnostics, system state, menu state, hardware, wake-state restoration, LCD power, and LCD writer initialization. `app_main()` captures each result and handles failures explicitly: fatal prerequisites keep the system unavailable and return, while recoverable diagnostics/hardware warnings are logged and startup safety gates remain authoritative. Contract tests verify the typed declarations and corresponding `ESP_OK` checks.


## Q. Initializer implementation status propagation

The typed initializer refactor now propagates internal operation results rather than returning unconditional success. Hardware initialization returns fan and buzzer errors; LCD power initialization checks GPIO, LEDC timer, and LEDC channel setup; LCD writer initialization rejects a missing system-state mutex; system diagnostics returns persistence failure; system state returns protection initialization failure; and NVS returns storage initialization failure. Recoverable settings correction remains explicitly successful because safe defaults are active in RAM and flash persistence is deferred by design.


## R. Explicit initializer return paths

The startup initializer APIs now have explicit internal status paths. `init_hardware()` initializes `init_err` to `ESP_OK`, returns fan failures immediately, records buzzer failures, reports display-timeout task creation failure as `ESP_ERR_NO_MEM`, and returns the accumulated result. `init_menu_system()` returns `ESP_ERR_INVALID_STATE` when settings recovery required validated defaults, while continuing safely with those defaults and deferred persistence. `restore_from_deep_sleep()` explicitly returns `ESP_OK` because its current implementation only reads/logs RTC state and has no fallible operation. `lcd_power_init()` returns each GPIO/LEDC setup error immediately or the final channel result. Tests assert these internal paths rather than only checking signatures.
