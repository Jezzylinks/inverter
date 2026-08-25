# LCD Startup Hardware-Stage Debugging Report

## Root Cause

The LCD was not stuck because `draw_startup_status()` was changing state or because the elapsed-time calculation had stopped. The actual failure was in the failed-POST propagation path.

`LCD_SCREEN_STARTUP_STATUS` advances from `LCD_STARTUP_STAGE_HARDWARE` only when both the stage duration has elapsed and `snap.startup_status.post_complete` is true. The LCD task reads that value from the shared `sys_lcd.startup_status` snapshot.

Before this fix, `post_show_result_and_notify()` updated the LCD startup status only when `result.all_passed` was true. When `post_run_all()` returned a failed result, the function displayed the POST fault screen but never called `lcd_show_startup_status(..., post_complete=true, ...)`. Consequently, `sys_lcd.startup_status.post_complete` remained false indefinitely. The hardware stage therefore satisfied neither its intended terminal-state propagation nor its transition gate.

The same propagation gap existed in the ADC/LCD prerequisite-failure path in `main.c`: it set `post_completed = true` for the application’s safety bookkeeping but did not publish a terminal startup status to the LCD state model before displaying the fault.

## Evidence and Value Flow

The relevant value flow is:

```text
post_run_all()
    |
    v
post_result_t result
    |
    v
post_show_result_and_notify(result)
    |
    v
lcd_show_startup_status(..., post_complete=true, ...)
    |
    v
sys_lcd.startup_status.post_complete
    |
    v
lcd_task() snapshot
    |
    v
draw_startup_status(&snap) and hardware-stage transition gate
```

The state structure is declared in [`include/lcd/lcd_state.h`][1]. The setter in [`src/lcd_writer.c`][2] copies the `post_complete` argument into `sys_lcd.startup_status.post_complete` while holding the LCD lock. The LCD task in [`src/lcd_task.c`][3] copies that state and computes:

```c
const bool post_ready = snap.startup_status.post_complete;
const bool can_advance = post_ready ||
                         (stage != LCD_STARTUP_STAGE_HARDWARE &&
                          stage != LCD_STARTUP_STAGE_SELF_CHECK);
```

For the hardware stage, `can_advance` reduces to `post_ready`. Rendering only reads this snapshot; it does not set `post_complete`, change the stage, initialize hardware, or wait on POST.

The corrected writer in [`src/main.c`][4] now publishes the terminal startup state before branching into pass or failure display handling:

```c
lcd_show_startup_status(LCD_STARTUP_STAGE_HARDWARE, true,
                        result.all_passed, result.lcd_ok,
                        result.adc_ok, result.fan_ok);
```

The function also logs the propagation event, making the runtime value flow observable:

```text
POST result propagated: complete=1 passed=... lcd=... adc=... fan=...
```

## State Machine

The startup sequence is:

```text
HARDWARE → POWER → NETWORK → SERVICES → SELF_CHECK → READY
```

The hardware and self-check stages require `post_complete == true` before advancing. The intermediate POWER, NETWORK, and SERVICES stages advance after their display duration. READY remains visible for its configured duration and then calls `lcd_boot_complete()`.

A failed POST is still a terminal POST result. It must set `post_complete=true` and `post_passed=false`; the failure flags remain available for rendering and safety decisions. This permits the UI state machine to leave the hardware screen while preserving the failed state, and the application’s `startup_healthy` calculation remains false because `startup_post.all_passed` is false.

## Root-Cause Fix

The smallest safe fix was to publish a terminal LCD startup status for both POST outcomes. Successful POST continues to publish `post_complete=true` and `post_passed=true`. Failed POST now publishes `post_complete=true` and `post_passed=false` before the existing `** POST FAILED **` screen is shown.

The ADC/LCD prerequisite-failure path now publishes the corresponding terminal status with the correct `lcd_ok` and `adc_ok` values before showing a specific initialization-failure or timeout message. `post_run_all()` is still called only after the ADC task has produced its first sample and the LCD task has reported readiness.

No transition condition was bypassed, and no hardware check was removed.

## Safety Assessment

The change preserves the required safety behavior. ADC readiness is still required before POST runs, LCD task readiness is still required before POST runs, and failed ADC/LCD prerequisites still call `inverter_emergency_shutdown()`. A failed POST remains visible through `post_passed=false`, `all_passed=false`, and the existing fault display. The application’s `startup_healthy` result remains false for any failed prerequisite or POST result, so OTA validity and normal startup health are not falsely reported.

The renderer remains separate from state transitions. `draw_startup_status()` only formats and draws the current snapshot; the producer functions are responsible for updating the state.

## Build and Regression Verification

The following checks passed after the fix:

| Check | Result |
|---|---|
| `python3 tools/test_firmware_contracts.py` | **Passed; 11 tests** |
| `pio run -e esp32dev` | **Passed** |
| `pio run -e esp32dev-ui-mock` | **Passed** |
| `git diff --check` | **Passed** |
| LCD/ADC readiness gate | **Static contract covered**; POST call is guarded by both readiness bits |
| 16×2 and 20×4 configurations | **Both represented by the two firmware build profiles and passed** |

The test suite now includes a regression assertion that `post_show_result_and_notify()` propagates `post_complete=true` for the terminal POST result and that the failure path retains the ADC/LCD safety actions.

## Runtime Verification

Physical hardware-in-the-loop testing was not available in the sandbox. No claim is made that the actual inverter board was tested. The next board-side verification should capture serial output through startup and confirm the following ordering:

```text
LCD task initialized and publishes LCD_READY
ADC task publishes ADC_READY after its first sample
post_run_all() starts
POST result propagated: complete=1 ...
LCD hardware stage advances after its configured duration
```

## References

[1]: https://github.com/Jezzylinks/inverter/blob/master/include/lcd/lcd_state.h "LCD startup state definition"

[2]: https://github.com/Jezzylinks/inverter/blob/master/src/lcd_writer.c "LCD startup status writer"

[3]: https://github.com/Jezzylinks/inverter/blob/master/src/lcd_task.c "LCD task and startup state machine"

[4]: https://github.com/Jezzylinks/inverter/blob/master/src/main.c "Application startup and POST propagation"
