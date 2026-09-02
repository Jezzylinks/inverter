# ADC architecture improvement report

## Scope and audit conclusion

This report records the ADC architecture work requested for the ESP32 inverter firmware. The implementation keeps Continuous/DMA as the menuconfig-selected default, retains Oneshot as a legitimate secondary mode and runtime compatibility fallback, and preserves the existing electrical scaling, startup POST, telemetry validation, protection timing, LCD behavior, and emergency shutdown behavior.

The board evidence supplied during the audit was decisive: every Continuous read returned `ESP_ERR_TIMEOUT`, while the Continuous diagnostics reported `frames=0`, `overflows=0`, and `last_frame=0`. This places the failure before frame parsing; the DMA producer was not delivering a completed conversion frame to the application. ESP-IDF documents `ESP_ERR_TIMEOUT` from `adc_continuous_read()` as no data being available in the internal conversion pool during the requested timeout [1].

## Existing architecture

The reusable low-level helper in `src/adc/adc.c` owns ADC1 Oneshot unit creation, per-channel calibration, calibrated/raw conversion, ten-sample averaging, and cleanup. `src/adc/adc_continuous.c` owns the ADC1 digital-controller/DMA backend, while `src/adc/adc_oneshot.c` owns the compile-time Oneshot backend. `src/adc/adc_manager.c` is the application adapter: it maps measurements to inverter state, applies divider scaling and battery-system multipliers, updates telemetry health, drives the battery filter, publishes LCD/network values, and invokes existing protection and emergency-shutdown paths.

The four configured channels remain ADC1 channels 6, 0, 7, and 4 for Low Battery, AC Voltage, Battery Voltage, and Inverter Voltage. The existing channel-to-GPIO mappings, 12 dB attenuation, 12-bit conversion assumptions, resistor-divider ratios, and application measurement names are unchanged.

The ADC task remains the single acquisition consumer. It processes each configured channel, updates a coherent application snapshot under a critical section, enforces per-channel telemetry freshness, and does not set `APP_EVENT_ADC_READY` until required measurements have been valid and fresh. POST remains downstream of both ADC and LCD readiness and independently verifies the ADC snapshot before evaluating its own plausibility checks.

## Problems actually found

The original Continuous implementation used per-channel blocking reads against a shared DMA stream. That interface was not itself sufficient to explain the board failure, but it made it difficult to distinguish driver-level frame production from parser-level channel filtering. The initial frame-size and sample-rate corrections did not resolve the real hardware symptom.

The board logs then demonstrated a stronger fact: Continuous DMA produced no frame at all. This is not a voltage-divider, calibration-range, or channel-threshold failure. It is also not evidence that the four physical channels independently failed; the four warnings were repeated attempts to read one shared stream.

Before this work, the application-facing ADC snapshot exposed only scaled voltages and coarse validity/freshness flags. It did not expose whether the backend was operating normally, had degraded to Oneshot, or had entered a fault state, nor did it expose per-channel sample count, calibration, saturation, timestamp, and error metadata.

## Changes made

| File | Change and reason |
| --- | --- |
| `src/adc/adc_continuous.c` | Preserved Continuous/DMA as the primary backend; aligned frames to the ESP32 DMA conversion stride; masked channel IDs; registered conversion-done and pool-overflow diagnostics; resolved and logged ADC1 GPIO mappings; added a bounded no-frame transition to safe Oneshot operation. |
| `src/adc/adc_oneshot.c` | Added the matching runtime-status implementation for menuconfig-selected Oneshot mode. |
| `include/adc/adc_driver.h` | Added a backend runtime-status contract shared by both backends. |
| `include/adc/adc_manager.h` | Added backend states, per-channel measurement metadata, backend health counters, additive snapshot fields, and read-only status/measurement getters. Existing APIs remain available. |
| `src/adc/adc_manager.c` | Added coherent measurement caching, per-channel quality metadata, health counters, backend-state refresh, age-based freshness, and read-only getter implementations. Existing validation and protection decisions remain in place. |
| `tools/test_firmware_contracts.py` | Added source contracts for DMA diagnostics, safe fallback ordering, explicit backend states, measurement metadata, and status APIs. |
| `docs/adc_zero_frame_investigation.md` | Records the zero-frame evidence and driver investigation. |

## Resulting architecture

```text
ADC1 inputs
    |
    v
Continuous/DMA primary  <---- menuconfig default
    |
    | completed frames and channel demultiplexing
    v
Ten-sample calibrated/raw conversion
    |
    | no DMA frame for bounded startup grace period
    v
Stop + deinitialize Continuous
    |
    v
ADC1 Oneshot fallback  ---- visible FALLBACK/DEGRADED state
    |
    v
Application measurement cache
    |
    +--> validity, freshness, calibration, saturation, timestamp, errors
    |
    +--> control and protection
    +--> startup POST
    +--> LCD, REST, WebSocket, cloud telemetry
```

Continuous and Oneshot remain mutually exclusive at compile time. The runtime fallback is not a second simultaneously active ADC unit: Continuous is stopped and deinitialized before the Oneshot unit is created, so ADC1 ownership is not shared between both drivers.

## Backend state and health model

The public API now distinguishes the following states:

| State | Meaning |
| --- | --- |
| `ADC_DRIVER_UNINITIALIZED` | No backend context is active. |
| `ADC_DRIVER_CONTINUOUS` | The configured Continuous/DMA backend is active. |
| `ADC_DRIVER_ONESHOT` | Oneshot was selected directly through menuconfig. |
| `ADC_DRIVER_FALLBACK` | Continuous produced no frame within the bounded startup grace period and the firmware switched to Oneshot after releasing Continuous resources. |
| `ADC_DRIVER_FAULT` | Backend initialization or another unrecoverable ADC manager failure occurred. |

