# WireClaw

An AI agent that lives on a $5 microcontroller and controls real hardware.

**Supported chips:** ESP32-C6, ESP32-S3, ESP32-C3 (4 MB flash required)

**[Flash it to your ESP32 from the browser](https://wireclaw.io/flash.html)** - no tools to install, configure from your phone. The web flasher auto-detects your chip.

Tell it what you want in plain language - over Telegram, serial, or NATS - and it wires up GPIO pins, reads sensors, switches relays, and sets up automation rules that keep running without the AI. It remembers your preferences across reboots, knows what time it is, can talk to other WireClaw devices on the network, and bridges to any serial device - Arduinos, GPS modules, CO2 sensors, RFID readers - over UART.

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

It can also talk to other microcontrollers over serial:

```
You (on Telegram):
    "Set up a serial connection to an Arduino at 9600 baud, call it arduino"

Serial:  device_register({"name":"arduino","type":"serial_text","baud":9600})
         SerialText: UART1 at 9600 baud (RX=4 TX=5)
         -> Registered serial_text sensor 'arduino' at 9600 baud (RX=4 TX=5)
```

Now WireClaw can send commands and receive data over the UART:

```
You:  "Ask the Arduino for a temperature reading"

Serial:  serial_send({"text":"GET_TEMP"})
         -> Sent to serial: GET_TEMP

         [SerialText] '23.5' -> val=23.5 msg='23.5'

         sensor_read({"name":"arduino"})
         -> arduino: 23.5  (last: '23.5')

WireClaw: "The Arduino reports a temperature of 23.5°C."
```

And rules work on serial data just like any other sensor:

```
You:  "Alert me on Telegram when the Arduino reading goes above 30."

Serial:  rule_create(sensor_name="arduino", condition="gt", threshold=30,
                     on_action="telegram",
                     on_telegram_message="Arduino temp: {value}°C - {arduino:msg}")

         -> Rule created: rule_01 'arduino alert' - arduino > 30 (every 5s)
```

GPS modules, CO2 sensors, RFID readers, custom Arduinos - anything with a serial port becomes a sensor that feeds into WireClaw's rule engine. The AI sets it up; the rules run without it.

## How It Works

WireClaw runs two loops on the ESP32:

**The AI loop** handles conversation. When you send a message - via Telegram, serial, or NATS - it calls an LLM (OpenRouter over HTTPS, or a local server like Ollama over HTTP), which responds with tool calls. The AI can read sensors, flip GPIOs, set LEDs, create automation rules, remember things, and talk to other devices. Up to 5 tool-call iterations per message. This is the setup phase.

**The rule loop** runs every iteration of `loop()`, continuously, with no network and no LLM. Each cycle it walks through all enabled rules, reads their sensors, evaluates their conditions, and fires their actions - GPIO writes, LED changes, NATS publishes, Telegram alerts - directly from the microcontroller. Rules persist to flash and survive reboots. This is what runs 24/7.

The AI creates the rules. The rules run without the AI.

Time is synced via NTP on boot. Persistent memory is loaded into every conversation as a system message, so the AI always has context about you and your setup.

**`loop()`** - runs continuously on the ESP32:

1. **`rulesEvaluate()`** - every iteration, no network
   - For each enabled rule: read sensor -> check condition -> fire action
   - Actions: GPIO write, LED set, NATS publish, Telegram alert, serial send
   - **`serialTextPoll()`** - reads incoming UART bytes, stores last complete line
2. **AI chat** - triggered by incoming messages
   - Input: Telegram poll / Serial / NATS
   - -> `chatWithLLM()` -> LLM API -> tool calls
   - Tools: `rule_create`, `led_set`, `gpio_write`, `sensor_read`, `serial_send`, `remote_chat`, ...

The rule loop and the AI loop share the same `loop()` function but serve different purposes. The rule engine evaluates every cycle regardless of whether anyone is chatting. Multiple rules monitoring the same sensor see the exact same reading per cycle (cached internally), so they always trigger and clear together.

## Try It With Just a Dev Board

All you need is an ESP32-C6 dev board and a USB cable. [Flash it from your browser](https://wireclaw.io/flash.html), connect to the setup AP from your phone, enter your WiFi and API key, and you're up and running.

The ESP32's internal temperature sensor is pre-registered as `chip_temp`, clock sensors provide the current hour and minute, and the onboard RGB LED is available as a rule action. No external sensors needed - here's some examples using only the bare dev board:

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

### Example: Rule Chaining

Rules can trigger other rules with delays, creating multi-step sequences. The `chain_create` tool handles everything in one call:

```
You:  "please send me a telegram message when the test sensor > 100,
       then wait 5s, then send me another message "hello test",
       then set the led to green, and after another 10s set the led off"

WireClaw: "Perfect! I've created the automation chain (rule_08)."

Serial Debug:
  chain_create(sensor_name="test", condition="gt", threshold=100,
    step1_action="telegram", step1_message="Test sensor exceeded 100: {value}",
    step2_action="telegram", step2_delay=5, step2_message="hello test",
    step3_action="led_set", step3_delay=0, step3_r=0, step3_g=255, step3_b=0,
    step4_action="led_set", step4_delay=10, step4_r=0, step4_g=0, step4_b=0)

  -> Chain created: rule_08 test>100 -> telegram -> 5s -> telegram -> LED(0,255,0) -> 10s -> LED(0,0,0)
```

When the sensor crosses the threshold:

```
[Rule] rule_08 'test step1' TRIGGERED (reading=1000, threshold=100)

--- 5 seconds later ---

[Rule] rule_07 'test step2' CHAIN-TRIGGERED
[Rule] rule_06 'test step3' CHAIN-TRIGGERED

--- 10 seconds later ---

[Rule] rule_05 'test step4' CHAIN-TRIGGERED

--- sensor drops back below threshold ---

[Rule] rule_08 'test step1' CLEARED (reading=10)
```

One tool call creates 4 linked rules. Delays are non-blocking. The chain fires again the next time the sensor crosses the threshold. See [docs/RULE-CHAINING.md](docs/RULE-CHAINING.md) for the full reference.

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
- **Rule Chaining** - `chain_create` tool builds multi-step sequences in one call (e.g. alert + LED change + auto-off), with non-blocking delays
- **Time-Aware Rules** - NTP sync with POSIX timezone, `clock_hour`, `clock_minute`, and `clock_hhmm` virtual sensors for schedule-based automation
- **Persistent Memory** - AI remembers user preferences, device nicknames, and observations across reboots
- **Telegram Alerts** - rules send push notifications with live sensor values via `{device_name}` interpolation, no LLM in the loop
- **Device Registry** - named sensors and actuators instead of raw pin numbers, persisted to flash
- **Serial Bridge** - connect any serial device (Arduino, GPS, CO2 sensor) via UART1; read data as a sensor, send commands via `serial_send`, use in rules with `{name:msg}` interpolation
- **AI Agent** - agentic loop with 20 tools, up to 5 iterations per message
- **Local LLM** - use a local server (Ollama, llama.cpp) over HTTP instead of cloud API
- **OpenClaw Integration** - [OpenClaw](https://github.com/openclaw) (or any NATS client) can execute tools directly on the ESP32 without involving WireClaw's LLM. Flat JSON protocol, device discovery, 19 tools available. Includes a skill and wrapper script.
- **Multi-Device Mesh** - devices talk to each other over NATS via `remote_chat`
- **Telegram Bot** - chat with your ESP32 from your phone
- **NATS Virtual Sensors** - subscribe to any NATS subject as a sensor, trigger rules from external systems (Python, Home Assistant, PLCs, other WireClaws)
- **NATS Integration** - device-to-device messaging, commands, and rule-triggered events
- **Web Config Portal** - browser-based UI at `http://<device-ip>/` for editing config, system prompt, memory, and viewing device status. mDNS: `http://<device-name>.local/`
- **Serial Interface** - local chat and commands over USB (115200 baud)
- **Conversation History** - 4-turn circular buffer, persisted across reboots

## Hardware

- **Supported:** ESP32-C6, ESP32-S3, ESP32-C3
- **Platform:** [pioarduino](https://github.com/pioarduino/platform-espressif32) via PlatformIO
- **Requirements:** WiFi network, [OpenRouter](https://openrouter.ai/) API key or local LLM server

Onboard RGB LED control works out of the box on Espressif DevKit boards with WS2812B (C3, C6, S3). Boards without an onboard RGB LED can skip the `led_set` tool - everything else works the same.

The dev board alone is enough to get started - chip temperature sensor, clock sensors, and RGB LED work out of the box. Add external sensors and actuators as needed.

## Quick Start

**4 MB flash required** - smaller chips do not work with the OTA partition table and web installer.

### Option A: Setup Portal (no CLI needed)

Flash the firmware from your browser and configure from your phone:

1. Go to **[wireclaw.io/flash.html](https://wireclaw.io/flash.html)** and click **Flash Now** (requires Chrome/Edge with WebSerial)
2. The ESP32 boots, finds no WiFi config, and starts an open AP called **WireClaw-Setup**
3. Connect to the AP from your phone - a setup page opens automatically
4. Fill in your WiFi credentials, API key, and any optional settings
5. Hit **Save & Reboot** - the device connects to your network and is ready to use

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
  "model": "google/gemini-2.5-flash",
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

#### Tested Models

Models tested with tool calling and chain reasoning:

| Model | Response | Chain Reasoning | Notes |
|-------|----------|----------------|-------|
| Gemini 2.5 Flash | ~4s | Excellent | Numbered list, plain text. |
| GPT-OSS-120B | ~8s | Excellent | Conversational numbered steps. |
| Claude Sonnet 4.5 | ~10s | Excellent | Detailed markdown with bold labels. |
| GPT-5 Mini | ~16s | Excellent | One-line summary. Verbose tool calls (sends all defaults). |
| DeepSeek V3.2 | ~44s | Excellent | Thinking model. Correct but very slow. |
| Aurora Alpha | ~4s | Basic | Fast but inconsistent delay reasoning across runs. |
| GPT-4o Mini | | Basic | May misinterpret delays in chain steps. |
| Claude 3 Haiku | ~7s | Basic | May misinterpret delays in chain steps. |
| DeepSeek V3 | ~14s | Basic | May misinterpret delays in chain steps. |
| Gemini 2.5 Flash Lite | ~4s | Basic | May misinterpret delays in chain steps. |
| Qwen 3 Coder | ~6s | Basic | May misinterpret delays in chain steps. |
| Qwen 2.5 7B | ~10s | Fail | Missing steps, wrong delays, bad template syntax. |

See [docs/RULE-CHAINING.md](docs/RULE-CHAINING.md#appendix-a-model-comparison) for detailed output from each model.

#### 3. Build and Flash

The default target is ESP32-C6. To build for a different chip, pass `-e <target>`:

| Target | Board | Command |
|--------|-------|---------|
| ESP32-C6 | esp32-c6-devkitc-1 | `pio run` (default) |
| ESP32-S3 | esp32-s3-devkitc-1 | `pio run -e esp32-s3` |
| ESP32-C3 | esp32-c3-devkitm-1 | `pio run -e esp32-c3` |

```
pio run -t uploadfs    # upload config + system prompt to filesystem
pio run -t upload      # flash firmware
pio device monitor     # connect via serial (115200 baud)
```

For non-default targets, add `-e <target>` to each command (e.g. `pio run -e esp32-s3 -t upload`).

Type a message and press Enter. Or open Telegram and text your bot.

## Setup Portal

When WireClaw has no WiFi configuration - or can't connect to the configured network - it automatically enters setup mode:

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
| WiFi SSID | Yes | - |
| WiFi Password | Yes | - |
| OpenRouter API Key | No* | - |
| Model | No | `openai/gpt-4o-mini` |
| Device Name | No | `wireclaw-01` |
| API Base URL | No | - |
| NATS Host / Port | No | - / `4222` |
| Telegram Token / Chat ID | No | - |
| Timezone | No | `UTC0` |

\* Required unless using a local LLM via API Base URL.

To change configuration after initial setup without reflashing, use the **Web Config Portal** (see below).

## Web Config Portal

After initial setup, you can change any configuration from your browser - no USB cable or reflashing needed.

Open `http://<device-ip>/` or `http://<device-name>.local/` (mDNS) from any device on the same network. The URL is printed to serial and Telegram on every boot.

### Tabs

| Tab | What it does |
|-----|-------------|
| **Config** | Edit all 12 config fields (WiFi, API key, model, NATS, Telegram, timezone). Sensitive fields are masked. **Requires reboot to apply.** |
| **System Prompt** | Edit the AI's personality and instructions. **Applied immediately**, no reboot needed. |
| **Memory** | Edit the AI's persistent memory (`/memory.txt`). Active on next conversation. |
| **Status** | Version, uptime, heap, WiFi SSID/IP/RSSI, model, NATS/Telegram status. Refresh and reboot buttons. |

### REST API

For scripting and automation, the web config exposes a JSON/text API:

| Endpoint | Method | Content-Type | Description |
|----------|--------|-------------|-------------|
| `/api/config` | GET | application/json | Current config (sensitive fields masked) |
| `/api/config` | POST | application/json | Merge with existing config, write to flash |
| `/api/prompt` | GET | text/plain | Current system prompt |
| `/api/prompt` | POST | text/plain | Update system prompt (live, no reboot) |
| `/api/memory` | GET | text/plain | AI memory contents |
| `/api/memory` | POST | text/plain | Update AI memory |
| `/api/status` | GET | application/json | Device status (version, uptime, heap, WiFi, etc.) |
| `/api/reboot` | POST | - | Reboot the device |

Example:

```bash
# Read status
curl http://wireclaw-01.local/api/status

# Update system prompt
curl -X POST -H "Content-Type: text/plain" \
  -d "You are a helpful assistant." \
  http://wireclaw-01.local/api/prompt

# Update config (only changed fields - masked values are preserved)
curl -X POST -H "Content-Type: application/json" \
  -d '{"model":"google/gemini-2.5-flash"}' \
  http://wireclaw-01.local/api/config
```

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
| `serial_text` | Sensor | Text lines from UART1 serial port (no pin, virtual, one max) |
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
| `chained` | Only fires when triggered by another rule's chain (see [Rule Chaining](docs/RULE-CHAINING.md)) |

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
- **Sensor caching** - all rules monitoring the same sensor see the same value per evaluation cycle
- **NATS events** - every rule trigger publishes to `{device_name}.events`
- **Persistence** - rules survive reboots (`/rules.json`)
- **Rule chaining** - `chain_create` builds multi-step sequences in one call (up to 5 steps with delays). Internally creates linked rules with `condition="chained"` that only fire via chain. For advanced use (OFF-chains, manual linking), `rule_create` with `chain_rule`/`chain_off_rule` is also available. Max depth 8. See [docs/RULE-CHAINING.md](docs/RULE-CHAINING.md)
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

19 tools available to the AI:

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
| `serial_send` | Send text over serial_text UART |
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
| `/model` | Show current LLM model, or `/model <name>` to switch at runtime |
| `/setup` | Start WiFi setup portal for reconfiguration |
| `/reboot` | Restart ESP32 |
| `/help` | List commands |

## NATS Integration

When `nats_host` is configured, the device subscribes to:

| Subject | Description |
|---------|-------------|
| `{device_name}.chat` | Request/reply - send a message, get LLM response |
| `{device_name}.cmd` | Commands: status, clear, heap, debug, devices, rules, memory, time, reboot |
| `{device_name}.events` | Published events: online, rule triggers, tool_exec results |
| `{device_name}.tool_exec` | Request/reply - execute a tool directly (no LLM), returns JSON result |
| `{device_name}.capabilities` | Request/reply - query devices, rules, tools, version |
| `_wc.discover` | Request/reply - discover all WireClaw devices on the network |

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
- 11 NATS subscription slots available (16 max minus 5 for chat, cmd, tool_exec, capabilities, discover)

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

## OpenClaw Integration

[OpenClaw](https://github.com/openclaw) is an open-source AI agent that controls your digital life — email, calendar, GitHub, shell. WireClaw controls the physical world — LEDs, relays, GPIO pins, sensors. This integration connects them over NATS: OpenClaw executes tools directly on WireClaw's hardware without involving WireClaw's own LLM. One LLM call, not two. Fast, cheap, no telephone game between models.

Cross-domain examples that neither agent can do alone:

- CI fails on GitHub → desk LED goes red
- Temperature > 35°C → Slack message via OpenClaw
- Calendar empty → office lights off via relay
- Every morning → LED shows today's weather color

### Protocol

Flat JSON over NATS request/reply. The `"tool"` field names the tool, all other fields are its parameters at the same level:

```bash
$ nats req wireclaw-01.tool_exec '{"tool":"led_set","r":0,"g":255,"b":0}'
{"ok":true,"result":"LED set to RGB(0, 255, 0)"}

$ nats req wireclaw-01.tool_exec '{"tool":"sensor_read","name":"chip_temp"}'
{"ok":true,"result":"chip_temp: 33.2 C"}

$ nats req wireclaw-01.tool_exec '{"tool":"nonexistent"}'
{"ok":false,"error":"unknown tool 'nonexistent'"}
```

The key is `"tool"` (not `"name"`) to avoid collision with tools that have a `"name"` parameter like `device_register`. This works because WireClaw's internal tool handlers use `strstr`-based parsing — they find their keys anywhere in the JSON string, ignore the rest.

### Discovery

Find all WireClaw devices on the network:

```bash
$ nats req _wc.discover "" --replies=0 --timeout=3s
{"device":"wireclaw-01","version":"0.4.0","free_heap":81664,
 "tools":["led_set","gpio_write",...],
 "devices":[{"name":"chip_temp","kind":"internal_temp","value":33.2,"unit":"C"},
            {"name":"clock_hour","kind":"clock_hour","value":12.0,"unit":"h"},
            ...],
 "rules":[]}
```

Query a specific device:

```bash
$ nats req wireclaw-01.capabilities ""
```

Returns: device name, firmware version, free heap, registered sensors/actuators with current values, active rules with status, and all available tool names.

### Tool Execution

All 19 tools are available via `tool_exec` (except `remote_chat` — see [Security](#security-1)):

```bash
# Set LED color
nats req wireclaw-01.tool_exec '{"tool":"led_set","r":255,"g":0,"b":128}'

# Read a sensor
nats req wireclaw-01.tool_exec '{"tool":"sensor_read","name":"chip_temp"}'

# Create a persistent rule that runs 24/7 on the ESP32
nats req wireclaw-01.tool_exec '{"tool":"rule_create","rule_name":"heat led","sensor_name":"chip_temp","condition":"gt","threshold":32,"on_action":"led_set","on_r":255,"on_g":100,"on_b":0,"off_action":"led_set","off_r":0,"off_g":255,"off_b":255}'

# List all devices
nats req wireclaw-01.tool_exec '{"tool":"device_list"}'
```

Rules created via `tool_exec` persist on the ESP32 and run 24/7 — even when OpenClaw, the NATS server, or WiFi is offline.

### Cross-Domain Automation

Three patterns for combining digital and physical automation:

**Digital trigger → physical action.** OpenClaw detects something digital (CI failure, calendar event, email) and calls a tool directly:

```bash
nats req wireclaw-01.tool_exec '{"tool":"led_set","r":255,"g":0,"b":0}'
```

**Physical trigger → digital action.** Create a WireClaw rule that publishes to NATS when a sensor threshold is crossed. OpenClaw subscribes to that subject and triggers digital workflows:

```bash
# On WireClaw: publish temperature to NATS when it exceeds 35°C
nats req wireclaw-01.tool_exec '{"tool":"rule_create","rule_name":"temp alert","sensor_name":"chip_temp","condition":"gt","threshold":35,"on_action":"nats_publish","on_nats_subject":"alerts.overheating","on_nats_payload":"Temperature: {value}C"}'

# On OpenClaw: subscribe and react
nats sub alerts.overheating
```

**External data → WireClaw sensor.** The most powerful pattern — OpenClaw pushes data to a NATS subject, WireClaw has a `nats_value` sensor on it, and persistent rules handle the rest autonomously:

```bash
# Step 1: Register a NATS virtual sensor on WireClaw
nats req wireclaw-01.tool_exec '{"tool":"device_register","name":"ci_status","type":"nats_value","subject":"ci.build.status"}'

# Step 2: Create a rule that reacts to it
nats req wireclaw-01.tool_exec '{"tool":"rule_create","rule_name":"ci alert","sensor_name":"ci_status","condition":"eq","threshold":0,"on_action":"led_set","on_r":255,"on_g":0,"on_b":0,"off_action":"led_set","off_r":0,"off_g":255,"off_b":0}'

# Step 3: OpenClaw publishes when CI fails/passes
nats pub ci.build.status "0"   # fail → LED goes red
nats pub ci.build.status "1"   # pass → LED goes green
```

Once the sensor and rule are set up, WireClaw handles everything locally. OpenClaw just pushes data — no tool calls needed after initial setup.

### Setup

**Requirements:**

1. A **NATS server** running and reachable from both the OpenClaw host and WireClaw device(s). One-liner: `nats-server -a 0.0.0.0`
2. The **NATS CLI** (`nats` binary) installed on the OpenClaw host
3. WireClaw configured with the NATS server's address (`nats_host` in config.json)

**Install the skill** on the machine running OpenClaw:

```bash
cp -r skill/wireclaw ~/.openclaw/workspace/skills/wireclaw
```

Or just ask your OpenClaw: *"Install the wireclaw skill from github.com/M64GitHub/WireClaw"*

If your NATS server is not on `localhost:4222`, set the environment variable:

```bash
export WIRECLAW_NATS_URL="nats://192.168.1.100:4222"
```

### Verify It Works

After setup, run the discover command from the OpenClaw host:

```
$ ./wc.sh discover
13:40:47 Sending request on "_wc.discover"
13:40:47 Received with rtt 47.973015ms
{"device":"wireclaw-01","version":"0.4.0","free_heap":81664,"tools":["led_set","gpio_write",
"gpio_read","device_info","file_read","file_write","nats_publish","temperature_read",
"device_register","device_list","device_remove","sensor_read","actuator_set","rule_create",
"rule_list","rule_delete","rule_enable","serial_send","chain_create"],"devices":[{"name":
"chip_temp","kind":"internal_temp","value":33.2,"unit":"C"},{"name":"clock_hour","kind":
"clock_hour","value":12.0,"unit":"h"},{"name":"clock_minute","kind":"clock_minute","value":
40.0,"unit":"m"},{"name":"clock_hhmm","kind":"clock_hhmm","value":1240.0,"unit":""}],
"rules":[]}
```

If you see the device respond with its capabilities, the full stack is connected.

### The Skill

The OpenClaw skill lives in `skill/wireclaw/` and contains everything OpenClaw needs:

- **`SKILL.md`** — full reference: protocol, all 19 tools with parameters, constraints (one action per rule, edge-triggered behavior, clock sensors), cross-domain patterns, and worked examples. Derived from WireClaw's actual system prompt so OpenClaw generates correct tool calls on the first try.
- **`scripts/wc.sh`** — convenience wrapper with 4 subcommands:

```bash
wc.sh exec <device> <tool> [json_params]  # Execute a tool
wc.sh caps <device>                        # Query capabilities
wc.sh discover                             # Find all devices
wc.sh sub <device>                         # Subscribe to events
```

OpenClaw reads `SKILL.md`, understands what WireClaw can do, and uses `wc.sh` (or raw `nats req`) to execute commands. The LLM sees the tool definitions and generates correct flat JSON payloads directly.

### Security

Two tools are blocked via `tool_exec`:

| Blocked | Reason |
|---------|--------|
| `remote_chat` | Calls `natsClient.process()` in a polling loop — re-entrant NATS processing from within a callback would corrupt internal state. Use `nats req other-device.tool_exec` instead. |
| `file_write` to `/memory.txt` | Internal AI memory, auto-injected into every conversation. External writes would corrupt the device's learned context. |

No authentication in v1 — same as the existing NATS chat and cmd subjects. Anyone on the NATS network can call tools. If your network is untrusted, restrict access at the NATS server level (credentials, subject permissions).

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
RAM:   59.8% (196KB of 320KB)
Flash: 50.8% (1.3MB of 2.5MB)
```

Static allocations: device registry (768B), rule engine (6.2KB), LLM request buffer (20KB), conversation history, persistent memory (512B), TLS stack, WebServer + mDNS (~4.5KB RAM). Setup portal and web config HTML are stored in flash (PROGMEM), not RAM.

## License

MIT

---

[wireclaw.io](https://wireclaw.io)
