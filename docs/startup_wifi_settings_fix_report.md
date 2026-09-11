# Startup, Wi-Fi, and Settings Reliability Fix Report

**Repository:** `Jezzylinks/inverter`  
**Target:** ESP32 / ESP-IDF 5.2.1 / PlatformIO

## Root causes

The startup display problem was caused by the release of the existing LCD startup state machine being driven only by initialization and POST completion. On fast hardware, those prerequisites completed before a user could reliably observe the progress presentation. The existing implementation now uses a centralized presentation clock and a yielding minimum-duration gate after readiness and POST evaluation; this preserves the ADC-before-POST ordering and does not delay ADC acquisition or safety checks.

The Wi-Fi reboot was traced to lifecycle and concurrency hazards rather than an inherent Wi-Fi failure. Wi-Fi start/stop operations were able to overlap when a second panel request was accepted while the first request was still executing. ESP-IDF Wi-Fi start and stop are not re-entrant, and the overlapping transition could panic/assert and reset the ESP32. The current implementation serializes the operation worker and rejects a second transition while the first radio lifecycle operation is active. The controller and manager also have explicit initialized/started state transitions and rollback paths.

The settings save failure had two related storage issues. The application had multiple NVS lifecycle paths, and the settings load/save path could hold one NVS transaction while opening another namespace, creating a deadlock risk. The repository's current storage manager serializes NVS transactions, and the settings loader closes its system handle before loading the battery profile. In addition, the settings read helper treated every read failure as a missing key, so type mismatches and invalid-state errors could be silently converted into defaults. The final change propagates non-`ESP_ERR_NVS_NOT_FOUND` read errors and logs the key, error name, and code. The save path also now checks NVS initialization instead of ignoring its return value or attempting an implicit recovery erase.

A normal save failure does not call `esp_restart()`. The remaining restart paths are explicit factory-reset, OTA, button-initialization-failure, and user-requested system-restart paths.

## Files modified in this turn

| File | Change |
|---|---|
| `src/app_runtime.c` | Propagates non-missing NVS read errors during settings load; improves per-key diagnostics; corrects the save log tag; refuses to save when NVS initialization fails instead of ignoring the error; removes obsolete boolean read wrappers. |
| `docs/startup_wifi_settings_fix_report.md` | This engineering report. |

The repository already contained the broader architectural fixes needed for startup timing, Wi-Fi transition serialization, and centralized NVS lifecycle management. Those existing changes were audited rather than duplicated.

## Fixes implemented and audited

The NVS manager is the authoritative owner of flash initialization, documented recovery, transaction serialization, and commit/close handling. Settings writes use `NVS_READWRITE`, check each `nvs_set_*` result, check commit results, and close the handle on all error paths. Missing keys remain a supported first-boot/migration case; other read errors mark the load as recovered/defaulted and schedule deferred persistence.

The Wi-Fi toggle is handled by a dedicated worker rather than the input task. A single active operation flag prevents a second start/stop request from entering the ESP-IDF radio stack while the first operation is still in progress. Network service shutdown precedes radio stop, and ordinary toggle failures are reported to the UI without invoking a reboot.

The startup gate remains ordered as ADC readiness, LCD readiness, POST, safety result, visible startup minimum, and normal operation. The minimum-duration loop yields to FreeRTOS and does not disable or extend the task watchdog.

## Validation

The host-side firmware contract suite passes: **30 tests passed**. `git diff --check` passes. The complete PlatformIO build for environment `esp32dev` also passes, generating the 4 MB ESP32 image. Final reported usage is approximately **65,064 bytes RAM (19.9%)** and **1,429,372 bytes flash (90.9%)**.

## Remaining risks

No physical ESP32, LCD, inverter power stage, Wi-Fi environment, or serial monitor was available in the sandbox. Therefore, the following still require hardware verification: perceived LCD readability and contrast; Wi-Fi connection/provisioning and repeated enable/disable cycles; reset-reason capture under real radio load; NVS persistence across power loss; namespace isolation; flash-full behavior; and the absence of watchdog resets during real HTTP/MQTT/mDNS/NTP/WebSocket startup and teardown.

The repository's existing documentation also records that generated build artifacts must be regenerated from a clean build before flashing, and that flash wear from repeated settings saves requires long-duration testing.

## Forensic NVS review update

The settings namespace contains two historical representations of several battery values: the general settings table and the dedicated battery keys. The previous `save_settings()` implementation committed the dedicated battery keys first and then opened the same namespace again for the general settings and transaction marker. This was not atomic and could leave a partial configuration when a later write failed. The corrected implementation opens `inv_sys_v2` once, writes the battery keys, general settings, and transaction metadata, and commits once.

NVS key types are persistent. A key created by an older firmware with a different numeric type can return `ESP_ERR_NVS_TYPE_MISMATCH` when the new firmware writes it. The corrected save path migrates only the conflicting key by erasing that key in the current uncommitted transaction and retrying the validated current value. It never erases the namespace or the full NVS partition for this condition. The same compatibility behavior covers the dedicated battery keys.

The loader now logs transaction-marker read failures explicitly, while still treating `ESP_ERR_NVS_NOT_FOUND` as a normal first-boot case. All settings handles are closed before a nested battery read can occur, and every save failure path closes the active handle without committing a partial transaction.

## Conclusion

The source-level root causes are addressed without disabling watchdogs, removing safety checks, erasing NVS as a first response, or using reboot as ordinary error recovery. Hardware testing remains necessary before claiming field-level closure.