`adc_manager_get_backend_status()` exposes frame count, dropped-frame count, pool-overflow count, read errors, invalid samples, saturation count, consecutive success/failure counters, and the last successful sample time. `adc_manager_get_measurement()` exposes voltage, sample count, timestamp, error count, validity, calibration, freshness, and saturation for each application channel. These are read-only snapshots and do not permit consumers to bypass the common acquisition or safety policy.

## Measurement quality and safety behavior

Every successful sample records ten contributing samples, its timestamp, whether calibration was applied, whether the raw ADC voltage was near the configured measurable ceiling, and whether the converted engineering value passed the existing telemetry range. A failed acquisition records an error and marks that channel invalid and not fresh. Freshness is age-based against the existing one-second telemetry staleness window rather than being a permanent property of a prior successful sample.

The existing protection behavior is intentionally unchanged. Invalid or stale required telemetry prevents ADC readiness, prevents protection dispatch during warmup, clears the ADC-valid interlock, and triggers the existing emergency-disable path when the inverter is active or starting. Runtime fallback does not imply valid telemetry; the firmware must still obtain valid fresh Oneshot values before publishing ADC readiness.

No electrical limits, resistor-divider calculations, attenuation settings, channel assignments, fan checks, POST checks, or emergency behavior were weakened or bypassed. Saturation is recorded as a diagnostic quality flag; existing range validation remains the authority for deciding whether a converted value is trusted.

## Fallback behavior

In the Continuous build, the backend records conversion-done callbacks and waits only within the existing bounded read cycle. If no frame has been produced after 500 ms from Continuous start, the backend performs the following sequence:

1. It calls `adc_continuous_stop()`.
2. It calls `adc_continuous_deinit()` to release Continuous/DMA and I2S0 resources.
3. It creates a new ADC1 Oneshot unit.
4. It configures the same channels with the same attenuation and calibration state.
5. It reports `ADC_DRIVER_FALLBACK` through the common status and snapshot APIs.
6. It continues through the normal telemetry-validity, freshness, POST, and protection logic.

If the fallback cannot be initialized, the common ADC manager enters its existing failed state and keeps output inhibited. There is no automatic repeated Continuous recovery loop yet; repeatedly disrupting a functioning safety-compatible Oneshot path would require a separate cooldown and hardware validation design.

## Startup and POST

`adc_manager_start()` still runs before `post_run_all()`. The common ADC task sets `APP_EVENT_ADC_READY` only after required battery and inverter-output telemetry has been valid and fresh. `post_adc.c` independently requires the ADC lifecycle to be ready and the application snapshot to be valid and fresh before applying battery and idle-output plausibility checks. A fallback backend must satisfy the same readiness contract.

## Build and test results

| Check | Result |
| --- | --- |
| `python3 tools/test_firmware_contracts.py` | Passed: 18 tests. |
| Continuous/default build | Passed with ESP-IDF 5.3.0 / PlatformIO espressif32 6.8.1. |
| Menuconfig Oneshot build | Passed. |
| Temporary 16x2 LCD configuration | Previously passed and remains menuconfig-backed. |
| `git diff --check` | Passed. |
| Repository-wide `ESP_ERROR_CHECK(` scan | Existing contract remains enforced with no user-firmware matches. |
| Physical hardware validation | Not performed in the sandbox. |

## Hardware validation status

The supplied board logs first validated the diagnosis of a zero-frame Continuous producer and then exposed a second failure during the fallback transition: an ESP-IDF ringbuffer assertion immediately after LCD initialization. ESP-IDF 5.3 stops the ADC DMA engine but frees the Continuous ringbuffer before freeing the I2S0 interrupt handle. A pending EOF interrupt can therefore enter the driver during teardown. The fallback now waits two RTOS ticks after `adc_continuous_stop()` and before `adc_continuous_deinit()` so the disabled I2S0 interrupt can quiesce before the ringbuffer is deleted. This is a targeted teardown-race mitigation; it does not claim that Continuous frame production is fixed.

The supplied board logs have validated the diagnosis of a zero-frame Continuous producer, but they have not validated the corrected fallback build. After flashing the resulting commit, the expected diagnostic is:

```text
E (...) ADC_CONTINUOUS: No DMA frames after 500 ms; switched safely to ADC1 Oneshot fallback
```

The decisive follow-up is that the repeated Continuous `DMA read failed` messages stop, the backend status reports `FALLBACK`, and the normal ADC warmup reaches readiness only after valid fresh Oneshot measurements. Continuous operation itself remains unproven on the board and must not be described as hardware-validated until a board log shows nonzero frame production.

## References

[1]: https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32/api-reference/peripherals/adc_continuous.html "ESP-IDF ADC Continuous Mode Driver documentation"
[2]: https://github.com/espressif/esp-idf/issues/12053 "ESP-IDF issue #12053: adc_continuous_read returning ESP_ERR_TIMEOUT"
[3]: https://github.com/espressif/esp-idf/issues/10612 "ESP-IDF issue #10612: ESP32 ADC DMA sampling rate behavior"
[4]: https://github.com/espressif/esp-idf/blob/v5.3.0/components/esp_adc/adc_continuous.c "ESP-IDF 5.3 ADC continuous driver teardown implementation"
