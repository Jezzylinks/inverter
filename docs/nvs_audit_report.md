# Repository-Wide NVS Audit and Refactor Report

**Repository:** `Jezzylinks/inverter`  
**Target:** ESP32, ESP-IDF through PlatformIO  
**Scope:** NVS initialization, namespaces, keys, persistence, recovery, concurrency, reset behavior, validation, partition layout, and build/test verification.

## Executive Summary

The principal reliability defect was architectural rather than a single failed `nvs_set_*()` call. NVS flash initialization and recovery were implemented independently in the application runtime, Wi-Fi storage, Wi-Fi security, and BLE provisioning paths. Persistent modules also opened NVS directly without a common transaction lock. This allowed duplicate lifecycle calls, inconsistent recovery behavior, and unsafe concurrent access.

The refactor adds one authoritative storage manager. It owns NVS initialization, documented recovery, readiness state, schema initialization, transaction serialization, diagnostics, namespace erasure, and explicit full-partition reset. Application modules now use manager-owned open/close operations. Duplicate subsystem initialization and duplicate recovery erases were removed.

A second defect was exposed by the new serialization model: the settings save path opened the system namespace and then attempted to open the battery namespace. The operation was changed so battery persistence completes before the system handle is opened. Fault-log dirty state is now cleared only after a successful commit.

The firmware build was attempted after installing PlatformIO. The final build result is recorded below after the build completes. Hardware reboot persistence, OTA compatibility, and flash-wear behavior require physical-device testing.

## Root Causes Found

| Root cause | Effect | Correction |
|---|---|---|
| Multiple modules called `nvs_flash_init()` and implemented their own recovery | Duplicate lifecycle ownership and inconsistent erase behavior | Added `storage_nvs_manager.c`; subsystem initializers now delegate to it |
| Direct NVS access had no application-wide transaction lock | Concurrent commits and inconsistent multi-key operations were possible | Manager holds a mutex from successful open through close or commit-close |
| `save_settings()` nested a battery NVS open while holding the system NVS lock | Deterministic deadlock after lock-based serialization | Battery save now completes before opening system settings |
| Legacy frequency/error-log paths ignored set and commit results | False persistence success | Set and commit results are checked and logged |
| Factory reset code erased and committed without checking results | UI could report success after storage failure | Erase and commit results are checked; full reset uses the manager |
| Fault-log dirty flag was cleared before persistence completed | Failed flushes were not retried | Dirty state is cleared only after a successful commit |
| Generated build metadata recorded `2MB` while checked-in configuration specifies `4MB` | A stale flash command could be unsafe | Checked-in configuration was preserved; generated artifacts must be regenerated before flashing |

## Final NVS Architecture

`include/storage/nvs_manager.h` and `src/storage/nvs_manager.c` are the authoritative storage boundary. `storage_nvs_init()` is idempotent and is called from the compatibility entry point `nvs_init()` and from manager-backed storage operations.

The manager tracks these states:

| State | Meaning |
|---|---|
| `UNINITIALIZED` | No initialization attempt has completed |
| `READY` | The default NVS partition initialized normally |
| `RECOVERED` | Initialization returned `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND`; the default NVS partition was erased and initialized again |
| `FAILED` | Initialization, recovery, or schema-marker setup failed |

The manager logs the exact ESP-IDF error name and code. It does not log credentials, PIN data, hashes, salts, or certificate contents.

Successful `storage_nvs_open()` calls hold the storage mutex until `storage_nvs_close()` or `storage_nvs_commit_close()`. This serializes complete module transactions and prevents simultaneous commits. The design requires every successful manager open to reach one of those close functions.

## Initialization and Recovery

The boot sequence initializes NVS before system-state loading, security initialization, diagnostics, fault-log restoration, or network services. `main.c` retains the existing `nvs_init(false)` compatibility call, but the manager is now authoritative and idempotent.

Normal startup does not erase NVS. Erasure occurs only for the documented recoverable initialization errors handled by `storage_nvs_init()`, or through the explicit `storage_nvs_factory_reset()` path. The recovery path logs that user NVS data was lost and exposes the recovered state and recovery count through the manager API.

The manager creates or reads `inv_sys_v2/schema_ver`. Missing schema data is initialized to version `1`. An existing different schema version is preserved and reported for future migration rather than being erased.

## Partition Verification

The checked-in partition table is `partitions.csv`, selected by `platformio.ini` and by `sdkconfig.defaults`. It uses the default NVS partition at offset `0x9000` with size `0x5000` bytes. The table contains OTA data, PHY data, two OTA application slots, and SPIFFS.

| Partition | Offset | Size | End |
|---|---:|---:|---:|
| NVS | `0x9000` | `0x5000` | `0xE000` |
| OTA data | `0xE000` | `0x2000` | `0x10000` |
| PHY | `0x10000` | `0x1000` | `0x11000` |
| OTA slot 0 | `0x20000` | `0x180000` | `0x1A0000` |
| OTA slot 1 | `0x1A0000` | `0x180000` | `0x320000` |
| SPIFFS | `0x320000` | `0xD0000` | `0x3F0000` |

The layout does not overlap the OTA slots or SPIFFS and fits within a 4 MiB flash device. The tracked generated file `build/flasher_args.json` still contains a stale `2MB` flash-size value. It is a generated artifact and must not be used for flashing; a clean PlatformIO build regenerates it.

