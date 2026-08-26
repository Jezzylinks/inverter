# Button sound and buzzer integration report

## Root-cause conclusion

The repository contained two concrete software causes for missing button sound. First, `buzzer_init()` was implemented but had no boot call, so `s_buzzer_initialized` remained false. Every sound request therefore took the compatibility path that only delayed for the requested duration and never configured physical output. Second, the buzzer originally selected LEDC low-speed timer 0/channel 0 while the LED subsystem selected timer 0 and channels 1–2. Sharing a timer with a different frequency/resolution is unsafe because the timer configuration is common to its channels.

The button event path itself is independent of the buzzer. Button handlers call `post_button_click_event()`, which posts an `EVENT_CATEGORY_BUTTON` / `EVENT_ACTION_PRESSED` event to the central system queue. The dispatcher routes that event independently to the LCD and buzzer subscriber queues. The application callback does not wait for buzzer playback, and a buzzer queue failure cannot prevent the original button callback from returning.

No physical buzzer or board-level electrical failure is claimed. The hardware buzzer type cannot be conclusively identified from the source alone; the current implementation uses LEDC PWM, which is appropriate only if the fitted device is a passive piezo or otherwise requires a driven waveform. If the fitted part is an active buzzer, the PWM implementation should be verified against the hardware datasheet before interpreting an absent tone as a firmware failure.

## Final signal path

```text
Physical button
    -> GPIO input with pull-up and any-edge ISR
    -> shared button edge queue
    -> button_task debounce and classification
    -> app_buttons callback
    -> app_input handler
    -> post_button_click_event()
    -> central system event queue
    -> event_dispatcher_task
    -> EVENT_SUB_BUZZER subscriber queue
    -> buzzer_event_task
    -> non-blocking-in-input playback task
    -> LEDC PWM timer/channel
    -> GPIO13
    -> physical buzzer
```

The buzzer is a consumer of the event. It is not called from the GPIO ISR, does not own the button controller, and is not on the critical path of application input.

## Hardware ownership

| Resource | Owner | Configuration |
| --- | --- | --- |
| Button GPIOs 16, 19, 17, 5, 18 | `button_controller` | Input, pull-up, any edge, active-low |
| Buzzer GPIO13 | `buzzer.c` | LEDC PWM output |
| LEDC timer 0 / channels 1–2 | `led.c` | Status and error LEDs |
| LCD backlight timer 1 / channel 3 | `app_runtime.c` | Backlight PWM |
| Buzzer timer 2 / channel 0 | `buzzer.c` | 10-bit PWM, 1 kHz initial configuration |

The buzzer was moved to LEDC timer 2/channel 0. This prevents timer-configuration interference with the LED subsystem and the LCD backlight. `buzzer_init()` explicitly sets duty to zero and updates the channel before marking the buzzer initialized, guaranteeing an inactive startup output.

## Initialization and independence

`main.c` now initializes the event queues, initializes the LCD/backlight, calls `buzzer_init()`, and logs any buzzer failure as non-fatal. The buzzer event task is still created independently. If buzzer initialization fails, button initialization continues, application input continues, and inverter safety logic continues; only sound output is unavailable.

The central dispatcher uses zero-timeout sends for subscriber queues. A full buzzer queue is logged and counted as a dropped buzzer request; it does not block the button callback or the dispatcher task waiting for buzzer capacity. The application’s original system event remains independent of delivery to the buzzer subscriber.

## Playback behavior

The buzzer task performs all delays and PWM playback. Button callbacks only post the application event. Normal click and limit requests are delivered through the subscriber queue. Critical events preempt queued lower-priority buzzer indications and set a critical-active state that suppresses lower-priority sounds until recovery. Rapid ordinary requests are processed by the single buzzer task rather than by overlapping callers, so PWM ownership remains serialized.

The explicit pattern enumeration includes click, limit, success, error, critical, warning, derate, shutdown, recovered, on, and off. Existing sound patterns and boundary-tone calls remain in the application semantics; this change does not remove the Up/Down limit-tone calls or replace the button controller.

## Diagnostics and self-test

The buzzer diagnostic structure reports whether hardware initialization succeeded, whether sound is currently enabled, requests received, requests played, requests dropped, queue overflows, the last pattern, its timestamp, and the current pattern. `buzzer_get_diagnostic()` provides a read-only snapshot. `buzzer_self_test()` provides a callable short tone test without requiring a physical button event; it is not invoked during normal boot.

The button diagnostics added previously remain available through menuconfig and show whether an input signal reached the ISR, edge queue, classifier, callback, and application. Together, the two diagnostic layers distinguish these cases:

| Observation | Diagnosis |
| --- | --- |
| Button GPIO raw level changes but ISR count does not | Electrical/GPIO interrupt/module-function issue |
| ISR count rises but button events do not | Queue, task, or debounce issue |
| Button event and app callback appear but no buzzer request is counted | Dispatcher routing issue |
| Buzzer request is counted but dropped/overflow rises | Buzzer subscriber queue pressure |
| Buzzer request is played but no audible output occurs | PWM/GPIO wiring, active-vs-passive device, or hardware issue |

## Validation

The normal Continuous/default firmware and diagnostics-enabled firmware both build successfully. The contract suite verifies the buzzer initialization call, non-conflicting LEDC resource selection, zero-timeout event delivery, queue accounting, self-test API, and preservation of the button safety guard. Physical buzzer audibility, active-high/active-low electrical behavior, and the fitted device type require board testing and are not claimed here.
