# Startup Timing and Physical LCD Visibility Report

**Repository:** `Jezzylinks/inverter`  
**Target:** ESP32 inverter firmware  
**Objective:** Make the existing startup sequence deliberately visible on the physical LCD without delaying ADC acquisition, changing the ADC-before-POST architecture, blocking critical tasks, or changing normal runtime behavior.

## A. Existing Startup Architecture

Startup is controlled by `app_main()` in `src/main.c`, with LCD presentation rendered by the dedicated `lcd_task()` in `src/lcd_task.c` and state changes written through `src/lcd_writer.c`.

The relevant path is:

```text
Reset
  → app_main()
  → event queue / dispatcher / mutex setup
  → lcd_writer_init()                  [starts startup presentation clock]
  → NVS and system state initialization
  → services initialization (asynchronous network services)
  → hardware initialization
  → ADC manager start
  → LCD controller initialization
  → LCD task and LCD event receiver start
  → ADC task publishes APP_EVENT_ADC_READY
  → LCD task publishes APP_EVENT_LCD_READY
  → app_main waits for ADC_READY or ADC_FAILED and LCD_READY or LCD_FAILED
  → post_run_all()
  → startup health decision
  → visible-startup minimum gate
  → lcd_startup_release()
  → normal inverter operation
```

### Startup state machine

The existing LCD startup enum is defined in `include/lcd/lcd_state.h`:

| Stage | Meaning |
|---|---|
| `LCD_STARTUP_STAGE_HARDWARE` | Hardware check and LCD/sensor result presentation |
| `LCD_STARTUP_STAGE_POWER` | Battery and inverter power presentation |
| `LCD_STARTUP_STAGE_NETWORK` | Network status presentation |
| `LCD_STARTUP_STAGE_SERVICES` | Background service status presentation |
| `LCD_STARTUP_STAGE_SELF_CHECK` | Self-check presentation |
| `LCD_STARTUP_STAGE_READY` | Final ready presentation before the main UI |

Before those status stages, `lcd_task()` presents the boot identity screen and a loading/progress screen. `draw_startup_status()` renders both 16×2 and 20×4 layouts without making the timing dependent on LCD geometry.

### Readiness and failure events

The ADC task sets `APP_EVENT_ADC_READY` only after its initialization, configured channels, calibration/sample processing, and valid measurement path are established. It sets `APP_EVENT_ADC_FAILED` on failure. The LCD task sets `APP_EVENT_LCD_READY` after the controller and LCD task initialization path is complete; `APP_EVENT_LCD_FAILED` is handled by `app_main()` when the controller/task path cannot become ready.

`app_main()` waits on the event group in 100 ms increments and feeds the watchdog during the wait. POST is invoked only when both ADC and LCD readiness bits are present. If either failure bit is present or the wait times out, POST is not incorrectly treated as successful and the existing startup-fault behavior remains active.

### POST and services

`post_run_all()` runs the LCD, ADC, and fan power-on tests and returns a structured result. Network, MQTT, HTTP, WebSocket, mDNS, NTP, cloud, and OTA services are initialized as independent services; they are not used as prerequisites for ADC readiness or POST completion. Their failures therefore do not create an artificial wait in the startup gate.

The physical LCD I²C frequency is kept at the validated `I2C_FREQ_HZ 25000` configuration.

## B. Why Startup Was Too Fast

The authoritative release point was immediately after the POST result and startup-health calculation. There was no deterministic minimum duration between boot presentation and release of startup filtering. The loading screen had a randomized duration between 1.8 and 3.6 seconds, which made the experience unpredictable and did not guarantee a minimum on fast physical hardware. Once POST completed quickly, the application could release startup ownership before a human had a consistent opportunity to observe the sequence.

The LCD task already had per-stage timers, but those timers controlled only the LCD state machine. They did not provide a centralized minimum from the beginning of startup through the terminal readiness/failure result.

## C. Changes Made

| File | Change | Reason |
|---|---|---|
| `include/lcd/lcd_startup_config.h` | Added centralized deterministic timing constants. | Avoid scattered magic numbers and remove random startup timing. |
| `include/lcd/lcd_writer.h` | Added `lcd_startup_minimum_elapsed()`. | Expose the presentation-only timing gate without coupling `app_main()` to LCD internals. |
| `src/lcd_writer.c` | Starts a boot presentation clock in `lcd_writer_init()` and implements the minimum-duration check. | Ensures fast hardware still provides a predictable visible startup. |
| `src/lcd_task.c` | Replaced randomized loading duration with the fixed configured duration; retained the existing stage renderer and transitions. | Makes every boot deterministic while preserving the existing LCD state machine. |
| `src/main.c` | Waits for the visible-startup minimum after readiness/POST evaluation, yielding and feeding the watchdog. | Holds the final startup result visibly without delaying ADC, LCD, protection, or service tasks. |
| `src/lcd.c` | Restored `I2C_FREQ_HZ` to 25 kHz. | Preserves the validated physical LCD configuration explicitly required for this task. |
| `docs/startup_timing_report.md` | Added this report. | Documents architecture, timing, safety order, and verification. |

No button classification, debounce, repeat, long-press, multi-click, GPIO ISR, ADC sampling period, I²C driver operation, service startup, or protection task was changed to create the longer presentation.

