# WireClaw – NATS Integration

---

## NATS Integration

When `nats_host` is configured, the device subscribes to:

| Subject | Description |
|---------|-------------|
| `{device_name}.chat` | Request/reply - send a message, get LLM response |
| `{device_name}.cmd` | Commands: status, clear, heap, debug, devices, rules, memory, time, reboot |
| `{device_name}.events` | Published events: online, rule triggers, tool_exec results |
| `{device_name}.tool_exec` | Request/reply - execute a tool directly (no LLM), returns JSON result |
| `{device_name}.capabilities` | Request/reply - query devices, rules, tools, version |
| `{device_name}.hal.>` | Request/reply - direct hardware access (GPIO, ADC, PWM, UART, system, devices) |
| `_ion.discover` | Request/reply - discover all WireClaw devices on the network |

Rule triggers automatically publish events:

```json
{"event":"rule","rule":"cool down","state":"on","reading":29,"threshold":28}
```

### NATS Virtual Sensors

Any NATS subject can become a sensor in WireClaw's device registry. Register it via conversation, and the ESP32 subscribes and stores the last received value. Rules, `sensor_read`, and message interpolation all work on it like any other sensor. No pin needed.

```
device_register(name="power", type="nats_value", subject="home.power", unit="W")
```

#### Payload Formats

The sensor accepts multiple payload formats on the subscribed subject:

| Published payload | Stored value | Stored message |
|---|---|---|
| `42.5` | 42.5 | *(empty)* |
| `{"value":42.5}` | 42.5 | *(empty)* |
| `{"value":42.5,"message":"Peak load"}` | 42.5 | Peak load |
| `on` / `true` / `1` | 1.0 | *(empty)* |
| `off` / `false` / `0` | 0.0 | *(empty)* |

#### Reading

Via LLM tool:
```
sensor_read(name="power")  ->  power: 3200.0 W
```

Via serial:
```
/devices
  power [nats_value] nats=home.power  = 3200.0 W
```

#### Rules on NATS Sensors

Rules work exactly the same as any other sensor:

```
"Alert me on Telegram when power exceeds 3000W."

-> rule_create(sensor_name="power", condition="gt", threshold=3000,
               on_action="telegram", on_telegram_message="Power high: {value}W",
               off_action="telegram", off_telegram_message="Power normal: {value}W")
```

Relay control from a remote sensor:

```
device_register(name="motion", type="nats_value", subject="garage.motion")
device_register(name="light", type="relay", pin=4)

rule_create(rule_name="garage_light", sensor_name="motion",
            condition="eq", threshold=1, actuator_name="light")
```

When something publishes `1` to `garage.motion`, the relay turns on. `0` turns it off.

#### Message Forwarding with `{name:msg}`

JSON payloads can include a `"message"` field. Use `{name:msg}` in rule templates to forward it:

```bash
nats pub alerts.fire '{"value":1,"message":"Smoke detected in kitchen"}'
```

```
device_register(name="fire", type="nats_value", subject="alerts.fire")

rule_create(rule_name="fire_alert", sensor_name="fire",
            condition="eq", threshold=1,
            on_action="telegram", on_telegram_message="{fire:msg}")
```

Sends `Smoke detected in kitchen` to Telegram. Use `{fire}` (without `:msg`) for the numeric value.

You can combine both in one message:

```
on_telegram_message="Power: {value}W - {power:msg}"  ->  "Power: 3500W - Washing machine + dryer running"
```

#### Periodic Reporting

```
rule_create(rule_name="power_report", sensor_name="power",
            condition="always", threshold=0, interval_seconds=300,
            on_action="telegram", on_telegram_message="Power: {power}W")
```

Sends the current power reading to Telegram every 5 minutes.

#### Removal

```
device_remove(name="power")
```

Unsubscribes from the NATS subject and removes the device. Rules referencing it stop evaluating.

#### Persistence and Limits

- Persisted to `/devices.json`, re-subscribed automatically after reboot or NATS reconnect
- Last received value resets to 0 on boot until a new message arrives
- Up to 16 devices total (shared with physical sensors/actuators and built-in virtual sensors)
- NATS subject max length: 31 characters, message field max length: 63 characters
- 10 NATS subscription slots available (16 max minus 6 for chat, cmd, tool_exec, capabilities, discover, hal)

### Serial Text UART

Any serial device can become a sensor in WireClaw's device registry. Register it with a baud rate, and the ESP32 reads newline-delimited text from UART1 on fixed pins. The last received line is stored as both a numeric value (for rule conditions) and a text string (for message interpolation). You can also send text out via the `serial_send` tool or rule action.

Only one serial_text device is allowed (one spare UART). UART0 is reserved for USB serial debug.

#### Pin Assignments

