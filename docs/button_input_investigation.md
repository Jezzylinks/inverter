# Physical button input investigation

## Scope

This investigation traces the five physical buttons from the hardware aliases through GPIO configuration, interrupt delivery, the shared FreeRTOS task, debounce/classification, callback dispatch, and application-level readiness guards. The ADC issue is analyzed separately; no evidence was found that the button path shares an ADC GPIO.

## Current GPIO map

| Button | GPIO | Configuration | Software owner |
| --- | ---: | --- | --- |
| Power | 16 | Input, pull-up, any edge, active-low | `button_controller` |
| Enter/Menu | 19 | Input, pull-up, any edge, active-low | `button_controller` |
| Up | 17 | Input, pull-up, any edge, active-low | `button_controller` |
| Down | 5 | Input, pull-up, any edge, active-low | `button_controller` |
| Back | 18 | Input, pull-up, any edge, active-low | `button_controller` |

Compile-time uniqueness assertions now prevent duplicate button aliases. A repository-wide source search found no second application owner for GPIOs 5, 16, 17, 18, or 19. GPIO5, GPIO16, GPIO17, GPIO18, and GPIO19 can have board-specific strapping, PSRAM, or SPI functions on some ESP32 module variants; the source tree does not establish whether the user’s physical board routes any of those alternate functions. That remains a hardware/schematic check, not a reason to change the requested assignments.

A separate, concrete configuration conflict was found: ADC1 channel 7 maps to GPIO35 for Battery Voltage, while `GPIO_FAN_TACH` is also GPIO35. This does not explain zero DMA frames, because frame production occurs before channel-value parsing, but it can compromise the battery ADC channel and fan tach measurement. The assignments were not changed because the requirements explicitly prohibit changing electrical mappings without a board-level replacement plan.

## Signal path

`app_buttons_init()` calls `button_controller_init()`, creates one controller for each binding, registers each non-null callback, and starts each controller. The controller configures the GPIO as an input with pull-up and `GPIO_INTR_ANYEDGE`, installs the shared ISR service, and adds one ISR handler per button GPIO. The ISR only reads the level and timestamp, increments an atomic ISR counter, and enqueues the edge. The shared `button_task` receives edges, periodically reconciles each GPIO level directly, waits for the configured 35 ms debounce interval, classifies press/release/click/long-press/repeat events, and invokes the registered callback outside the ISR.

The task is configured with a 4096-byte stack, priority 5, and a 10 ms polling interval. It feeds the existing watchdog at the top of every loop. The ISR queue has 32 entries and records queue-full events atomically. The implementation therefore has a recovery path when an edge is lost: direct GPIO reconciliation can still update the raw state on the next task cycle.

## Application readiness guard

All five app-input callbacks retain their existing `sys_state.system_ready` guard. The guard was not removed. In the current boot sequence, `init_menu_system()` sets `system_ready=true` after settings/profile/security initialization, before the ADC/LCD readiness wait and POST. If startup later calls `inverter_emergency_shutdown()`, it sets `system_ready=false`; subsequent button callbacks are intentionally ignored as part of the safety halt. This means a board that remains in the emergency startup fault state can appear to have dead buttons even when GPIO events are arriving. The diagnostic output now distinguishes this case by logging callback entry together with `ready=0`.

## Diagnostics added

Button diagnostics are opt-in through `CONFIG_INVERTER_BUTTON_DIAGNOSTICS`, disabled by default. When enabled through `pio run -e esp32dev -t menuconfig`, the shared task emits at most one diagnostic line per button per second with GPIO, raw level, stable level, pressed state, controller state, ISR calls, queue overflows, total events, and the last event timestamp. Each successful binding initialization also logs its initial GPIO level. Event dispatch and app-input callback entry are logged at the same opt-in level.

Representative output is:

```text
I (...) APP_BUTTONS: Power GPIO16 started: initial=1 active_low=1 pullup=1
I (...) BUTTON: DIAG Power gpio=16 raw=1 stable=1 pressed=0 state=IDLE isr=0 queue_full=0 events=0 last=NONE last_us=0
I (...) BUTTON: EVENT PRESS button=Power gpio=16 clicks=0 duration_ms=0
I (...) APP_INPUT: CALLBACK Power event=PRESS ready=1
```

The diagnostic interpretation is direct. If `raw` changes when a button is pressed but `isr` remains zero, the GPIO interrupt path or board electrical connection is suspect. If `isr` increases but `events` remains zero, the shared task, queue, or debounce path is suspect. If `EVENT` appears but `CALLBACK` does not, callback dispatch is suspect. If `CALLBACK ... ready=0` appears, application safety gating is intentionally rejecting the event. If callback logs show `ready=1` but the action does not occur, the problem is inside the specific app-input handler or downstream event path.

## Changes made

The button controller now records the last emitted event and timestamp, exposes a read-only diagnostic structure, and provides bounded opt-in logs. The app-input layer logs callback entry only when diagnostics are enabled. Hardware aliases now include compile-time uniqueness assertions. No GPIO assignment, active-low polarity, debounce interval, long-press behavior, repeat behavior, multi-click behavior, system-ready guard, or safety behavior was changed.

## Validation status

The firmware contract suite passed with 19 tests, and the Continuous/default firmware built successfully with ESP-IDF 5.3.0 through PlatformIO 6.8.1. No physical board or serial device is available in the sandbox, so GPIO levels, ISR counts, button electrical polarity, alternate-function conflicts on the user’s module, and real callback behavior still require board testing.