The NVS partition is relatively small for the number of stored namespaces, credentials, certificates, MQTT strings, diagnostics, fault-log blobs, and repeated configuration writes. The manager exposes `nvs_get_stats()` through `storage_nvs_get_stats()`, but capacity and wear require long-running hardware tests.

## Namespace and Key Inventory

| Namespace | Keys or record | Purpose |
|---|---|---|
| `inv_sys_v2` | Settings keys, `schema_ver`, transaction metadata, `adc_cal`, frequency, legacy error keys | Main inverter settings, calibration, schema marker, and compatibility data |
| `inv_sys_v2` | `sec_pin_hash`, `sec_pin_salt`, `sec_force_chg` | Security PIN hash, salt, and forced-change state |
| `wifi` | SSID, password, mode, reconnect, DHCP, IP/DNS, AP settings, hostname | Runtime Wi-Fi configuration |
| `wifi_sec` | Root CA, client certificate, private key and related security records | Wi-Fi security material |
| `battery` | `state` | Versioned learned battery state and health data |
| `fault_log` | `entries_v1` | CRC-protected fault-log ring-buffer blob |
| `diagnostics` | `snapshot_v1` | Versioned diagnostics snapshot |
| `monitor_stats` | Monitor statistics record | Runtime monitor statistics |
| `cloud_rpt` | `config` | Cloud reporting configuration |
| MQTT | MQTT configuration keys under the system namespace | Broker, client, topic, keepalive, QoS, and enable state |

Existing namespace and key names were preserved to avoid breaking deployed devices. Literal namespace/key ownership remains distributed in older modules for compatibility, but all handle lifecycle operations now pass through the manager.

## Persistence and Validation

Writes continue to use the established pattern of open, set, commit, and close. Every modified persistence path checks the result of the set or erase operation and the commit. Important records retain read-back validation where already present, including version and size checks for battery and fault-log blobs.

Battery data validates version, finite floating-point values, SOC/SOH ranges, capacity ranges, and chemistry bounds. Wi-Fi configuration validates enum ranges, string termination, channel limits, reconnect intervals, and password constraints. Fault-log blobs include a versioned record and CRC.

Reads distinguish missing keys from other failures in the audited paths. Missing values use documented defaults or an uninitialized state. Other failures are returned or logged instead of being silently treated as valid defaults. Further module-by-module conversion of legacy boolean APIs is still recommended because those APIs cannot express the difference between missing data and storage failure to their callers.

## Factory Reset

`storage_nvs_factory_reset()` explicitly deinitializes, erases, and reinitializes the complete default NVS partition. It is used only by the authenticated full factory-reset path.

Namespace-specific reset paths use the least destructive operation. Wi-Fi reset erases only the Wi-Fi namespace. Settings reset targets `inv_sys_v2`, and log reset targets `fault_log`. All erase and commit results are checked. A future hardening step should separate security keys from the overloaded `inv_sys_v2` namespace so a settings-only reset cannot remove the PIN.

## Flash Wear

The manager avoids duplicate initialization and serializes transactions, but it does not yet provide a dirty-cache or write-coalescing layer for every configuration module. Fault-log flushing now preserves the dirty flag after failures. Repeated-save and long-duration wear tests remain hardware work.

## Testing and Verification

| Test or check | Result |
|---|---|
| Recursive NVS API inventory | Completed across `src`, `include`, build configuration, and partition files |
| Direct flash lifecycle bypass audit | No remaining direct application `nvs_flash_init`, `nvs_flash_erase`, `nvs_open`, or `nvs_close` calls outside the manager after the final cleanup |
| `git diff --check` | Passed |
| Existing Python contract suite | 24 passed; all contract tests successful after the LCD progress-block correction |
| `pio run -e esp32dev` before PlatformIO installation | Could not run because `pio` was absent |
| `pio run -e esp32dev` after PlatformIO installation | Attempted; final result is reported in the completion response |
| First boot, reboot persistence, unrelated namespace isolation | Not executable without ESP32 hardware or an NVS host/emulation harness |
| OTA persistence | Not verified on hardware |

The LCD progress-bar contract was corrected after CI reported that character slot 2 did not contain the required solid progress block. The full contract suite now passes.

## Remaining Risks and Required Hardware Tests

The following items cannot be proven by source inspection alone:

1. Save a Wi-Fi, security, battery, and system configuration on an ESP32, reboot, and verify each value survives.
2. Modify one namespace and confirm unrelated namespaces remain unchanged.
3. Inject missing, truncated, wrong-size, and invalid blobs and confirm safe recovery.
4. Fill the NVS partition and verify explicit `ESP_ERR_NVS_NOT_ENOUGH_SPACE` reporting without unintended full-partition erasure.
5. Perform an OTA update and verify configuration compatibility.
6. Exercise authenticated settings reset, log reset, and full factory reset, including restart and PIN behavior.
7. Measure repeated-save frequency and fault-log write rate for flash-wear risk.
8. Regenerate the PlatformIO build from a clean directory and verify the flash command reports `4MB`, not the stale generated `2MB` value.

## Files Changed

The refactor adds `include/storage/nvs_manager.h`, `src/storage/nvs_manager.c`, and updates the NVS call sites in the application runtime, Wi-Fi, security, battery, MQTT, cloud, diagnostics, events, services, and factory-reset modules. No LCD, button, ADC, OTA protocol, or server behavior was intentionally changed.

## References

[1]: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/nvs_flash.html "ESP-IDF Non-Volatile Storage Flash API"
[2]: https://docs.platformio.org/en/latest/projectconf/section_env_board.html "PlatformIO Project Configuration"
