# Reusable ESP32 ADC Driver

The reusable ADC implementation is split into two files:

```text
src/adc/adc.c
include/adc/adc.h
```

Copy these files into another ESP-IDF project, add the project `include/` directory to the component include paths, and compile `src/adc/adc.c` with the project sources. The driver uses the ESP-IDF oneshot ADC API and supports the ESP-IDF curve-fitting and line-fitting calibration schemes when available.

## Dependencies

The driver depends only on ESP-IDF and the C standard types:

```c
#include "esp_err.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
```

It does not depend on `system_state_t`, inverter settings, LCD code, telemetry-health code, battery filters, Wi-Fi, or any application-specific error flags. Those policies live in the inverter adapter at `src/adc/inverter_adc.c` and consume the driver through `include/adc/adc.h`.

## Public API

| Function | Purpose |
| --- | --- |
| `adc_unit_init()` | Creates an ESP-IDF ADC oneshot unit. |
| `adc_calibration_init()` | Creates the best supported calibration scheme for one ADC channel. |
| `adc_calibration_deinit()` | Releases a calibration handle. |
| `adc_read_with_multisampling()` | Reads a channel repeatedly and returns the average pin voltage. |
| `adc_resources_cleanup()` | Releases channel calibration state and deletes the ADC unit. |

A minimal usage pattern is:

```c
#include "adc/adc.h"

adc_oneshot_unit_handle_t unit = NULL;
adc_channel_state_t state = {0};
float pin_voltage = 0.0f;

if (!adc_unit_init(&unit, ADC_UNIT_1)) {
    /* Handle unit initialization failure. */
}

adc_oneshot_chan_cfg_t channel_config = {
    .bitwidth = ADC_BITWIDTH_DEFAULT,
    .atten = ADC_ATTEN_DB_12,
};
adc_oneshot_config_channel(unit, ADC_CHANNEL_0, &channel_config);

state.is_calibrated = adc_calibration_init(
    ADC_UNIT_1, ADC_CHANNEL_0, ADC_ATTEN_DB_12, &state.cali_handle);

esp_err_t result = adc_read_with_multisampling(
    unit,
    ADC_CHANNEL_0,
    state.cali_handle,
    state.is_calibrated,
    &pin_voltage,
    10);

adc_resources_cleanup(unit, &state, 1);
```

The caller owns channel configuration, channel-to-signal mapping, thresholds, filtering, and error policy. The driver only owns the ADC unit/calibration lifecycle and conversion of raw or calibrated samples into volts at the ADC pin.

## Configuration overrides

Uncalibrated fallback conversion uses these defaults from `include/adc/adc.h`:

```c
#define ADC_DRIVER_REFERENCE_VOLTAGE 3.3f
#define ADC_DRIVER_RAW_FULL_SCALE 4095.0f
```

Override them before including the header or through the compiler command line when a target project uses a different reference or resolution:

```text
-DADC_DRIVER_REFERENCE_VOLTAGE=3.3f
-DADC_DRIVER_RAW_FULL_SCALE=4095.0f
```

Calibrated reads use the voltage returned by ESP-IDF’s calibration scheme and do not use these fallback macros.

## Repository integration

The inverter application’s complete ADC task and application adapter are in `src/adc/inverter_adc.c`, with the task declaration in `include/adc/inverter_adc.h`. The adapter owns channel definitions, voltage-divider ratios, battery-system scaling, telemetry-health recording, filtering, LCD updates, and inverter safety actions. Its hardware primitives call the reusable driver functions from `adc/adc.h`.

For a subsequent project, copy only `src/adc/adc.c` and `include/adc/adc.h` when you need the portable driver. Copy `src/adc/inverter_adc.c` and `include/adc/inverter_adc.h` only when you also want this inverter’s application-specific policy and are prepared to provide its state, telemetry, display, and safety dependencies.

The ESP-IDF component exposes the reusable headers through `src/CMakeLists.txt`:

```cmake
idf_component_register(SRCS ${app_sources} INCLUDE_DIRS "../include")
```

For another project, use an equivalent component declaration with the directory containing `adc/adc.h` in `INCLUDE_DIRS`.

## Portability boundary

This module is portable across ESP32-family ESP-IDF projects that provide the oneshot ADC and calibration APIs. The caller must still select valid ADC units/channels and attenuation for the target chip, avoid ADC2/Wi-Fi conflicts where applicable, and apply the correct external voltage-divider ratio outside this driver.
