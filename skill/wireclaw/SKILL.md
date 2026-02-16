---
name: wireclaw
description: >
  Control WireClaw IoT devices on your network. Use when the user wants to
  interact with physical hardware: LEDs, GPIO pins, sensors, relays, temperature,
  or create automation rules on ESP32 microcontrollers. Also use when the user
  references "wireclaw", "ESP32 devices", or physical/hardware automation.
tools:
  - Bash
  - Read
metadata:
  openclaw:
    requires:
      binaries:
        - nats
    env:
      - WIRECLAW_NATS_URL
---

# WireClaw — Physical World Automation for OpenClaw

WireClaw is an AI agent running on ESP32 microcontrollers ($5 chips) that
controls real hardware: LEDs, GPIO pins, relays, sensors. It connects via NATS.

You generate tool calls. WireClaw executes them directly on hardware. No second
LLM involved. Rules created on WireClaw persist and run 24/7 on the chip, even
when OpenClaw, the NATS server, or WiFi is offline.

## How to Talk to WireClaw

All communication uses `nats req` (request/reply). Never use `nats pub` for
tool execution — you need the response to confirm success.

**Flat JSON protocol** — the `"tool"` field names the tool, all other fields
are the tool's parameters at the same level:

```bash
nats req <device>.tool_exec '{"tool":"<tool_name>", ...params}'

# Response format:
{"ok":true,"result":"..."}
{"ok":false,"error":"..."}
```

IMPORTANT: The top-level key is `"tool"`, not `"name"`. This avoids collision
with tools that have a `"name"` parameter (like `device_register`).

### Wrapper Script

A convenience wrapper is available at `scripts/wc.sh`:

```bash
# Tool execution — merges "tool" key automatically
scripts/wc.sh exec <device> <tool_name> '{"param":"value"}'

# Query device capabilities
scripts/wc.sh caps <device>

# Discover all WireClaw devices on the network
scripts/wc.sh discover

# Subscribe to a device's event stream
scripts/wc.sh sub <device>
```

You can use either the wrapper or raw `nats req` commands — both work identically.

## Discovery

```bash
# Find all WireClaws on the network
scripts/wc.sh discover
# or: nats req "_wc.discover" "" --replies=0 --timeout=3s

# Query a specific device
scripts/wc.sh caps wireclaw-01
# or: nats req wireclaw-01.capabilities ""
```

Returns: device name, version, IP, registered sensors/actuators with current
values, active rules with status, and available tool names.

## Critical Rules — Read Before Generating Tool Calls

### Direct action vs. automation
- For direct requests ("set LED pink", "turn on pin 4"), use the direct tool:
  `led_set`, `gpio_write`, `actuator_set`, etc. Do NOT create a rule.
- Only create rules when the user asks for AUTOMATION with a condition:
  "when temperature exceeds...", "if sensor reads...", "whenever...", "at 8am..."

### One rule = one action type
Each rule supports only ONE action type (led_set OR telegram OR gpio_write, etc).
You CANNOT combine action types in a single rule.

To do two different actions on the same condition (e.g. LED + Telegram), create
TWO separate rules with the same sensor/condition/threshold.

When the user asks to ADD a new action to existing automation, do NOT delete
existing rules. Create an additional rule alongside them.

### on_action + off_action pattern
A single rule can have BOTH an on_action (fires when condition becomes true)
and an off_action (fires when condition becomes false). Use this instead of
creating two opposing rules.

Example: "LED red when hot, green when cool" = ONE rule with
on_action=led_set (red) + off_action=led_set (green).

### Edge-triggered behavior
All conditions except "always" are edge-triggered. A rule fires ONCE when the
condition transitions from false->true (on_action) and once when true->false
(off_action). It does NOT fire repeatedly while the condition stays true.

### The LED is NOT a registered actuator
The onboard RGB LED is controlled via `led_set` tool or `on_action="led_set"`
in rules. Do NOT use `actuator_name` for the LED — that field is for registered
actuator devices like relays or motors.

