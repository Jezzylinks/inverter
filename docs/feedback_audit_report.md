# Firmware-Wide Audible and Status-LED Feedback Audit

**Repository:** `Jezzylinks/inverter`  
**Target:** ESP32 firmware  
**Audit scope:** Existing buzzer and status-LED architecture, button and menu feedback, editable-setting boundaries, startup and POST, inverter state transitions, Wi-Fi, warnings, faults, security, and service-facing errors.

## A. Files Modified

| File | Change |
|---|---|
| `src/events/event_dispatcher.c` | Added LED and fault-log subscribers to all known protection routes. Added LED delivery to central button-event routing. |
| `src/main.c` | Added buzzer and LED feedback for POST success/failure. Kept system readiness false until the complete startup-health decision is available. |
| `src/app_input.c` | Marked normal select, boolean, and list edits as changed so they use the existing persistence path. Kept debounce, repeat, long-press, multi-click, and ISR behavior unchanged. |
| `src/app_runtime.c` | Made edit-mode exit return persistence status. Restores the previous value and shows save-failure feedback when persistence fails. Corrected fault paths that emitted a success tone for an error. |
| `src/wifi/wifi_events.c` | Added edge-triggered Wi-Fi success/error events for connected, AP-active, failed, and disconnected transitions. |
| `docs/feedback_audit_report.md` | Added this report. |

No second buzzer driver or second LED driver was introduced.

## B. Existing Feedback Found

The firmware already has a centralized event architecture. Classified button actions are converted into system events and dispatched to LCD, buzzer, LED, logging, and other subscribers. The buzzer event task owns normal buzzer playback, and the LED event task owns event-based LED patterns.

The existing buzzer provides click, limit, success, error, on, off, warning, derate, shutdown, recovered, and critical patterns. Numeric edit limits already posted a dedicated limit event for attempts beyond the configured minimum or maximum. The existing implementation also supported held and repeated Up/Down limit attempts because those gestures use the same numeric adjustment functions.

The existing LED driver provides status and error channels, direct states, blink, pulse, fade, and event-task patterns. It already contained button, system, protection, Wi-Fi, startup, success, error, shutdown, recovered, and critical pattern definitions. Several of those paths were unreachable because the dispatcher did not route events to the LED subscriber.

The existing button architecture remains intact. GPIO input is sampled and classified by the button task, including debounce, repeat, long press, and multi-click behavior. Feedback is attached after classification through the event dispatcher rather than being added to GPIO interrupt handlers.

## C. New Feedback Points

| Event | Buzzer | LED | Location |
|---|---|---|---|
| POST succeeds | Existing success pattern | Existing success blink | `src/main.c`, after `post_run_all()` |
| POST fails or ADC/LCD startup prerequisite fails | Existing error pattern | Existing error blink | `src/main.c`, startup failure branch |
| Protection warning | Existing warning pattern | Existing warning blink now routed | `src/events/event_dispatcher.c` |
| Protection derate | Existing derate pattern | Existing derate blink now routed | `src/events/event_dispatcher.c` |
| Protection shutdown | Existing shutdown/critical pattern | Existing shutdown/critical pattern now routed | `src/events/event_dispatcher.c` |
| Protection recovery | Existing recovered pattern | Existing recovery pattern now routed | `src/events/event_dispatcher.c` |
| Classified button press | Existing click pattern | Existing short status flash now routed | `src/events/event_dispatcher.c` |
| Wi-Fi connected or AP active | Existing success pattern | Existing success blink | `src/wifi/wifi_events.c` |
| Wi-Fi failed or disconnected | Existing error pattern | Existing error blink | `src/wifi/wifi_events.c` |
| Fault/error condition | Existing error pattern | Existing system error route | `src/app_runtime.c` |
| Setting save failure | Existing error pattern | Existing error route | `src/app_runtime.c` |

Wi-Fi feedback is edge-triggered. Repeated publication of the same state does not create another event. Connecting and reconnecting states remain silent to avoid buzzer activity during background retries.

## D. Boundary Coverage

The audit found the following active editable settings and existing boundary definitions:

| Setting | Minimum | Maximum | Boundary behavior |
|---|---:|---:|---|
| Voltage threshold | 100 V | 240 V | Clamped by numeric adjustment; limit event on rejected attempt |
| Current limit | 1 A | 50 A | Clamped by numeric adjustment; limit event on rejected attempt |
| Temperature limit | 40 °C | 85 °C | Clamped by numeric adjustment; limit event on rejected attempt |
| System timeout | 5,000 ms | 300,000 ms in editor metadata | Clamped by numeric adjustment; limit event on rejected attempt |
| Scroll speed | 1 | 10 | Clamped by numeric adjustment; limit event on rejected attempt |
| Quiet start | 0 h | 23 h | Bounded time editor |
| Quiet end | 0 h | 23 h | Bounded time editor |
| UTC offset | −12 h | +14 h | Bounded numeric editor |
| Set hour | 0 h | 23 h | Bounded numeric editor |
| Set minute | 0 min | 59 min | Bounded numeric editor |
| Battery type | 0 | 5 | Existing intentional wraparound list behavior |
| Voltage system | 0 | 2 | Existing intentional wraparound list behavior |

