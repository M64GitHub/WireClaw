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
- **NATS HAL** - direct hardware access over NATS (`{device}.hal.gpio.5.set "1"` → `ok`) - no LLM, no JSON, just raw request/reply for GPIO, ADC, PWM, UART, system info, and registered devices
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

## Documentation

| Document | Description |
|----------|-------------|
| [Getting Started Examples](docs/EXAMPLES.md) | Walkthrough examples using just a bare dev board |
| [Configuration Reference](docs/CONFIGURATION.md) | Setup portal, web config, config fields, local LLM, persistent memory |
| [Device Registry](docs/DEVICE-REGISTRY.md) | All sensor and actuator types |
| [Rule Engine Reference](docs/RULE-ENGINE.md) | Conditions, actions, and rule behavior |
| [Rule Chaining](docs/RULE-CHAINING.md) | Multi-step sequences with delays |
| [NATS Integration](docs/NATS.md) | Virtual sensors, serial bridge, multi-device, subjects |
| [OpenClaw Integration](docs/OPENCLAW.md) | Cross-domain automation with OpenClaw over NATS |
| [NATS HAL](docs/HAL.md) | Direct hardware access over NATS, no LLM |
| [Tools & Commands](docs/TOOLS.md) | All 20 LLM tools and serial commands |

## Resource Usage

```
RAM:   59.7% (196KB of 320KB)
Flash: 51.4% (1.3MB of 2.5MB)
```

Static allocations: device registry (768B), rule engine (6.2KB), LLM request buffer (20KB), conversation history, persistent memory (512B), TLS stack, WebServer + mDNS (~4.5KB RAM). Setup portal and web config HTML are stored in flash (PROGMEM), not RAM.

## License

MIT

---

[wireclaw.io](https://wireclaw.io)