| Chip | UART1 RX | UART1 TX |
|------|----------|----------|
| ESP32-C6 | GPIO 4 | GPIO 5 |
| ESP32-S3 | GPIO 19 | GPIO 20 |
| ESP32-C3 | GPIO 4 | GPIO 5 |

Pins are fixed per chip - no configuration needed. Connect your serial device's TX to the RX pin and vice versa. Don't forget a common ground.

#### Registration

```
device_register(name="arduino", type="serial_text", baud=9600)
```

Supported baud rates: 9600, 19200, 38400, 57600, 115200 (or any standard rate). Default: 9600.

#### Receiving Data

The ESP32 reads bytes non-blocking in the main loop and accumulates them until a newline (`\n`). The last complete line is stored and parsed:

| Received line | Stored value | Stored message |
|---|---|---|
| `42.5` | 42.5 | `42.5` |
| `{"value":42.5,"message":"Hot"}` | 42.5 | Hot |
| `on` / `true` / `1` | 1.0 | `on` / `true` / `1` |
| `Hello world` | 0.0 | `Hello world` |

Same parsing as NATS virtual sensors. Pure text lines that aren't numbers or JSON are stored as the message with value 0.0.

#### Sending Data

Via LLM tool:
```
serial_send(text="GET_TEMP")
```

Sends `GET_TEMP\n` over the UART. A newline is appended automatically if not present.

Via rule action:
```
rule_create(rule_name="poll_sensor", sensor_name="chip_temp",
            condition="always", threshold=0, interval_seconds=30,
            on_action="serial_send", on_serial_text="READ")
```

Sends `READ\n` every 30 seconds. Supports `{value}` and `{device_name}` interpolation in the text.

#### Example: Arduino Bridge

Wire an Arduino's TX to the ESP32's UART1 RX pin, share ground:

```
You:  "Connect an Arduino on serial at 9600 baud"

AI:   device_register(name="arduino", type="serial_text", baud=9600)
      -> Registered serial_text sensor 'arduino' at 9600 baud (RX=4 TX=5)

You:  "Every 30 seconds, send READ to the Arduino.
       Alert me on Telegram when it reports above 50."

AI:   rule_create(rule_name="poll", sensor_name="chip_temp",
                  condition="always", interval_seconds=30,
                  on_action="serial_send", on_serial_text="READ")
      -> Rule created: rule_01 'poll'

      rule_create(rule_name="alert", sensor_name="arduino",
                  condition="gt", threshold=50,
                  on_action="telegram",
                  on_telegram_message="Arduino: {arduino:msg}")
      -> Rule created: rule_02 'alert'
```

The first rule sends `READ\n` to the Arduino every 30 seconds. The Arduino replies with a number (e.g. `51.3\n`). The second rule fires when that value exceeds 50 and sends the text to Telegram.

#### Example: CO2 Sensor

Many serial CO2 sensors (MH-Z19, SenseAir S8) respond to simple commands:

```
You:  "Set up the CO2 sensor on serial at 9600 baud, call it co2.
       Send me a Telegram when CO2 goes above 1000 ppm."

AI:   device_register(name="co2", type="serial_text", baud=9600)
      rule_create(sensor_name="co2", condition="gt", threshold=1000,
                  on_action="telegram",
                  on_telegram_message="CO2 is {value} ppm - ventilate!")
```

#### Reading

Via LLM tool:
```
sensor_read(name="arduino")  ->  arduino: 23.5  (last: '23.5')
```

Via serial:
```
/devices
  arduino [serial_text] 9600baud  = 23.5   msg='23.5'
```

#### Limits

- One serial_text device at a time (one UART available)
- Maximum line length: 127 characters (longer lines are truncated)
- Text mode only (newline-delimited), no binary
- Last value resets to 0 on boot until a new line is received
- Persisted to `/devices.json`, UART re-initialized automatically after reboot
- `{name:msg}` interpolation works in telegram, nats_publish, and serial_send rule actions

#### Removal

```
device_remove(name="arduino")
```

Stops the UART and removes the device. Rules referencing it stop evaluating.

### Multi-Device Communication

Devices on the same NATS server can talk to each other. The AI on one device uses `remote_chat` to send a message to another device's agentic loop and get a response back:

```
You (on wireclaw-01): "Ask garden-node what its soil moisture is."

AI calls: remote_chat(device="garden-node", message="What is your soil moisture reading?")

garden-node processes the request, reads its sensor, replies.

AI: "Garden-node reports soil moisture at 42%."
```

Each device needs a unique `device_name` and the same `nats_host`. The request times out after 30 seconds if the target device is offline.

```bash
# Watch rule events
nats sub "wireclaw-01.events"

# Chat with the AI
nats req wireclaw-01.chat "What's the temperature?"

# System command
nats req wireclaw-01.cmd "rules"
```