## Available Tools

### Hardware — Direct Control
- `led_set` — Set onboard RGB LED: `{"tool":"led_set","r":0-255,"g":0-255,"b":0-255}`
  - r=0, g=0, b=0 turns LED off
- `gpio_write` — Set GPIO pin: `{"tool":"gpio_write","pin":N,"value":0|1}`
- `gpio_read` — Read GPIO pin: `{"tool":"gpio_read","pin":N}` -> HIGH/LOW
- `temperature_read` — Read chip temperature (no params) -> degrees C

### Sensors & Actuators
- `device_register` — Register hardware:
  `{"tool":"device_register","name":"fan","type":"relay","pin":16}`
  Types: `digital_in`, `analog_in`, `ntc_10k`, `ldr`, `nats_value`,
  `serial_text`, `digital_out`, `relay` (inverted logic), `pwm`
- `device_list` — List all registered devices with current readings
- `device_remove` — Remove by name: `{"tool":"device_remove","name":"fan"}`
- `sensor_read` — Read a sensor: `{"tool":"sensor_read","name":"chip_temp"}` -> value + unit
- `actuator_set` — Set an actuator: `{"tool":"actuator_set","name":"fan","value":1}`

### Pre-registered Sensors (always available, no registration needed)
- `chip_temp` — Internal chip temperature in degrees C
- `clock_hour` — Current hour 0-23
- `clock_minute` — Current minute 0-59
- `clock_hhmm` — Time as hour x 100 + minute (e.g. 810=08:10, 1830=18:30)

### NATS Virtual Sensors
Register a sensor that receives values from any NATS subject:
```bash
scripts/wc.sh exec wireclaw-01 device_register '{"name":"room_temp","type":"nats_value","subject":"home.room.temp","unit":"C"}'
```
- No pin needed. Stores the last value received on that subject.
- Accepted payloads: bare numbers ("32.5"), JSON (`{"value":32.5}`),
  booleans ("on"/"off" -> 1/0).
- JSON payloads can include a "message" field: `{"value":32.5,"message":"Alert text"}`.
- Use `{sensor_name:msg}` in templates to insert the message field.
- Then create rules on it like any physical sensor.

This is powerful for OpenClaw integration: publish to a NATS subject from
OpenClaw, and WireClaw reacts to it with persistent rules.

### Serial Text UART
- `{"tool":"device_register","name":"x","type":"serial_text","baud":9600}` — attach serial device on UART1
- Only ONE serial_text device allowed. Fixed pins per chip: C6/C3=RX4/TX5, S3=RX19/TX20.
- Stores last received text line. Numbers parsed as float for rule conditions.
- IMPORTANT: `{name}` gives NUMERIC value (0 if text). `{name:msg}` gives actual TEXT.
  Always use `{name:msg}` when forwarding serial text to telegram/nats.
- `serial_send` tool: sends text to UART. In rules: `on_action="serial_send"` with `on_serial_text="message"`.
- `condition="change"` detects both numeric value changes AND text changes.

### Automation Rules

**rule_create** — Create a persistent rule that runs 24/7 on the ESP32:
```bash
scripts/wc.sh exec wireclaw-01 rule_create '{"rule_name":"heat alert","sensor_name":"chip_temp","condition":"gt","threshold":35,"on_action":"led_set","on_r":255,"on_g":0,"on_b":0,"off_action":"led_set","off_r":0,"off_g":255,"off_b":0}'
```

**Conditions:** `gt` (>), `lt` (<), `eq` (==), `neq` (!=), `change`, `always`

**Actions** (one type per rule):
- `led_set` — on_r, on_g, on_b / off_r, off_g, off_b (0-255)
- `gpio_write` — on_pin, on_value / off_pin, off_value
- `telegram` — on_telegram_message / off_telegram_message
- `nats_publish` — on_nats_subject, on_nats_payload / off variants
- `actuator` — actuator_name (registered device name, NOT "led")
- `serial_send` — on_serial_text / off_serial_text

