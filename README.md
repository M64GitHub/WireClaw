# WireClaw

An AI agent that lives on a $5 microcontroller and controls real hardware.

**[Flash it to your ESP32 from the browser](https://wireclaw.io/flash.html)** — no tools to install, configure from your phone.

Tell it what you want in plain language - over Telegram, serial, or NATS - and it wires up GPIO pins, reads sensors, switches relays, and sets up automation rules that keep running without the AI. It remembers your preferences across reboots, knows what time it is, and can talk to other WireClaw devices on the network.

```
You:  "When the chip temperature goes above 28, set the LED orange.
       When it drops back down, set it cyan."

WireClaw: "I've created a rule to set the LED to orange when the temperature exceeds 28
           degrees and to cyan when it goes below. If you need any further adjustments
           or assistance, feel free to ask!"

Serial Debug: rule_create(sensor_name="chip_temp", condition="gt", threshold=28,
                       on_action="led_set", on_r=255, on_g=80, on_b=0,
                       off_action="led_set", off_r=0, off_g=255, off_b=255)

       -> Rule created: rule_01 'heat warning' - chip_temp > 28 (every 5s) with auto-off
```

The rule now runs in the main loop. No cloud, no server, no LLM calls. Just the ESP32 checking the sensor every 5 seconds and flipping the LED.

It remembers things too:

```
You:  "My favorite color is blue."

WireClaw: "I've noted that your favorite color is blue. If there's anything else
           you'd like to share or ask, feel free!"

          (writes "User's favorite color is blue." to /memory.txt)

--- reboot, hours later ---

You:  "Set the LED to my favorite color."

WireClaw: "The LED is now set to your favorite color, blue!
           If you need anything, just let me know."

Serial Debug: led_set(r=0, g=0, b=255)
```

The AI persists notes to flash and reloads them into every conversation. Preferences, device nicknames, observations - they survive reboots.

It can also text you:

```
You:  "Send me a Telegram message when chip temperature goes above 40."

WireClaw: "I've created a new rule to send a Telegram message when the temperature exceeds 40°C.
           You will receive a warning message, and another notification when the temperature
           returns to normal. Let me know if you need anything else!"

Serial Debug: rule_create(sensor_name="chip_temp", condition="gt", threshold=40,
                       on_action="telegram",
                       on_telegram_message="Chip is {value}°C - overheating!",
                       off_action="telegram",
                       off_telegram_message="Back to normal at {value}°C.")

       -> Rule created: rule_01 'heat alert' - chip_temp > 40 (every 5s) with auto-off
```

The ESP32 monitors the sensor and sends you a Telegram message the moment the threshold is crossed - with `{value}` replaced by the actual reading - and another when it clears. No LLM in the loop.

It knows what time it is too:

```
You:  "Please send me a telegram message at 10:12 with the chip temperature"

WireClaw: "I've set up a rule to send you a Telegram message with the chip
           temperature at 10:12."

Serial Debug: rule_create(sensor_name="clock_hhmm", condition="eq", threshold=1012,
                       on_action="telegram",
                       on_telegram_message="Chip temp: {chip_temp}°C")

       -> Rule created: rule_01 'Send Telegram at 10:12' - clock_hhmm == 1012 (every 5s)

       [Rule] rule_01 'Send Telegram at 10:12' TRIGGERED (reading=1012, threshold=1012)

Telegram received: "Chip temp: 28.7°C"
```

Time synced via NTP, the `clock_hhmm` sensor encodes time as hour\*100+minute (1012 = 10:12), and `{chip_temp}` is replaced with the live sensor reading when the rule fires. Schedule-based automation with real data - no LLM involved at runtime.

It can also react to data from other systems on the network:

```
You (on Telegram):
    "Register a NATS sensor called room_temp on subject home.room.temp with unit C"

Serial:  device_register({"name":"room_temp","type":"nats_value","subject":"home.room.temp","unit":"C"})
         [NATS] Subscribed 'room_temp' -> home.room.temp (sid=3)
         -> Registered nats_value sensor 'room_temp' on subject 'home.room.temp'
```

The device registry now includes the built-in sensors and the new NATS sensor:

```
> /devices
--- devices ---
  chip_temp [internal_temp] pin=255  = 27.7 C
  clock_hour [clock_hour] pin=255  = 4.0 h
  clock_minute [clock_minute] pin=255  = 11.0 m
  clock_hhmm [clock_hhmm] pin=255  = 411.0
  room_temp [nats_value] nats=home.room.temp  = 0.0 C
---
```

Now any system on the NATS network can push values to it:

```bash
$ nats pub home.room.temp "25.3"
```

```
You (on Telegram):  "whats the room temp?"

Serial:  sensor_read({"name":"room_temp"})
         -> room_temp: 25.3 C

WireClaw: "The current room temperature is 25.3°C."
```

And you can set up rules on it - same as any other sensor:

```
You (on Telegram):
    "please send me a telegram message, when the room temperature reaches 30 degrees"

Serial:  rule_create(sensor_name="room_temp", condition="gt", threshold=30,
                     on_action="telegram",
                     on_telegram_message="Room temperature has reached {value}°C.")
         -> Rule created: rule_01 'Room Temp Alert' - room_temp > 30 (every 5s)
```

```bash
$ nats pub home.room.temp "45.3"
```

```
Serial:  [Rule] rule_01 'Room Temp Alert' TRIGGERED (reading=45, threshold=30)

Telegram received: "Room temperature has reached 45°C."
```

```bash
$ nats pub home.room.temp "25.3"
```

```
Serial:  [Rule] rule_01 'Room Temp Alert' CLEARED (reading=25)
```

Python scripts, Home Assistant, industrial PLCs, other WireClaws - anything that can publish to NATS can feed data into WireClaw's rule engine. The ESP32 handles the reactions: GPIO, LEDs, relays, Telegram alerts.

## How It Works

WireClaw runs two loops on the ESP32:

**The AI loop** handles conversation. When you send a message - via Telegram, serial, or NATS - it calls an LLM (OpenRouter over HTTPS, or a local server like Ollama over HTTP), which responds with tool calls. The AI can read sensors, flip GPIOs, set LEDs, create automation rules, remember things, and talk to other devices. Up to 5 tool-call iterations per message. This is the setup phase.

**The rule loop** runs every iteration of `loop()`, continuously, with no network and no LLM. Each cycle it walks through all enabled rules, reads their sensors, evaluates their conditions, and fires their actions - GPIO writes, LED changes, NATS publishes, Telegram alerts - directly from the microcontroller. Rules persist to flash and survive reboots. This is what runs 24/7.

The AI creates the rules. The rules run without the AI.

Time is synced via NTP on boot. Persistent memory is loaded into every conversation as a system message, so the AI always has context about you and your setup.

**`loop()`** - runs continuously on the ESP32:

1. **`rulesEvaluate()`** - every iteration, no network
   - For each enabled rule: read sensor -> check condition -> fire action
   - Actions: GPIO write, LED set, NATS publish, Telegram alert
2. **AI chat** - triggered by incoming messages
   - Input: Telegram poll / Serial / NATS
   - -> `chatWithLLM()` -> LLM API -> tool calls
   - Tools: `rule_create`, `led_set`, `gpio_write`, `sensor_read`, `remote_chat`, ...

The rule loop and the AI loop share the same `loop()` function but serve different purposes. The rule engine evaluates every cycle regardless of whether anyone is chatting. Multiple rules monitoring the same sensor see the exact same reading per cycle (cached internally), so they always trigger and clear together.

## Try It With Just a Dev Board

All you need is an ESP32-C6 dev board and a USB cable. [Flash it from your browser](https://wireclaw.io/flash.html), connect to the setup AP from your phone, enter your WiFi and API key, and you're up and running.

The ESP32's internal temperature sensor is pre-registered as `chip_temp`, clock sensors provide the current hour and minute, and the onboard RGB LED is available as a rule action. No external sensors needed — here's some examples using only the bare dev board:

### Example: Temperature-Based LED Color

Open serial (or Telegram) and type:

```
Set the LED orange when chip temperature exceeds 28 degrees,
and cyan when it drops below.
```

The AI creates a rule. Behind the scenes:

```
rule_create(
    rule_name     = "heat warning",
    sensor_name   = "chip_temp",
    condition     = "gt",
    threshold     = 28,
    on_action     = "led_set",
    on_r = 255, on_g = 80, on_b = 0,     <- orange when hot
    off_action    = "led_set",
    off_r = 0, off_g = 255, off_b = 255   <- cyan when cool
)
```

Now check it:

```
> /rules
--- rules ---
  rule_01 'heat warning' [ON] chip_temp gt 28 val=31 FIRED
---
```

The rule is running. Warm up the chip (run some WiFi traffic) and watch the LED change. Reboot - the rule persists.

### Example: Telegram Alerts

Get a push notification on your phone when a sensor crosses a threshold:

```
You:  "Alert me on Telegram when chip temp goes above 40, and tell me when it's back to normal."
```

Behind the scenes:

```
rule_create(
    rule_name            = "heat alert",
    sensor_name          = "chip_temp",
    condition            = "gt",
    threshold            = 40,
    on_action            = "telegram",
    on_telegram_message  = "Chip is {value}°C - overheating!",
    off_action           = "telegram",
    off_telegram_message = "Back to normal at {value}°C."
)
```

The ESP32 checks every 5 seconds. The moment it crosses 40, your phone buzzes. When it drops back down, you get the all-clear. No LLM calls, no cloud services - just a direct HTTPS request from the ESP32 to the Telegram API.

You can combine actions too: "Set the LED red AND send me a Telegram message when temperature exceeds 50" creates two rules - one for the LED, one for the alert.

### Example: Scheduled Telegram with Live Sensor Data

Combine time-based rules with message interpolation to get sensor reports at specific times:

```
You:  "Please send me a telegram message at 10:12 with the chip temperature"

WireClaw: "I've set up a rule to send you a Telegram message with the chip
           temperature at 10:12."

Serial Debug: rule_create(rule_name="Send Telegram at 10:12",
                       sensor_name="clock_hhmm", condition="eq", threshold=1012,
                       on_action="telegram",
                       on_telegram_message="Chip temp: {chip_temp}°C")

       -> Rule created: rule_01 'Send Telegram at 10:12' - clock_hhmm == 1012 (every 5s) with auto-off

       [Rule] rule_01 'Send Telegram at 10:12' TRIGGERED (reading=1012, threshold=1012)

Telegram received: "Chip temp: 28.7°C"
```

The `{chip_temp}` in the message was replaced with the live sensor reading (28.7°C) at the moment the rule fired. Use `{value}` for the triggering sensor's reading, or `{device_name}` for any named sensor.

### Example: Time-Based Rules

WireClaw syncs time via NTP on boot. Three virtual sensors - `clock_hour` (0-23), `clock_minute` (0-59), and `clock_hhmm` (hour\*100+minute, e.g. 810 = 08:10) - let you create schedule-based automation. Edge-triggered - fires once when the time matches, not repeatedly.

For exact times (hour and minute), use the `clock_hhmm` sensor which encodes time as `hour*100+minute`:

```
You:  "Can you set the led to purple at 08:41 please?"

WireClaw: "I've set a rule to turn the LED purple at 08:41.
           It will automatically turn off afterward."

Serial Debug: rule_create(rule_name="LED to Purple at 08:41",
                       sensor_name="clock_hhmm", condition="eq", threshold=841,
                       on_action="led_set", on_r=128, on_g=0, on_b=128,
                       off_action="led_set", off_r=0, off_g=0, off_b=0)

       -> Rule created: rule_01 'LED to Purple at 08:41' - clock_hhmm == 841 (every 5s) with auto-off

       [Rule] rule_01 'LED to Purple at 08:41' TRIGGERED (reading=841, threshold=841)
       [Rule] rule_01 'LED to Purple at 08:41' CLEARED (reading=842)
```

The threshold 841 means 08:41. Similarly, 1830 = 18:30, 2200 = 22:00. Edge-triggered: fires when the minute matches, auto-off clears when it passes.

For periodic tasks, use `condition="always"` which fires every interval:

```
You:  "Send me a Telegram every 2 minutes saying 'heartbeat'."
```

Behind the scenes:

```
rule_create(
    rule_name            = "heartbeat",
    sensor_name          = "chip_temp",
    condition            = "always",
    interval_seconds     = 120,
    on_action            = "telegram",
    on_telegram_message  = "heartbeat"
)
```

Verify with `/time`:

```
> /time
2026-02-10 19:34:12 (TZ=CET-1CEST,M3.5.0,M10.5.0/3)
```

### Example: Persistent Memory

Tell the AI something, and it remembers - even across reboots:

```
You:  "My favorite color is blue."
AI:   "I've noted that your favorite color is blue."
      -> file_write(path="/memory.txt", content="User's favorite color is blue.")
```

Check it on serial:

```
> /memory
--- memory (30 bytes) ---
User's favorite color is blue.
---
```

Later (even after a power cycle):

```
You:  "Set the LED to my favorite color."
AI:   "The LED is now set to your favorite color, blue!"
      -> led_set(r=0, g=0, b=255)
```

The AI recalled "blue" from its persistent memory without being told again. It stores user preferences, device nicknames, and observations in `/memory.txt`, which is loaded into every conversation automatically.

### Example: Register an External Sensor + Actuator

If you wire up an NTC thermistor on pin 4 and a relay on pin 16:

```
You:  "Register an NTC thermistor on pin 4 called 'temperature', unit is C"
AI:   device_register(name="temperature", type="ntc_10k", pin=4, unit="C")
      -> Registered sensor 'temperature' on pin 4

You:  "Register a relay on pin 16 called 'fan', it uses inverted logic"
AI:   device_register(name="fan", type="relay", pin=16, inverted=true)
      -> Registered actuator 'fan' on pin 16

You:  "Turn on the fan when temperature exceeds 28"
AI:   rule_create(rule_name="cool down", sensor_name="temperature",
                  condition="gt", threshold=28, actuator_name="fan")
      -> Rule created: rule_01 'cool down' - temperature > 28 (every 5s) with auto-off
```

Devices and rules persist to flash. After a reboot:

```
> /devices
--- devices ---
  chip_temp [internal_temp] pin=255  = 27.3 C
  clock_hour [clock_hour] pin=255  = 19 h
  clock_minute [clock_minute] pin=255  = 34 m
  clock_hhmm [clock_hhmm] pin=255  = 1934
  temperature [ntc_10k] pin=4  = 24.1 C
  fan [relay] pin=16 (inverted)
---

> /rules
--- rules ---
  rule_01 'cool down' [ON] temperature gt 28 val=24 idle
---
```

Everything is restored. The fan rule is watching the temperature and will fire when it crosses 28.

## Features

- **Rule Engine** - persistent local automation, evaluated every loop iteration, edge-triggered or periodic
- **Time-Aware Rules** - NTP sync with POSIX timezone, `clock_hour`, `clock_minute`, and `clock_hhmm` virtual sensors for schedule-based automation
- **Persistent Memory** - AI remembers user preferences, device nicknames, and observations across reboots
- **Telegram Alerts** - rules send push notifications with live sensor values via `{device_name}` interpolation, no LLM in the loop
- **Device Registry** - named sensors and actuators instead of raw pin numbers, persisted to flash
- **AI Agent** - agentic loop with 18 tools, up to 5 iterations per message
- **Local LLM** - use a local server (Ollama, llama.cpp) over HTTP instead of cloud API
- **Multi-Device Mesh** - devices talk to each other over NATS via `remote_chat`
- **Telegram Bot** - chat with your ESP32 from your phone
- **NATS Virtual Sensors** - subscribe to any NATS subject as a sensor, trigger rules from external systems (Python, Home Assistant, PLCs, other WireClaws)
- **NATS Integration** - device-to-device messaging, commands, and rule-triggered events
- **Serial Interface** - local chat and commands over USB (115200 baud)
- **Conversation History** - 6-turn circular buffer, persisted across reboots

## Hardware

- **Tested on:** ESP32-C6 (WaveShare DevKit, 8MB flash)
- **Compatible:** Any ESP32 with WiFi (C3, C6, S3, S2, classic ESP32)
- **Platform:** [pioarduino](https://github.com/pioarduino/platform-espressif32) via PlatformIO
- **Requirements:** WiFi network, [OpenRouter](https://openrouter.ai/) API key or local LLM server

Onboard RGB LED control works out of the box on Espressif DevKit boards with WS2812B (C3, C6, S3). Boards without an onboard RGB LED can skip the `led_set` tool - everything else works the same.

The dev board alone is enough to get started - chip temperature sensor, clock sensors, and RGB LED work out of the box. Add external sensors and actuators as needed.

## Quick Start

### Option A: Setup Portal (no CLI needed)

Flash the firmware from your browser and configure from your phone:

1. Go to **[wireclaw.io/flash.html](https://wireclaw.io/flash.html)** and click **Flash Now** (requires Chrome/Edge with WebSerial)
2. The ESP32 boots, finds no WiFi config, and starts an open AP called **WireClaw-Setup**
3. Connect to the AP from your phone — a setup page opens automatically
4. Fill in your WiFi credentials, API key, and any optional settings
5. Hit **Save & Reboot** — the device connects to your network and is ready to use

The setup portal also activates if WiFi connection fails (wrong password, network down). The LED pulses cyan while the portal is active.

To reconfigure later, type `/setup` in the serial monitor to re-enter the portal at any time.

### Option B: Manual Config (PlatformIO)

#### 1. Install PlatformIO

```
pip install platformio
```

#### 2. Configure

```
cp data/config.json.example data/config.json
```

Edit `data/config.json`:

```json
{
  "wifi_ssid": "YourNetwork",
  "wifi_pass": "YourPassword",
  "api_key": "sk-or-v1-your-openrouter-api-key",
  "model": "openai/gpt-4o-mini",
  "device_name": "wireclaw-01",
  "api_base_url": "",
  "nats_host": "",
  "nats_port": "4222",
  "telegram_token": "",
  "telegram_chat_id": "",
  "telegram_cooldown": "60",
  "timezone": "CET-1CEST,M3.5.0,M10.5.0/3"
}
```

Leave `telegram_token` empty to disable Telegram. Leave `nats_host` empty to disable NATS. Leave `api_base_url` empty to use OpenRouter (default).

For Telegram: create a bot via [@BotFather](https://t.me/BotFather), get your chat ID from [@userinfobot](https://t.me/userinfobot).

For a local LLM: set `api_base_url` to your server's OpenAI-compatible endpoint, e.g. `http://192.168.1.50:11434/v1/chat/completions` for Ollama.

#### 3. Build and Flash

```
pio run -t uploadfs    # upload config + system prompt to filesystem
pio run -t upload      # flash firmware
pio device monitor     # connect via serial (115200 baud)
```

Type a message and press Enter. Or open Telegram and text your bot.

## Setup Portal

When WireClaw has no WiFi configuration — or can't connect to the configured network — it automatically enters setup mode:

1. Starts an open WiFi access point: **WireClaw-Setup**
2. Runs a captive portal on 192.168.4.1 (phones open this automatically)
3. Serves a config form with all settings (WiFi, API key, model, NATS, Telegram, timezone)
4. On submit, writes `config.json` to flash and reboots into normal operation

The portal times out after 5 minutes and reboots to retry. The LED pulses cyan while the portal is active.

### When the portal activates

| Condition | What happens |
|-----------|--------------|
| No `wifi_ssid` in config (or no config.json at all) | Portal starts immediately on boot |
| WiFi connection fails after retries | Portal starts instead of rebooting blindly |
| `/setup` typed in serial monitor | Portal starts on demand (for reconfiguration) |

### Form fields

| Field | Required | Default |
|-------|----------|---------|
| WiFi SSID | Yes | — |
| WiFi Password | Yes | — |
| OpenRouter API Key | No* | — |
| Model | No | `openai/gpt-4o-mini` |
| Device Name | No | `wireclaw-01` |
| API Base URL | No | — |
| NATS Host / Port | No | — / `4222` |
| Telegram Token / Chat ID | No | — |
| Timezone | No | `UTC0` |

\* Required unless using a local LLM via API Base URL.

## Device Registry

Named sensors and actuators that the AI and rule engine can reference by name.

### Supported Device Types

| Type | Kind | Description |
|------|------|-------------|
| `digital_in` | Sensor | `digitalRead()` - 0 or 1 |
| `analog_in` | Sensor | `analogRead()` - raw ADC value 0-4095 |
| `ntc_10k` | Sensor | NTC thermistor - converts to Celsius (B=3950) |
| `ldr` | Sensor | Light-dependent resistor - rough lux estimate |
| `internal_temp` | Sensor | ESP32 chip temperature (no pin, virtual) |
| `clock_hour` | Sensor | Current hour 0-23 via NTP (no pin, virtual) |
| `clock_minute` | Sensor | Current minute 0-59 via NTP (no pin, virtual) |
| `clock_hhmm` | Sensor | Time as hour\*100+minute, e.g. 1830 = 18:30 (no pin, virtual) |
| `nats_value` | Sensor | Value received from a NATS subject (no pin, virtual) |
| `digital_out` | Actuator | `digitalWrite()` - HIGH or LOW |
| `relay` | Actuator | `digitalWrite()` with optional inverted logic |
| `pwm` | Actuator | `analogWrite()` - 0-255 |

`chip_temp`, `clock_hour`, `clock_minute`, and `clock_hhmm` are auto-registered on first boot. All other devices are registered through conversation with the AI.

Devices persist to `/devices.json` on flash.

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
```

### Behavior

- **Edge-triggered** - conditions `gt`, `lt`, `eq`, `neq`, `change` fire once on threshold crossing, not repeatedly. When the condition clears, the off action runs (if configured).
- **Periodic** - condition `always` fires every interval, repeatedly. Use for heartbeats, periodic reports, scheduled tasks.
- **Auto-off** - when using `actuator_name` or `off_action`, the reverse action runs when the condition clears
- **Interval** - configurable per rule (default 5 seconds)
- **Telegram cooldown** - per-rule cooldown prevents message spam when sensor oscillates around threshold (configurable via `telegram_cooldown` in config.json, default 60s, 0 = disabled)
- **Message interpolation** - `{value}` in telegram/NATS messages is replaced with the triggering sensor's reading; `{device_name}` (e.g. `{chip_temp}`) reads any named sensor live at fire time; `{name:msg}` inserts the message string from a NATS virtual sensor's last JSON payload
- **Sensor caching** - all rules monitoring the same sensor see the same value per evaluation cycle
- **NATS events** - every rule trigger publishes to `{device_name}.events`
- **Persistence** - rules survive reboots (`/rules.json`)
- **IDs** - auto-assigned: `rule_01`, `rule_02`, etc.

## Persistent Memory

The AI persists notes to `/memory.txt` on flash and reloads them into every conversation as a system message. This lets it remember user preferences, device nicknames, and observations across reboots - without using up conversation history slots.

The AI decides autonomously what's worth remembering. Tell it "my favorite color is blue" and it writes that to memory. Ask "set the LED to my favorite color" a week later and it knows what to do.

```
> /memory
--- memory (30 bytes) ---
User's favorite color is blue.
---
```

The memory file is limited to 512 characters. The AI is instructed to keep it concise. You can also read or write `/memory.txt` directly using the `file_read` and `file_write` tools, or upload it with `pio run -t uploadfs`.

## Local LLM

By default WireClaw uses [OpenRouter](https://openrouter.ai/) (cloud, HTTPS). Set `api_base_url` to point to a local LLM server instead - no internet or API key required.

```json
{
  "api_base_url": "http://192.168.1.50:11434/v1/chat/completions",
  "model": "gpt-oss:latest",
  "api_key": ""
}
```

**Recommended local model:** [gpt-oss](https://ollama.com/library/gpt-oss) (OpenAI's open-weight model) - it has native tool calling support and handles WireClaw's 18-tool schema reliably. The `:latest` tag (20B, 13GB) is a good balance of speed and quality. [Qwen3](https://ollama.com/library/qwen3) models also work well.

HTTP mode skips TLS, saving significant RAM during LLM calls. The server must support OpenAI-compatible chat completions with tool calling.

Works with [Ollama](https://ollama.com/), [llama.cpp](https://github.com/ggerganov/llama.cpp) server, or any OpenAI-compatible endpoint. Leave `api_base_url` empty to use OpenRouter.

## LLM Tools

18 tools available to the AI:

| Tool | Description |
|------|-------------|
| **Hardware** | |
| `led_set` | Set RGB LED color (r, g, b: 0-255) |
| `gpio_write` | Set a GPIO pin HIGH or LOW |
| `gpio_read` | Read digital state of a GPIO pin |
| `temperature_read` | Read chip temperature (Celsius) |
| **Device Registry** | |
| `device_register` | Register a named sensor or actuator |
| `device_list` | List all devices with current readings |
| `device_remove` | Remove a device by name |
| `sensor_read` | Read a named sensor (returns value + unit) |
| `actuator_set` | Set a named actuator (0/1 or 0-255 for PWM) |
| **Rule Engine** | |
| `rule_create` | Create an automation rule |
| `rule_list` | List rules with status and last readings |
| `rule_delete` | Delete a rule by ID |
| `rule_enable` | Enable/disable a rule without deleting |
| **System** | |
| `device_info` | Heap, uptime, WiFi, chip info |
| `file_read` | Read a file from LittleFS |
| `file_write` | Write a file to LittleFS |
| `nats_publish` | Publish to a NATS subject |
| `remote_chat` | Send a message to another WireClaw device via NATS |

## Serial Commands

| Command | Description |
|---------|-------------|
| `/status` | Device status (WiFi, heap, NATS, uptime) |
| `/devices` | List registered devices with readings |
| `/rules` | List automation rules with status |
| `/memory` | Show AI persistent memory |
| `/time` | Show current time and timezone |
| `/config` | Show loaded configuration |
| `/prompt` | Show system prompt |
| `/history` | Show conversation history |
| `/clear` | Clear conversation history |
| `/heap` | Show free memory |
| `/debug` | Toggle debug output |
| `/setup` | Start WiFi setup portal for reconfiguration |
| `/reboot` | Restart ESP32 |
| `/help` | List commands |

## NATS Integration

When `nats_host` is configured, the device subscribes to:

| Subject | Description |
|---------|-------------|
| `{device_name}.chat` | Request/reply - send a message, get LLM response |
| `{device_name}.cmd` | Commands: status, clear, heap, debug, devices, rules, memory, time, reboot |
| `{device_name}.events` | Published events: online, rule triggers, chat responses |

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
- 14 NATS subscription slots available (16 max minus 2 for chat + cmd)

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

## Configuration

| Field | Description |
|-------|-------------|
| `wifi_ssid` | WiFi network name |
| `wifi_pass` | WiFi password |
| `api_key` | [OpenRouter](https://openrouter.ai/) API key (empty if using local LLM) |
| `model` | LLM model (e.g. `openai/gpt-4o-mini`, `gpt-oss:latest`) |
| `device_name` | Device name, used as NATS subject prefix |
| `api_base_url` | LLM endpoint URL (empty = OpenRouter, `http://...` for local LLM) |
| `nats_host` | NATS server hostname (empty = disabled) |
| `nats_port` | NATS server port (default: 4222) |
| `telegram_token` | Telegram bot token from [@BotFather](https://t.me/BotFather) (empty = disabled) |
| `telegram_chat_id` | Allowed Telegram chat ID |
| `telegram_cooldown` | Minimum seconds between Telegram messages per rule (default: 60, 0 = disabled) |
| `timezone` | POSIX TZ string for NTP time sync (default: `UTC0`) |

Edit `data/system_prompt.txt` to customize the AI's personality and instructions.

### Timezone Examples

| Region | TZ String |
|--------|-----------|
| UTC | `UTC0` |
| Central Europe (with DST) | `CET-1CEST,M3.5.0,M10.5.0/3` |
| US Eastern (with DST) | `EST5EDT,M3.2.0,M11.1.0` |
| US Pacific (with DST) | `PST8PDT,M3.2.0,M11.1.0` |
| Japan | `JST-9` |

### Runtime Data

Created automatically on flash, persisted across reboots:

| File | Contents |
|------|----------|
| `/devices.json` | Registered sensors and actuators |
| `/rules.json` | Automation rules |
| `/history.json` | Conversation history (6 turns) |
| `/memory.txt` | AI persistent memory (preferences, notes) |

## Resource Usage

```
RAM:   55.4% (181KB of 320KB)
Flash: 37.4% (1.2MB of 3.3MB)
```

Static allocations: device registry (768B), rule engine (6.2KB), LLM request buffer (20KB), conversation history, persistent memory (512B), TLS stack. Setup portal HTML is stored in flash (PROGMEM), not RAM.

## License

MIT

---

[wireclaw.io](https://wireclaw.io)