`voltage_thresh` is covered by the shared numeric adjustment path. Its maximum and minimum reject attempts post the existing limit buzzer event. The same path handles held and repeated Up/Down input without changing the button repeat implementation.

Normal-click select, boolean, and list edits previously changed the visible value without setting `value_changed`. They now mark the edit dirty and use the established confirmation and save path. This fixes persistence for Battery Type, Voltage System, Sound, Quiet Hours, and related boolean/select settings.

The audit also identified two pre-existing range inconsistencies that remain documented for follow-up: the Frequency Range editor and persisted validator use different ranges, and System Timeout has different editor and persisted upper bounds. These do not affect the buzzer or LED routing change, but they should be aligned in a subsequent settings-contract change.

## E. Startup Coverage

The startup sequence remains ordered as follows:

```text
BOOT
  → basic hardware initialization
  → ADC startup and valid ADC snapshot
  → LCD initialization and LCD event readiness
  → POST
  → startup health decision
  → normal operation
```

POST success now produces one success buzzer event and one success LED event. POST failure or an ADC/LCD prerequisite failure produces one error buzzer event and one error LED event. The existing LCD POST result and fault display remain unchanged.

The system-ready flag is now assigned from the complete startup-health result after NVS, LCD event readiness, POST completion, and POST pass status are known. This closes the earlier window in which a power-button action could observe `system_ready` before ADC/LCD/POST completion.

## F. Fault and Warning Coverage

Protection transitions are edge-triggered by the existing protection state machine. Stable warning, derate, shutdown, and recovered states do not continuously enqueue new alarms.

All known protection routes now deliver events to the LED subscriber as well as the existing LCD, buzzer, logger, relay, Wi-Fi, and fault-log subscribers where applicable. This makes the existing protection LED patterns reachable for temperature, battery voltage, AC voltage, and output-current conditions.

Known fault/error paths now use the existing error buzzer pattern rather than the success pattern. This includes recognized individual fault flags and combined or unknown fault masks.

The following pre-existing issues remain candidates for a later focused safety-feedback change: the critical buzzer mute policy during quiet hours, aggregate multi-fault recovery state, direct blocking LED calls in legacy fault handlers, and duplicate protection-shutdown/system-error alarm ownership.

## G. Duplicate Prevention

The audit searched for all buzzer and LED producers, event routes, button handlers, protection producers, startup results, and Wi-Fi state transitions. The implementation follows these duplicate-prevention rules:

1. Button feedback remains attached to the centralized classified button event. No buzzer or LED call was added to GPIO ISR code.
2. Wi-Fi feedback is posted only when the state changes and only for terminal/user-visible states.
3. Protection events retain the existing edge-triggered producer. LED delivery was added to the existing route rather than creating a second protection producer.
4. POST feedback is emitted once at the terminal startup result, after all prerequisite checks.
5. Setting boundaries continue to use the existing shared numeric adjustment and limit-event path.
6. Setting save failure is reported only after `save_settings()` returns failure; successful save feedback is not shown for failed persistence.

The LED subsystem still contains legacy direct calls that can compete with event-task patterns during inverter status, display activity, deep sleep, and older fault paths. These were documented rather than broadly rewritten because replacing them requires a hardware-state arbiter and could change existing product behavior.

## H. Build Verification

The following checks passed after the implementation:

| Check | Result |
|---|---|
| `python3 tools/test_firmware_contracts.py` | **24 tests passed** |
| `~/.local/bin/pio run -e esp32dev` | **Passed** |
| `git diff --check` | **Passed** |
| Flash image | Generated with `--flash_size 4MB` |
| ISR feedback audit | No buzzer or blocking LED operation added to GPIO ISR paths |

## I. Remaining Issues

The implementation does not invent hardware capabilities that are absent from the repository. The following items remain precise follow-up work:

- The LED driver does not yet implement a complete persistent base-state arbiter for all direct LED callers. A future change should make normal inverter state, warnings, faults, and temporary flashes explicit priority layers with guaranteed restoration.
- The critical buzzer policy during user mute and quiet hours requires a product decision. The existing behavior allows those settings to suppress critical tones.
- Some legacy direct `blink_led()`, `update_led()`, and `buzzer_off()` calls bypass the event tasks. They should be migrated only after a dedicated arbiter or acknowledgement API is defined.
- Frequency Range and System Timeout editor/persistence ranges should be unified.
- Network-service and OTA failures remain largely LCD/log status paths. Adding event feedback for every transient background failure would require rate limiting and a product-specific severity policy.
- Physical-device testing is still required for audible loudness, LED visibility, startup timing, simultaneous fault/recovery behavior, and sleep/restart feedback delivery.

## References

[1]: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/freertos.html "ESP-IDF FreeRTOS API Reference"
[2]: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/ledc.html "ESP-IDF LEDC Peripheral API"
