# WireClaw – NATS HAL (Hardware Abstraction Layer)

---

## NATS HAL (Hardware Abstraction Layer)

Direct hardware access over NATS - no LLM, no JSON wrappers, just simple request/reply. Any system on the network can read GPIO pins, control PWM, query sensors, and set actuators with plain NATS messages.

The HAL subscribes to `{device_name}.hal.>` as a wildcard. The subject suffix determines the operation, the payload (if any) is the value to set, and the reply is a plain value or `ok`.

### Endpoints

| Subject | Payload | Reply | Description |
|---------|---------|-------|-------------|
| `hal.gpio.{pin}.get` | *(empty)* | `0` or `1` | Read GPIO pin (sets INPUT mode) |
| `hal.gpio.{pin}.set` | `0` or `1` | `ok` | Write GPIO pin (sets OUTPUT mode) |
| `hal.adc.{pin}.read` | *(empty)* | `0`-`4095` | Read ADC value (raw 12-bit) |
| `hal.pwm.{pin}.set` | `0`-`255` | `ok` | Set PWM output (value is cached) |
| `hal.pwm.{pin}.get` | *(empty)* | `0`-`255` | Read last PWM value set on pin |
| `hal.uart.read` | *(empty)* | last UART line | Read last serial_text line |
| `hal.uart.write` | text | `ok` | Send text over serial_text UART |
| `hal.system.temperature` | *(empty)* | `32.2` | Chip temperature in Celsius |
| `hal.system.heap` | *(empty)* | `81664` | Free heap in bytes |
| `hal.system.uptime` | *(empty)* | `3672` | Uptime in seconds |
| `hal.device.list` | *(empty)* | JSON array | All registered devices with values |
| `hal.{sensor}` | *(empty)* | `24.5` | Read a registered sensor by name |
| `hal.{sensor}.info` | *(empty)* | JSON object | Device details (name, kind, unit, value, pin) |
| `hal.{actuator}.set` | value | `ok` | Set a registered actuator |
| `hal.{actuator}.get` | *(empty)* | `0` | Read last actuator value |

Errors return JSON: `{"error":"bad_pin","detail":"pin out of range"}`

### Examples

Read chip temperature:

```
$ nats req wireclaw-01.hal.system.temperature "" --server nats://192.168.178.39:4222
10:17:16 Sending request on "wireclaw-01.hal.system.temperature"
10:17:16 Received with rtt 64.233439ms
32.2
```

Read a GPIO pin:

```
$ nats req wireclaw-01.hal.gpio.5.get "" --server nats://192.168.178.39:4222
10:17:55 Sending request on "wireclaw-01.hal.gpio.5.get"
10:17:55 Received with rtt 58.240902ms
0
```

Set a GPIO pin HIGH:

```
$ nats req wireclaw-01.hal.gpio.5.set "1" --server nats://192.168.178.39:4222
10:19:31 Sending request on "wireclaw-01.hal.gpio.5.set"
10:19:31 Received with rtt 44.655273ms
ok
```

Verify it changed:

```
$ nats req wireclaw-01.hal.gpio.5.get "" --server nats://192.168.178.39:4222
10:19:33 Sending request on "wireclaw-01.hal.gpio.5.get"
10:19:33 Received with rtt 63.62419ms
1
```

Query system status:

```bash
$ nats req wireclaw-01.hal.system.heap ""
81664

$ nats req wireclaw-01.hal.system.uptime ""
3672
```

Read a registered sensor by name:

```bash
$ nats req wireclaw-01.hal.chip_temp ""
32.2

$ nats req wireclaw-01.hal.chip_temp.info ""
{"name":"chip_temp","kind":"internal_temp","unit":"C","value":32.2,"pin":255}
```

List all registered devices:

```bash
$ nats req wireclaw-01.hal.device.list ""
[{"name":"chip_temp","kind":"internal_temp","value":32.2,"unit":"C"},...]
```

Set a registered actuator:

```bash
$ nats req wireclaw-01.hal.fan.set "1"
ok

$ nats req wireclaw-01.hal.fan.get ""
1
```

Read ADC and set PWM:

```bash
$ nats req wireclaw-01.hal.adc.4.read ""
2048

$ nats req wireclaw-01.hal.pwm.5.set "128"
ok

$ nats req wireclaw-01.hal.pwm.5.get ""
128
```

### HAL vs OpenClaw tool_exec

Both provide LLM-free access over NATS, but serve different use cases:

| | HAL | tool_exec |
|---|---|---|
| **Subject** | `{device}.hal.gpio.5.set` | `{device}.tool_exec` |
| **Payload** | `"1"` | `{"tool":"gpio_write","pin":5,"value":1}` |
| **Reply** | `ok` | `{"ok":true,"result":"GPIO 5 set to HIGH"}` |
| **Use case** | Scripts, WireMaster nodes, tight loops | OpenClaw, complex tool calls, rule creation |
| **Overhead** | Minimal | JSON parse + tool dispatch |

The HAL is designed for high-frequency, low-overhead access - ideal for scripts, monitoring dashboards, and other microcontrollers on the network that need to query or control hardware directly. Set actions (GPIO, PWM, UART write, actuator) execute even without a reply subject (fire-and-forget via `nats pub`).

### Reserved Names

The HAL reserves 8 keywords as subject segments: `gpio`, `adc`, `pwm`, `dac`, `uart`, `system`, `device`, `config`. These cannot be used as device names in the device registry. Attempting to register a device with a reserved name returns an error.

### Capabilities

The `{device_name}.capabilities` response includes a `hal` field showing available HAL features:

```json
{
  "hal": {
    "gpio": true,
    "adc": true,
    "pwm": true,
    "dac": false,
    "uart": true,
    "system_temp": true
  }
}
```

`dac` is `false` because ESP32-C6, S3, and C3 have no DAC peripheral. Requesting `hal.dac.*` returns an error.
