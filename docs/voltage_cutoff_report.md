# Battery Cutoff Voltage Correction Report

## Scope

The active battery profile stores voltages scaled to the selected battery system. The correction renamed the misleading fixed-12 V suffixes to `_v`, made the cutoff setter use the active profile, and preserved the existing 12 V, 24 V, and 48 V scaling behavior.

## Current behavior

`battery_generate_profile()` computes `voltage_multiplier = voltage_system / 12.0f` and regenerates the active profile from immutable chemistry defaults. Therefore a reference cutoff of 10.8 V becomes 10.8 V for a 12 V system, 21.6 V for a 24 V system, and 43.2 V for a 48 V system. `battery_monitoring_task()` compares the measured system/pack voltage against `battery_profile.cutoff_voltage_v`.

## Changes

The active scaled profile members were renamed from fixed-12 V identifiers, including `cutoff_voltage_12v`, `cutoff_voltage_min_12v`, `recharge_voltage_12v`, and the related charge/protection voltage fields, to generic `_v` names such as `cutoff_voltage_v`, `cutoff_voltage_min_v`, and `recharge_voltage_v`. The `_v` suffix means volts in the currently selected battery voltage domain. The internal-resistance field retains its `_12v` suffix because it is not an active voltage threshold.

`battery_monitor_set_cutoff()` now validates against the active profile’s voltage-domain-aware minimum, recharge voltage, and high-voltage ceiling. On success it updates the authoritative `battery_profile.cutoff_voltage_v` and synchronizes the existing compatibility fields `sys_state.battery_cutoff` and `sys_state.cutoff_voltage`. The UI, NVS binding, protection thresholds, ADC plausibility check, fault logging, and runtime monitoring now reference the generic active field.

The existing NVS key `bat_cutoff_volt` was preserved, so this C-identifier rename does not require an NVS migration. Valid persisted values remain readable under the same key and continue through existing profile validation.

## Validation

| Check | Result |
|---|---|
| Repository contract tests | **PASS — 25 tests passed** |
| PlatformIO `esp32dev` build | **PASS** |
| Firmware image generation | **PASS** |
| `git diff --check` | **PASS** |
| Old active cutoff identifiers | **No remaining references** |
| Physical voltage-domain test | **NOT RUN — no attached ESP32/battery system** |
| Reboot persistence test | **NOT RUN — hardware/runtime test unavailable** |

LCD, button, ADC sampling architecture, Wi-Fi, OTA, MQTT, NTP, watchdog, partition, and LCD I²C configuration were not intentionally modified.