**Template variables in messages:**
- `{value}` — the triggering sensor's current reading
- `{device_name}` — reads ANY named sensor's value at fire time
  (e.g. `{chip_temp}` in a message on a different sensor's rule)
- `{name:msg}` — the message field from nats_value/serial_text payloads

**Periodic rules:**
Use `condition="always"` with `interval_seconds=N` for repeating tasks.
Example: publish temperature every 60 seconds:
```bash
scripts/wc.sh exec wireclaw-01 rule_create '{"rule_name":"temp publish","sensor_name":"chip_temp","condition":"always","interval_seconds":60,"on_action":"nats_publish","on_nats_subject":"home.temp","on_nats_payload":"{value}"}'
```
"always" rules fire repeatedly. All other conditions are edge-triggered.

**Time-based rules:**
- Whole hours: `sensor_name="clock_hour"`, `condition="eq"`, `threshold=18` -> at 6 PM
- Exact times: `sensor_name="clock_hhmm"`, `condition="eq"`, `threshold=810` -> at 08:10
- Ranges: `sensor_name="clock_hhmm"`, `condition="gt"`, `threshold=2200` -> after 10 PM
- For "do X at time Y", use ONE rule with on_action + off_action.
  The off_action auto-fires when the minute passes (edge-triggered).

**rule_list** — List all rules with status. Always call this before deleting.

**rule_delete** — Delete by ID: `{"tool":"rule_delete","rule_id":"rule_01"}` or `{"tool":"rule_delete","rule_id":"all"}` to wipe all.

**rule_enable** — Enable/disable without deleting: `{"tool":"rule_enable","rule_id":"rule_01","enabled":false}`

### Chain Automation (multi-step sequences)
For 2-5 step automations, use `chain_create`. One tool call creates the full chain.

```bash
scripts/wc.sh exec wireclaw-01 chain_create '{"sensor_name":"chip_temp","condition":"gt","threshold":40,"step1_action":"telegram","step1_message":"Overheating: {value}C","step2_action":"led_set","step2_delay":5,"step2_r":255,"step2_g":0,"step2_b":0,"step3_action":"led_set","step3_delay":10,"step3_r":0,"step3_g":0,"step3_b":0}'
```
- step1 fires immediately on trigger. step2+ fire after their delay (seconds).
- For simple single-action rules, use rule_create instead.

### System & Communication
- `device_info` — Heap, uptime, WiFi, chip info
- `file_read` — Read file from ESP32 filesystem: `{"tool":"file_read","path":"/memory.txt"}`
- `file_write` — Write file: `{"tool":"file_write","path":"/memory.txt","content":"..."}`
- `nats_publish` — Publish NATS message: `{"tool":"nats_publish","subject":"...","payload":"..."}`
- `serial_send` — Send text over UART: `{"tool":"serial_send","text":"GET_TEMP"}`
- `remote_chat` — Chat with another WireClaw via its LLM:
  `{"tool":"remote_chat","device":"garden-node","message":"what is your temperature?"}`
  (This invokes the other device's LLM. For direct tool calls on another
  device, use `nats req other-device.tool_exec` instead.)

### Subscribing to Events
```bash
scripts/wc.sh sub wireclaw-01
# or: nats sub "wireclaw-01.events"
```
Events: online announcements, rule triggers (with reading/threshold), tool_exec results.

## Cross-Domain Automation Patterns

### Digital trigger -> Physical action
OpenClaw detects something digital (CI failure, calendar event, email) and
sends a direct tool call or creates a persistent rule on WireClaw.

### Physical trigger -> Digital action (via NATS bridge)
Create a WireClaw rule with `on_action="nats_publish"`. OpenClaw subscribes
to that NATS subject using `nats sub` and triggers digital workflows.

### External data -> WireClaw sensor (via nats_value)
OpenClaw publishes data to a NATS subject. WireClaw has a `nats_value` sensor
on that subject. Rules on WireClaw react to changes autonomously.

This is the most powerful pattern: OpenClaw pushes data once, and WireClaw's
persistent rules handle all the logic locally, forever, without OpenClaw.

Example: OpenClaw checks weather API, publishes temperature to
`home.weather.temp`. WireClaw has a nats_value sensor on that subject with
rules that change LED color based on temperature ranges.

### Prefer rules over repeated commands
Once you create a rule on WireClaw, it runs forever on the ESP32 with zero
external dependencies. Use direct tool calls (`led_set`, `gpio_write`) only
for one-off actions. For anything ongoing, create a rule.

## Examples

**Set LED blue (one-off):**
```bash
scripts/wc.sh exec wireclaw-01 led_set '{"r":0,"g":0,"b":255}'
```

**Temperature alert with Telegram + LED (needs TWO rules — one action type each):**
```bash
scripts/wc.sh exec wireclaw-01 rule_create '{"rule_name":"heat telegram","sensor_name":"chip_temp","condition":"gt","threshold":30,"on_action":"telegram","on_telegram_message":"Office is {value}C!","off_action":"telegram","off_telegram_message":"Office cooled to {value}C"}'

scripts/wc.sh exec wireclaw-01 rule_create '{"rule_name":"heat led","sensor_name":"chip_temp","condition":"gt","threshold":30,"on_action":"led_set","on_r":255,"on_g":0,"on_b":0,"off_action":"led_set","off_r":0,"off_g":255,"off_b":0}'
```

**LED at 8:10 AM every day:**
```bash
scripts/wc.sh exec wireclaw-01 rule_create '{"rule_name":"morning light","sensor_name":"clock_hhmm","condition":"eq","threshold":810,"on_action":"led_set","on_r":255,"on_g":180,"on_b":50,"off_action":"led_set","off_r":0,"off_g":0,"off_b":0}'
```

**Publish sensor data every 60 seconds for OpenClaw to consume:**
```bash
scripts/wc.sh exec wireclaw-01 rule_create '{"rule_name":"temp stream","sensor_name":"chip_temp","condition":"always","interval_seconds":60,"on_action":"nats_publish","on_nats_subject":"home.office.temp","on_nats_payload":"{value}"}'
```

**Bridge external data into WireClaw (OpenClaw pushes, WireClaw reacts):**
```bash
# Step 1: Register a NATS virtual sensor on WireClaw
scripts/wc.sh exec wireclaw-01 device_register '{"name":"ci_status","type":"nats_value","subject":"ci.build.status","unit":""}'

# Step 2: Create rule that reacts to it
scripts/wc.sh exec wireclaw-01 rule_create '{"rule_name":"ci alert","sensor_name":"ci_status","condition":"eq","threshold":0,"on_action":"led_set","on_r":255,"on_g":0,"on_b":0,"off_action":"led_set","off_r":0,"off_g":255,"off_b":0}'

# Step 3: From OpenClaw, publish when CI fails/passes
nats pub ci.build.status "0"   # fail -> LED goes red
nats pub ci.build.status "1"   # pass -> LED goes green
```

**Discover all devices:**
```bash
scripts/wc.sh discover
```

**Register relay + motion-activated light:**
```bash
scripts/wc.sh exec wireclaw-01 device_register '{"name":"light","type":"relay","pin":16}'
scripts/wc.sh exec wireclaw-01 device_register '{"name":"motion","type":"nats_value","subject":"home.motion"}'
scripts/wc.sh exec wireclaw-01 rule_create '{"rule_name":"motion light","sensor_name":"motion","condition":"eq","threshold":1,"actuator_name":"light"}'
```

## Notes

- NATS server must be running and accessible from both OpenClaw and WireClaw.
  Default port 4222. Set `WIRECLAW_NATS_URL` env var if non-default.
- Always discover capabilities first if you don't know what's connected.
- Each WireClaw has its own device name and manages its own hardware.
- `remote_chat` invokes another device's LLM. For direct tool execution
  on another device, use `nats req other-device.tool_exec` instead.
