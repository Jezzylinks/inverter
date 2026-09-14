# Startup LCD Ownership Fix Report

## Root cause

The overwrite was caused by the ordering of the normal-screen transition in `src/main.c`. `app_main()` called `lcd_boot_complete()` immediately after starting background services. `lcd_boot_complete()` unconditionally set the shared render state to `LCD_SCREEN_MAIN`, while the startup sequence was still inside its visible-duration gate and before `lcd_startup_release()` was called. The LCD task correctly rendered the current shared state, so it rendered the normal page during startup. Subsequent startup-state updates changed the shared state back to a startup screen, producing the observed startup/main/startup/main visual fight.

This was not an I²C collision and was not fixed with a mutex or an arbitrary delay. The mutex protected the shared state, but the state transition itself was premature. The ADC update path also exposed the issue: its data refresh updates the main-screen model, and legacy `show_battery_voltage()` / `show_temperature()` helpers call `lcd_show_main()`. Normal menu, button, Wi-Fi, standby, OTA, and confirmation writers had the same architectural gap because they could change the screen before startup release.

## LCD ownership by state

| State | LCD owner and policy |
|---|---|
| STARTUP | `app_main()` publishes startup stages; `lcd_task` is the sole hardware renderer. Normal screen writers are rejected until release. |
| POST | Startup remains authoritative while ADC readiness and POST execute. POST status and permitted fault screens continue to render. |
| NORMAL | Ownership is released once, after the startup minimum and required health conditions; `lcd_boot_complete()` then selects `LCD_SCREEN_MAIN`. |
| MENU | Button/menu writers can select menu screens only after startup release. |
| FAULT | Fault writers remain permitted so existing protection and startup-fault behavior is preserved. |

The physical LCD driver, PCF8574 backpack, I²C bus, geometry, frequency, GPIO configuration, and initialization sequence were not changed.

## State transition

There is now one authoritative healthy-startup transition:

```text
POST and safety checks complete
        ↓
startup minimum visible duration elapses
        ↓
lcd_startup_release()
        ↓
lcd_boot_complete()  [only when startup_healthy]
        ↓
LCD_SCREEN_MAIN
```

The earlier `lcd_boot_complete()` call was removed. A failed or unhealthy startup does not enter the normal page.

## Files changed

| File | Change |
|---|---|
| `src/main.c` | Moved `lcd_boot_complete()` from immediately after service startup to after the authoritative startup-duration gate and `lcd_startup_release()`. The call is conditional on `startup_healthy`. |
| `src/lcd_writer.c` | Guarded normal screen transitions with `s_startup_released`, covering main, menu, value-edit, detail, Wi-Fi, confirmation, standby, factory-reset, and OTA screens. Fault and startup screen transitions remain available. |
| `src/app_input.c` | Prevented power, enter/menu, up, down, and back handlers from performing normal UI navigation while startup owns the LCD. Button tasks and safety event delivery remain alive. |
| `tools/test_firmware_contracts.py` | Added regression contracts for transition ordering, writer guards, and button-handler startup ownership. |
| `docs/startup_lcd_ownership_fix_report.md` | This engineering report. |

## Event and queue findings

The LCD event receiver drains the LCD subscriber queue and converts eligible events into flash notices. Its existing startup filtering suppresses non-critical startup notices while leaving system/protection consumers active. No global event queue flush, ADC suppression, POST suppression, watchdog change, or safety-event suppression was introduced.

The critical overwrite did not require a stale event: the direct `lcd_boot_complete()` state write was sufficient to make the LCD task render the main page prematurely. Normal writer guards additionally prevent queued or callback-driven normal screen changes from stealing the screen before release.

## Task findings

`lcd_task` is the single task that performs physical LCD drawing. Other tasks and callbacks publish to `sys_lcd` through writer functions or event/flash state. `app_main()` owns the startup milestone sequence. The event receiver does not write LCD hardware directly. Button handlers remain active but are prevented from changing normal UI screens during startup. Fault writers remain unblocked.

`menu_init()` initializes system/menu state and does not itself directly write the physical LCD. Menu rendering is now also protected at the writer boundary, so initialization or a callback cannot take startup ownership inadvertently.

## Validation

The host-side firmware contract suite passes: **33 tests passed**. `git diff --check` passes. Static inspection confirms that the only `lcd_boot_complete()` call occurs after `lcd_startup_release()` and under `if (startup_healthy)`.

A complete PlatformIO build could not be executed in this sandbox because the `pio` executable is unavailable (`bash: pio: command not found`). No physical ESP32, LCD, inverter power stage, or serial monitor is available here, so cold-boot, repeated-reboot, fast/slow startup, button, menu, and fault behavior require hardware validation after building with the project’s normal ESP-IDF/PlatformIO toolchain.

The source-level fix preserves ADC readiness, ADC → POST ordering, watchdog architecture, fault handling, button task availability, menu/settings behavior after release, and the existing LCD hardware configuration. 

## Recommended hardware verification

After building with PlatformIO, verify cold boot and repeated reboot while observing the LCD and serial log. Confirm that no `LCD_SCREEN_MAIN` render occurs before `lcd_startup_release()`, that slow ADC/POST startup remains on the startup display, that a healthy startup renders the complete 20×4 main page once after release, and that ENTER, BACK, UP, DOWN, long-press/repeat behavior, menu editing, and fault/protection screens remain functional.

## Conclusion

The startup/main overlap was caused by a premature state transition, not by insufficient delay or an LCD bus race. Startup now retains authoritative ownership until the required startup conditions and visible-duration gate complete, and normal UI entry points respect that ownership boundary.

## Files modified in this change

- `src/main.c`
- `src/lcd_writer.c`
- `src/app_input.c`
- `tools/test_firmware_contracts.py`
- `docs/startup_lcd_ownership_fix_report.md`

## Build status

**Not built in this sandbox:** PlatformIO is unavailable (`pio: command not found`).

## Test status

- Host contract tests: **PASS — 33 tests**.
- Diff whitespace check: **PASS**.
- Physical LCD cold boot/reboot/menu/button/fault tests: **not available in sandbox; requires hardware**.
- Complete firmware build: **blocked by missing PlatformIO executable**.
