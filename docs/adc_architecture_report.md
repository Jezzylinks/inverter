# ADC architecture refactor report

## Scope

This change applies the ADC architecture requirements from `pasted_content_3.txt` to the ESP32 PlatformIO/ESP-IDF firmware. The goal was to make ADC establishment a prerequisite for POST, provide a continuous/DMA default with an oneshot fallback, hide acquisition details behind a common interface and snapshot, and preserve the existing electrical scaling, telemetry safety, protection timing, LCD behavior, and terminal POST state propagation.

## Previous flow and principal defect

The previous application adapter initialized an ESP-IDF oneshot unit inside `adc_task()`, configured channels, and then processed four channels sequentially with ten oneshot reads per channel and a 20 ms delay. `APP_EVENT_ADC_READY` was set after the first loop pass even when the required telemetry-health state was not yet valid. `main.c` did wait for the ADC and LCD events before `post_run_all()`, but the event’s meaning was weaker than the required contract: it could represent task progress rather than a valid, fresh, scaled measurement set safe for POST.

## Resulting architecture

The ADC subsystem now has a portable helper, two mutually exclusive acquisition backends, and a common inverter adapter. `src/adc/adc.c` and `include/adc/adc.h` remain the reusable low-level helper for oneshot unit lifecycle, calibration, and fixed-count calibrated/raw conversion. `src/adc/adc_continuous.c` implements the default ESP-IDF ADC1 digital-controller/DMA backend. `src/adc/adc_oneshot.c` implements the explicit compatibility fallback. `src/adc/inverter_adc.c` owns application mapping and all downstream policy, including filtering, validation, protections, LCD updates, WebSocket/cloud reporting, and emergency behavior.

The menuconfig selectors are defined in `src/Kconfig.projbuild`. Continuous mode is the default (`CONFIG_INVERTER_ADC_MODE_CONTINUOUS`), and oneshot is the fallback (`CONFIG_INVERTER_ADC_MODE_ONESHOT`). LCD geometry is also a menuconfig choice, defaulting to `CONFIG_INVERTER_LCD_20X4`. The headers `include/adc/inverter_adc_config.h` and `include/lcd/menu_config.h` map the generated `sdkconfig.h` values and reject conflicting or missing selections at compile time. Both backend source files are present in the component source set, but only the selected backend exports the private backend symbols, so the firmware never initializes both acquisition implementations.

## Common API and readiness contract

`include/adc/inverter_adc.h` now provides `inverter_adc_start()`, `inverter_adc_get_state()`, `inverter_adc_is_ready()`, `inverter_adc_get_snapshot()`, and `inverter_adc_get_mode()`. The snapshot contains the four scaled application measurements, a sequence number, timestamp, required-data validity, and freshness.

`main.c` starts the ADC subsystem through `inverter_adc_start()` before creating the LCD task and waits for `APP_EVENT_ADC_READY` and `APP_EVENT_LCD_READY`. The ADC adapter sets `APP_EVENT_ADC_READY` only after backend initialization/configuration/calibration has succeeded and the required battery and inverter-output channels have produced finite, physically bounded, fresh values. If initialization or task creation fails, `APP_EVENT_ADC_FAILED` is set and output remains inhibited.

`src/post/post_adc.c` is still a consumer-only check. It does not initialize ADC hardware. It now independently requires the common ADC lifecycle to be READY and the common snapshot to be valid and fresh before evaluating its battery plausibility and idle-output conditions. Thus, the event gate in `main.c` and the consumer-side guard both prevent POST from operating on an uninitialized ADC state.

## Preserved electrical and safety behavior

The refactor preserves the existing 0.4–3.12 V to 0–3.3 V ADC range mapping, the four ADC1 channels, 12 dB attenuation, all external voltage-divider ratios, and the selected 12/24/48 V battery-system multiplier. It retains ten-sample aggregation in both modes: the oneshot backend uses the existing ten-read helper, while continuous mode accumulates ten matching DMA results for each configured channel before returning a channel sample to the application adapter.

Telemetry health continues to reject invalid or out-of-range values and to enforce freshness. The existing battery filter remains in the application adapter. Protection dispatch remains blocked until ten valid warmup cycles have completed. Existing LCD main/fault/standby updates, both 16x2 and 20x4 geometry paths, WebSocket/cloud status updates, channel error flags, stale-data invalidation, and emergency disable behavior remain in the common application layer rather than in a backend.

## Continuous backend details

The default continuous backend uses ADC1 only, avoiding ADC2/Wi-Fi contention. It configures the ESP-IDF digital controller with a four-channel pattern, type-1 ESP32 conversion results, a nominal aggregate sample frequency of 2 kHz, DMA frame sizing for forty conversion results, and a larger internal storage pool. It parses channel-tagged conversion results, applies the already-established calibration handles when available, and falls back to the existing raw-count conversion constants when calibration is unavailable. DMA read errors, including pool overflow state, are returned explicitly and the pool is flushed when appropriate.

The nominal controller rate is approximately 500 conversions per channel per second before scheduler and processing overhead. Ten samples per channel therefore represent a short aggregation window while retaining the previous sample count rather than arbitrarily reducing it. No physical timing or noise measurement is claimed because an ESP32 board was not available in the sandbox.

## Build and test matrix

| Check | Result |
| --- | --- |
| `python3 tools/test_firmware_contracts.py` | Passed, 15 tests. |
| `pio run -e esp32dev -t menuconfig` | Menuconfig target is documented and the active build generated the ADC/LCD symbols. |
| `pio run -e esp32dev` | Passed; default Continuous/DMA + 20x4. |
| Temporary menuconfig state: Continuous/DMA + 16x2 | Passed; active `sdkconfig` was restored afterward. |
| Temporary menuconfig state: Oneshot + 20x4 | Passed; active `sdkconfig` was restored afterward. |
| Temporary menuconfig state: Oneshot + 16x2 | Passed; active `sdkconfig` was restored afterward. |
| `pio check -e esp32dev` | Passed with 0 high-severity findings, 9 existing medium warnings, and 721 low/style findings across the existing project. The ADC component itself reported 0 high and 0 medium findings. |
| `git diff --check` | Passed. |
| Repository-wide `ESP_ERROR_CHECK(` scan in `src` and `include` | Passed with no matches. |

The contract suite now verifies menuconfig-backed default selection, compile-time exclusivity, common snapshot/readiness declarations, POST ordering, the direct POST snapshot guard, and terminal startup fault propagation.

## Files changed

The principal changes are in `src/adc/inverter_adc.c`, `src/adc/adc_continuous.c`, `src/adc/adc_oneshot.c`, `src/adc/inverter_adc_backend.h`, `include/adc/inverter_adc.h`, `include/adc/inverter_adc_config.h`, `src/main.c`, and `src/post/post_adc.c`. `src/Kconfig.projbuild` now provides the ADC and LCD menuconfig choices; `platformio.ini` documents the menuconfig workflow and retains only the UI-mock testing flag; `include/lcd/menu_config.h`, the contract tests, and `docs/adc_driver.md` were updated; this report records the architecture and verification results.

## Hardware verification limitation

The sandbox did not expose an ESP32 serial device or physical board. The available device list did not provide a usable ESP32 `/dev/ttyUSB*` or `/dev/ttyACM*` target. Consequently, this work claims source-level contracts, compilation, static analysis, and host verification only. It does not claim LCD electrical validation, ADC voltage accuracy, DMA timing, noise performance, boot behavior on the actual inverter, or hardware-in-the-loop results.
