# ADC architecture and reuse guide

The firmware now has a three-layer ADC design. The low-level helper remains reusable, the acquisition backend is selected at compile time, and the inverter adapter owns application policy. This keeps POST, protections, battery estimation, LCD rendering, WebSocket/API reporting, and cloud reporting independent of whether samples arrive through DMA/continuous mode or the oneshot fallback.

## Architecture

| Layer | Files | Responsibility |
| --- | --- | --- |
| Portable helper | `src/adc/adc.c`, `include/adc/adc.h` | ESP-IDF oneshot unit lifecycle, calibration handles, calibrated/raw conversion, and the existing fixed-count multisample helper. |
| Acquisition backends | `src/adc/adc_continuous.c`, `src/adc/adc_oneshot.c`, `src/adc/inverter_adc_backend.h` | Exactly one backend is compiled into the firmware. Continuous mode uses the ESP-IDF ADC1 digital controller and parses DMA conversion frames; oneshot mode uses the reusable helper. |
| Inverter adapter | `src/adc/inverter_adc.c`, `include/adc/inverter_adc.h` | Channel mapping, electrical conversion, battery 12/24/48 V scaling, range mapping, telemetry health, filtering, protections, emergency shutdown, LCD/API/cloud updates, snapshots, and lifecycle state. |

`src/adc/inverter_adc.c` is the only application-facing ADC implementation. The rest of the firmware uses the common API and the existing `sys_state` values; it does not call ESP-IDF acquisition APIs or select a backend.

## Compile-time mode selection

The selector lives in `include/adc/inverter_adc_config.h`:

```c
#define INVERTER_ADC_MODE_CONTINUOUS 0
#define INVERTER_ADC_MODE_ONESHOT    1

#ifndef INVERTER_ADC_MODE
#define INVERTER_ADC_MODE INVERTER_ADC_MODE_CONTINUOUS
#endif
```

Continuous/DMA is the default. The header rejects every value other than the two named modes. The backend source files are compiled together by the project source glob, but preprocessor selection ensures that only one implementation exports `inverter_adc_backend_init()`, `inverter_adc_backend_read_sample()`, and the matching cleanup functions. There is no runtime attempt to initialize both ADC controllers.

The project provides explicit verification environments:

| PlatformIO environment | ADC mode | LCD geometry |
| --- | --- | --- |
| `esp32dev` | Continuous/DMA | 20x4 default |
| `esp32dev-continuous-16x2` | Continuous/DMA | 16x2 |
| `esp32dev-oneshot-20x4` | Oneshot | 20x4 |
| `esp32dev-oneshot-16x2` | Oneshot | 16x2 |

The LCD geometry remains controlled by `MENU_CONFIG_LCD_20X4`; build flags can override it without changing the physical default. These profiles do not alter ADC channels, attenuation, scaling, thresholds, or sampling counts.

## Common lifecycle and snapshot API

The public interface in `include/adc/inverter_adc.h` is intentionally backend-neutral:

```c
esp_err_t inverter_adc_start(void);
inverter_adc_state_t inverter_adc_get_state(void);
bool inverter_adc_is_ready(void);
esp_err_t inverter_adc_get_snapshot(inverter_adc_snapshot_t *out);
inverter_adc_mode_t inverter_adc_get_mode(void);
```

The snapshot is a coherent application-level view containing low-battery, AC, battery, and inverter-output voltages, a monotonically increasing sequence, a timestamp, and validity/freshness flags. `inverter_adc_get_mode()` is diagnostic information; consumers do not branch on it.

The lifecycle is:

1. `main.c` creates the shared event group and starts the ADC subsystem with `inverter_adc_start()`.
2. The selected backend creates and configures ADC1, establishes all calibration handles before sampling, and starts its hardware acquisition path.
3. The adapter processes the configured channels using the preserved ten-sample aggregation policy. Continuous mode accumulates ten matching DMA results per channel; oneshot mode calls the existing ten-read helper.
4. Telemetry health validates the physically scaled values and requires fresh battery and inverter-output telemetry before the adapter enters `INVERTER_ADC_STATE_READY` and sets `APP_EVENT_ADC_READY`.
5. `main.c` waits for both `APP_EVENT_ADC_READY` and `APP_EVENT_LCD_READY`. `post_run_all()` is called only inside that gate, so POST never consumes an uninitialized ADC state.
6. After readiness, the adapter continues the existing warmup and protection timing. A later freshness failure clears data-valid state and can trigger emergency disable; the historical startup event is not falsely reused as a live validity flag.

An initialization failure sets `APP_EVENT_ADC_FAILED`, marks the state failed, and leaves output inhibited. A continuous DMA read/overflow or invalid sample is recorded through telemetry health; if required data is not fresh, the system remains data-invalid and applies the existing emergency behavior when the inverter is active.

## Electrical and safety policy preserved

The adapter still applies the measured ESP32 ADC pin-range mapping of 0.4–3.12 V to the 0–3.3 V logical range, then applies each channel’s external divider ratio. Battery voltage is scaled after the divider for the selected 12/24/48 V system, preserving the existing battery profile and filter behavior. Required telemetry is checked for finite, physically bounded, and fresh values before startup readiness.

The adapter continues to set and clear channel error flags, run `check_protections()` only after the existing ten valid warmup cycles, publish LCD data for both supported geometries, broadcast WebSocket/cloud status, show fault screens, and call `inverter_emergency_disable()` when required telemetry becomes invalid during operation. POST remains a consumer-only check in `src/post/post_adc.c`; it neither initializes nor configures ADC hardware. The ADC POST check independently calls the common readiness/snapshot API and refuses to evaluate battery or output values unless the snapshot is marked valid and fresh.

## Portable low-level helper

The reusable helper can be copied into another ESP-IDF project as:

```text
src/adc/adc.c
include/adc/adc.h
```

It depends only on ESP-IDF ADC and calibration APIs plus standard types. Its public functions are:

| Function | Purpose |
| --- | --- |
| `adc_unit_init()` | Creates an ESP-IDF oneshot unit. |
| `adc_calibration_init()` | Creates the best supported calibration scheme. |
| `adc_calibration_deinit()` | Releases a calibration handle. |
| `adc_read_with_multisampling()` | Performs the requested number of oneshot reads and returns average ADC-pin volts. |
| `adc_resources_cleanup()` | Releases oneshot calibration state and the unit. |

The helper does not know about `system_state_t`, LCD code, telemetry health, battery profiles, WebSocket, cloud reporting, or inverter safety policy. Continuous/DMA projects should reuse its calibration conversion definitions or provide an equivalent target-specific raw-to-voltage adapter; the continuous backend in this repository uses the same ESP-IDF calibration handles and fallback constants.

## Validation commands

From the repository root:

```text
python3 tools/test_firmware_contracts.py
pio run -e esp32dev
pio run -e esp32dev-continuous-16x2
pio run -e esp32dev-oneshot-20x4
pio run -e esp32dev-oneshot-16x2
pio check -e esp32dev
```

These checks validate the host-side startup/mode contracts, compile the default continuous configuration, compile the alternate LCD geometry, compile both oneshot combinations, and run static analysis. No physical HIL claim is made unless an ESP32 board and serial device are actually available.
