# Fan-tach GPIO reassignment

The firmware previously assigned both Battery Voltage ADC1 channel 7 and fan tachometer input to GPIO35. On the original ESP32, ADC1 channel 7 maps to GPIO35. This was a real board-level ownership collision: the fan tach input was configured before ADC readiness, while the ADC health gate required a valid and fresh battery-voltage measurement.

The fan tach input is now assigned to **GPIO23**, selected because the repository audit found it unused and it is a normal digital input-capable ESP32 GPIO. It does not overlap the five buttons, ADC inputs, LCD I2C pins, LCD backlight, LEDs, buzzer, relay, or fan PWM output. GPIO35 remains reserved for Battery Voltage.

| Signal | Firmware GPIO | Role |
|---|---:|---|
| Battery Voltage | GPIO35 | ADC1 channel 7; unchanged |
| Fan PWM | GPIO33 | LEDC output; unchanged |
| Fan Tach | GPIO23 | Digital falling-edge tach input; changed from GPIO35 |

**Required hardware action:** move the fan tachometer signal wire from GPIO35 to GPIO23 before testing this firmware. Do not connect the fan tach output and battery-voltage divider to the same pin. If the fan tach circuit requires a pull-up, retain the existing external pull-up or add one appropriate to the fan tach output and ESP32 voltage; GPIO23 supports ordinary digital input configuration, but the firmware cannot verify the external electrical circuit.

Compile-time assertions now prevent the selected fan-tach GPIO from colliding with the button, LCD, LED, buzzer, relay, fan-PWM, and other declared peripheral GPIOs.
