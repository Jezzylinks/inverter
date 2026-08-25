# Continuous ADC zero-frame investigation

## Hardware evidence

The board reports `ESP_ERR_TIMEOUT` from `adc_continuous_read()` with `frames=0`, `overflows=0`, and `last_frame=0`. This proves no conversion-done callback has run and no pool overflow has occurred; the failure precedes result parsing.

## Project evidence

The project uses ESP32 target, ADC1 channels 6, 0, 7, and 4, continuous mode, TYPE1 format, 12-bit width, 20 kHz sample frequency, 256-byte conversion frames, and a 2048-byte store pool. No other project source calls I2S or ADC continuous/legacy DMA APIs. GPIO35 is also assigned as the fan tach input and is ADC1 channel 7; this is a pin-use conflict worth documenting, but it should not by itself prevent all ADC1 frames.

## Installed ESP-IDF 5.3 driver behavior

The installed `components/esp_adc/adc_continuous.c` configures ESP32 continuous ADC through I2S0 DMA. `adc_continuous_config()` sets `use_adc1`, programs the pattern table, and configures the sample clock. `adc_continuous_start()` resets the digital controller, starts DMA, connects the ADC digital output to DMA, and enables the digital trigger. `adc_continuous_read()` returns `ESP_ERR_TIMEOUT` only when the internal ring buffer has no data.

The official v5.2 example uses the same essential values: ADC1, TYPE1, 20 kHz, 256-byte frame, callback registration before start, and a task that drains `adc_continuous_read()` after notification. The current project now logs callbacks and uses the official channel mask.

## Next root-cause targets

1. Verify the exact chip identity/revision and boot image target from hardware logs; an `esp32dev` image assumes the original ESP32.
2. Verify the continuous backend initialization log appears before the timeout lines and capture any `adc_continuous` component errors.
3. Check I2S0 occupation and the actual driver return values; `adc_continuous_new_handle()` should fail if I2S0 is already occupied, but a later LCD/audio legacy initialization must not claim/reconfigure I2S0.
4. Check whether the current firmware initializes fan tach GPIO35 before ADC; keep this conflict explicit and consider a safe pin ownership policy.
5. If the runtime remains zero-frame despite valid initialization, use the proven Oneshot fallback for operation and isolate Continuous with a minimal ADC-only test image before further application changes.

References:

- https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32/api-reference/peripherals/adc_continuous.html
- https://github.com/espressif/esp-idf/blob/master/examples/peripherals/adc/continuous_read/main/continuous_read_main.c
- https://github.com/espressif/esp-idf/blob/master/components/esp_adc/adc_continuous.c
- https://github.com/espressif/esp-idf/issues/12490
