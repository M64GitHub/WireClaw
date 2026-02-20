# WireClaw – Rule Engine Reference

---

## Rule Engine

Rules monitor a sensor, evaluate a condition, and trigger an action - all in the main loop, no LLM involved.

### Conditions

| Condition | Meaning |
|-----------|---------|
| `gt` | Sensor reading > threshold |
| `lt` | Sensor reading < threshold |
| `eq` | Sensor reading == threshold |
| `neq` | Sensor reading != threshold |
| `change` | Sensor reading changed since last check |
| `always` | Fire every interval (periodic) |
| `chained` | Only fires when triggered by another rule's chain (see [Rule Chaining](RULE-CHAINING.md)) |

### Sensor Sources

Rules need a sensor to monitor. Two options:

- **Named sensor** (`sensor_name`) - a device from the registry (e.g. `chip_temp`, `clock_hour`, or any registered sensor). Preferred.
- **Raw GPIO pin** (`sensor_pin`) - reads a GPIO directly. Set `sensor_analog=true` for `analogRead()` (0-4095), otherwise `digitalRead()` (0/1).

Multiple rules monitoring the same named sensor see the exact same reading per evaluation cycle (cached internally).

### Actions

Each rule has an **on action** (fires when condition becomes true) and an optional **off action** (fires when condition clears). All parameters below have `on_` and `off_` variants.

| Action | Parameters | Description |
|--------|-----------|-------------|
| `actuator` | `actuator_name` | Set a registered actuator on/off by device name. Simplest option - just provide the actuator name and the rule handles on=1/off=0 automatically. |
| `led_set` | `on_r`, `on_g`, `on_b` (0-255) | Set the onboard RGB LED color. |
| `gpio_write` | `on_pin`, `on_value` (0 or 1) | Write a raw GPIO pin HIGH/LOW. |
| `nats_publish` | `on_nats_subject`, `on_nats_payload` | Publish a message to a NATS subject. Supports `{value}` and `{device_name}` interpolation. |
| `telegram` | `on_telegram_message` | Send a Telegram message. Supports `{value}` and `{device_name}` interpolation. Subject to `telegram_cooldown` (default 60s per rule). |
| `serial_send` | `on_serial_text` | Send text over serial_text UART. Supports `{value}` and `{device_name}` interpolation. |

### Examples

You just describe what you want in natural language. The AI picks the right parameters:

```
"Set GPIO 4 high when chip_temp exceeds 30, low when it drops back."
-> on_action=gpio_write, on_pin=4, on_value=1
  off_action=gpio_write, off_pin=4, off_value=0

"Turn on the fan when temperature exceeds 28."
-> actuator_name=fan (auto on/off)

"Set LED red above 35, green below."
-> on_action=led_set, on_r=255, on_g=0, on_b=0
  off_action=led_set, off_r=0, off_g=255, off_b=0

"Alert me on Telegram when chip_temp goes above 40."
-> on_action=telegram, on_telegram_message="Temp is {value}°C - overheating!"
  off_action=telegram, off_telegram_message="Back to normal at {value}°C."

"Send me a Telegram at 6 PM with the chip temperature."
-> sensor_name=clock_hhmm, condition=eq, threshold=1800
  on_action=telegram, on_telegram_message="Evening report: chip is {chip_temp}°C"

"Set LED pink at 8:10 AM."
-> sensor_name=clock_hhmm, condition=eq, threshold=810
  on_action=led_set, on_r=255, on_g=105, on_b=180

"Send me a Telegram every 2 minutes."
-> condition=always, interval_seconds=120
  on_action=telegram, on_telegram_message="heartbeat"

"Alert me when power from NATS exceeds 3000W."
-> sensor_name=power (nats_value), condition=gt, threshold=3000
  on_action=telegram, on_telegram_message="Power: {value}W - {power:msg}"

"Turn on the garage light when motion is detected over NATS."
-> sensor_name=motion (nats_value), condition=eq, threshold=1
  actuator_name=light (auto on/off)

"Every 30 seconds, ask the Arduino for a reading."
-> sensor_name=chip_temp (any sensor), condition=always, interval_seconds=30
  on_action=serial_send, on_serial_text="READ"

"Alert me on Telegram when the Arduino reports above 50."
-> sensor_name=arduino (serial_text), condition=gt, threshold=50
  on_action=telegram, on_telegram_message="Arduino: {value} - {arduino:msg}"
```

### Behavior

- **Edge-triggered** - conditions `gt`, `lt`, `eq`, `neq`, `change` fire once on threshold crossing, not repeatedly. When the condition clears, the off action runs (if configured).
- **Periodic** - condition `always` fires every interval, repeatedly. Use for heartbeats, periodic reports, scheduled tasks.
- **Auto-off** - when using `actuator_name` or `off_action`, the reverse action runs when the condition clears
- **Interval** - configurable per rule (default 5 seconds)
- **Telegram cooldown** - per-rule cooldown prevents message spam when sensor oscillates around threshold (configurable via `telegram_cooldown` in config.json, default 60s, 0 = disabled)
- **Message interpolation** - `{value}` in telegram/NATS/serial messages is replaced with the triggering sensor's reading; `{device_name}` (e.g. `{chip_temp}`) reads any named sensor live at fire time; `{name:msg}` inserts the message string from a NATS virtual sensor or serial_text device
- **Background sensor polling** - NTC sensors are read every 5s via `ntcReadWithWarmup()` (16-sample warmup burst + 300ms settle delay + 16-sample real read, using calibrated `analogReadMilliVolts()`). The ESP32 SAR ADC reads ~60mV high after >1s idle; the 300ms delay lets it settle. Rules, web UI, and LLM tools read the cached value - no direct ADC access. All sensors get sparkline history every 5 minutes (6 slots = last 30 minutes). History is recorded exclusively by the background poll
- **Sensor caching** - all rules monitoring the same sensor see the same value per evaluation cycle
- **NATS events** - every rule trigger publishes to `{device_name}.events`
- **Persistence** - rules survive reboots (`/rules.json`)
- **Rule chaining** - `chain_create` builds multi-step sequences in one call (up to 5 steps with delays). Internally creates linked rules with `condition="chained"` that only fire via chain. For advanced use (OFF-chains, manual linking), `rule_create` with `chain_rule`/`chain_off_rule` is also available. Max depth 8. See [Rule Chaining](RULE-CHAINING.md)
- **IDs** - auto-assigned: `rule_01`, `rule_02`, etc.