## D. New Startup Timing

### Centralized configuration

```c
#define LCD_STARTUP_MIN_VISIBLE_DURATION_MS 5000U
#define LCD_STARTUP_IDENTITY_DURATION_MS 1200U
#define LCD_STARTUP_LOADING_DURATION_MS 2400U
#define LCD_STARTUP_STAGE_DURATION_MS 850U
#define LCD_STARTUP_READY_DURATION_MS 1100U
```

**Previous effective minimum:** No deterministic minimum. The application released startup ownership immediately after POST and startup-health evaluation. The loading screen used a random 1.8–3.6 second interval when reached.

**New minimum/user-visible startup duration:** **5,000 ms**, measured from `lcd_writer_init()` until `lcd_startup_release()` is permitted. This is a minimum presentation gate, not a fixed replacement for readiness conditions. If ADC, LCD, POST, or another required condition is not ready, the firmware remains in its existing failure/blocked path.

### Stage durations

| Stage | Configured display duration |
|---|---:|
| Boot identity | 1,200 ms |
| Loading/progress | 2,400 ms |
| Hardware | 850 ms |
| Power | 850 ms |
| Network | 850 ms |
| Services | 850 ms |
| Self-check | 850 ms |
| Ready | 1,100 ms |

The global 5,000 ms minimum and the stage timers are complementary. The global gate guarantees visibility on fast hardware, while the existing stage timers keep each user-visible status state from flashing by immediately. The actual time before normal operation may be longer than 5 seconds when hardware initialization, POST, or the staged LCD sequence takes longer.

## E. Startup Sequence After the Change

```text
BOOT IDENTITY             → 1.2 s presentation minimum
  ↓
SYSTEM STARTING / LOADING → 2.4 s deterministic presentation
  ↓
ADC initialization        → runs normally in its own task
  ↓
ADC_READY                 → readiness event remains authoritative
  ↓
LCD_READY                 → readiness event remains authoritative
  ↓
POST                      → runs only after ADC_READY + LCD_READY
  ↓
HARDWARE CHECK            → 0.85 s stage timer
  ↓
POWER                     → 0.85 s stage timer
  ↓
NETWORK                   → 0.85 s stage timer
  ↓
SERVICES                  → 0.85 s stage timer
  ↓
SELF CHECK                → 0.85 s stage timer
  ↓
READY                     → 1.10 s final stage timer
  ↓
Minimum visible startup elapsed and all required checks passed
  ↓
lcd_startup_release()
  ↓
NORMAL UI / NORMAL OPERATION
```

Failure states retain their existing behavior. The timer never converts a failed readiness bit or failed POST result into a successful startup. The LCD fault screen, LED/buzzer indications, and `system_ready` safety gate remain authoritative.

## F. Safety and Initialization Order Verification

The required order remains intact:

```text
ADC initialization
    ↓
ADC channel configuration and calibration/sample path
    ↓
APP_EVENT_ADC_READY
    ↓
LCD_READY
    ↓
POST
    ↓
startup health decision
    ↓
visible minimum-duration gate
    ↓
normal startup
```

`post_run_all()` is still called only inside the branch where both `adc_ready` and `lcd_ready` are true. The new timer is checked only after that branch or its preserved failure branch has populated the terminal result.

## G. Watchdog and Task Verification

The longer interval is implemented as a yielding loop in `app_main()`:

```c
while (!lcd_startup_minimum_elapsed()) {
    task_watchdog_feed();
    vTaskDelay(pdMS_TO_TICKS(100));
}
```

This does not disable or reconfigure the watchdog. ADC acquisition, the LCD task, event dispatcher, button task, protection task, monitoring tasks, and background services continue running under the FreeRTOS scheduler. The existing ADC wait loop also continues to feed the watchdog while waiting for readiness events.

No delay was inserted into ADC sampling, I²C transfers, LCD driver operations, button scanning, safety checks, or inverter control.

## H. Build Result

| Check | Result |
|---|---|
| `python3 tools/test_firmware_contracts.py` | **24 tests passed** |
| `~/.local/bin/pio run -e esp32dev` | **Passed** |
| `git diff --check` | **Passed** |
| Flash image | Generated successfully for 4 MB flash |
| I²C frequency audit | `I2C_FREQ_HZ 25000` retained |
| Random startup timing audit | Startup loading no longer uses `esp_random()` |

## I. Remaining Issues

The repository does not include a physical-board test harness, so the exact perceived brightness, LCD contrast, and human visual comfort must still be confirmed on the real inverter. The implementation is intentionally conservative: it guarantees at least five seconds of presentation time and retains the existing stage sequence, but it does not add a new startup task or redesign the LCD screens.

The existing startup status sequence includes network and service presentation stages, but those stages are visual states rather than blocking prerequisites. Wi-Fi, MQTT, HTTP, WebSocket, mDNS, NTP, cloud, and OTA services remain asynchronous and are not artificially awaited by the new timing gate.

The startup stage durations remain compile-time configuration values. If field testing shows that the physical LCD needs a longer or shorter presentation, the values in `include/lcd/lcd_startup_config.h` can be adjusted without modifying low-level hardware drivers or the startup architecture.
