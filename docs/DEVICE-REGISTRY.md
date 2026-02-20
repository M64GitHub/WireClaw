# WireClaw – Device Registry

---

## Device Registry

Named sensors and actuators that the AI and rule engine can reference by name.

### Supported Device Types

| Type | Kind | Description |
|------|------|-------------|
| `digital_in` | Sensor | `digitalRead()` - 0 or 1 |
| `analog_in` | Sensor | `analogRead()` - raw ADC value 0-4095 |
| `ntc_10k` | Sensor | NTC 10K thermistor (B=3950, calibrated millivolt reads, 16-sample averaged). Background-polled every 5s with ADC warmup - rules and web UI read the cached value. Default: NTC between ADC and GND. Set `inverted=true` if NTC is on the 3.3V side |
| `ldr` | Sensor | Light-dependent resistor - rough lux estimate (16-sample averaged, no EMA - instant response) |
| `internal_temp` | Sensor | ESP32 chip temperature (no pin, virtual) |
| `clock_hour` | Sensor | Current hour 0-23 via NTP (no pin, virtual) |
| `clock_minute` | Sensor | Current minute 0-59 via NTP (no pin, virtual) |
| `clock_hhmm` | Sensor | Time as hour\*100+minute, e.g. 1830 = 18:30 (no pin, virtual) |
| `nats_value` | Sensor | Value received from a NATS subject (no pin, virtual) |
| `serial_text` | Sensor | Text lines from UART1 serial port (no pin, virtual, one max) |
| `digital_out` | Actuator | `digitalWrite()` - HIGH or LOW |
| `relay` | Actuator | `digitalWrite()` with optional inverted logic |
| `pwm` | Actuator | `analogWrite()` - 0-255 |
| `rgb_led` | Actuator | Onboard RGB LED - packed 0xRRGGBB value, brightness-scaled (auto-registered on chips with RGB_BUILTIN) |

`chip_temp`, `clock_hour`, `clock_minute`, `clock_hhmm`, and `rgb_led` (on boards with an onboard RGB LED) are auto-registered on first boot. All other devices are registered through conversation with the AI.

Devices persist to `/devices.json` on flash.
