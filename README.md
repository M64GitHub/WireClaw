# WireClaw

An AI agent that lives on a $5 microcontroller and controls real hardware.

Tell it what you want in plain language - over Telegram, serial, or NATS - and it wires up GPIO pins, reads sensors, switches relays, and sets up automation rules that keep running without the AI.

```
You:  "When the chip temperature goes above 28, set the LED orange.
       When it drops back down, set it cyan."

WireClaw: rule_create(sensor_name="chip_temp", condition="gt", threshold=28,
                       on_action="led_set", on_r=255, on_g=80, on_b=0,
                       off_action="led_set", off_r=0, off_g=255, off_b=255)

       → Rule created: rule_01 'heat warning' - chip_temp > 28 (every 5s) with auto-off
```

The rule now runs in the main loop. No cloud, no server, no LLM calls. Just the ESP32 checking the sensor every 5 seconds and flipping the LED.

It can also text you:

```
You:  "Send me a Telegram message when chip temperature goes above 40."

WireClaw: rule_create(sensor_name="chip_temp", condition="gt", threshold=40,
                       on_action="telegram",
                       on_telegram_message="Chip is overheating!",
                       off_action="telegram",
                       off_telegram_message="Chip temperature back to normal.")

       → Rule created: rule_01 'heat alert' - chip_temp > 40 (every 5s) with auto-off
```

The ESP32 monitors the sensor and sends you a Telegram message the moment the threshold is crossed - and another when it clears. No LLM in the loop.

## How It Works

WireClaw runs two loops on the ESP32:

**The AI loop** handles conversation. When you send a message - via Telegram, serial, or NATS - it calls an LLM through OpenRouter, which responds with tool calls. The AI can read sensors, flip GPIOs, set LEDs, and create automation rules. Up to 5 tool-call iterations per message. This is the setup phase.

**The rule loop** runs every iteration of `loop()`, continuously, with no network and no LLM. Each cycle it walks through all enabled rules, reads their sensors, evaluates their conditions, and fires their actions - GPIO writes, LED changes, NATS publishes, Telegram alerts - directly from the microcontroller. Rules persist to flash and survive reboots. This is what runs 24/7.

The AI creates the rules. The rules run without the AI.

```
  ┌─────────────────────────────────────────────────────┐
  │                    loop()                           │
  │                                                     │
  │  ┌───────────────────────────────────────────────┐  │
  │  │  rulesEvaluate()          ← every iteration   │  │
  │  │                                               │  │
  │  │  for each rule:                               │  │
  │  │    read sensor ──→ check condition ──→ act    │  │
  │  │                        │                      │  │
  │  │              GPIO / LED / NATS / Telegram     │  │
  │  └───────────────────────────────────────────────┘  │
  │                                                     │
  │  Telegram poll ──┐                                  │
  │  Serial input ───┼──→ chatWithLLM() ──→ OpenRouter  │
  │  NATS message ───┘         │                        │
  │                       tool_calls                    │
  │                            │                        │
  │              rule_create, led_set, gpio_write, ...  │
  └─────────────────────────────────────────────────────┘
```

The rule loop and the AI loop share the same `loop()` function but serve different purposes. The rule engine evaluates every cycle regardless of whether anyone is chatting. Multiple rules monitoring the same sensor see the exact same reading per cycle (cached internally), so they always trigger and clear together.

## Try It With Just a Dev Board

You don't need external sensors to test WireClaw. The ESP32's internal temperature sensor is pre-registered as `chip_temp`, and the onboard RGB LED is available as a rule action. Here's a full example using only the bare dev board:

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
    on_r = 255, on_g = 80, on_b = 0,     ← orange when hot
    off_action    = "led_set",
    off_r = 0, off_g = 255, off_b = 255   ← cyan when cool
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
    on_telegram_message  = "Chip temperature exceeded 40 C!",
    off_action           = "telegram",
    off_telegram_message = "Chip temperature back to normal."
)
```

The ESP32 checks every 5 seconds. The moment it crosses 40, your phone buzzes. When it drops back down, you get the all-clear. No LLM calls, no cloud services - just a direct HTTPS request from the ESP32 to the Telegram API.

You can combine actions too: "Set the LED red AND send me a Telegram message when temperature exceeds 50" creates two rules - one for the LED, one for the alert.

### Example: Register an External Sensor + Actuator

If you wire up an NTC thermistor on pin 4 and a relay on pin 16:

```
You:  "Register an NTC thermistor on pin 4 called 'temperature', unit is C"
AI:   device_register(name="temperature", type="ntc_10k", pin=4, unit="C")
      → Registered sensor 'temperature' on pin 4

You:  "Register a relay on pin 16 called 'fan', it uses inverted logic"
AI:   device_register(name="fan", type="relay", pin=16, inverted=true)
      → Registered actuator 'fan' on pin 16

You:  "Turn on the fan when temperature exceeds 28"
AI:   rule_create(rule_name="cool down", sensor_name="temperature",
                  condition="gt", threshold=28, actuator_name="fan")
      → Rule created: rule_01 'cool down' - temperature > 28 (every 5s) with auto-off
```

Devices and rules persist to flash. After a reboot:

```
> /devices
--- devices ---
  chip_temp [internal_temp] pin=255  = 27.3 C
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

- **Rule Engine** - persistent local automation, edge-triggered, evaluated every loop iteration
- **Telegram Alerts** - rules can send you push notifications directly, no LLM in the loop
- **Device Registry** - named sensors and actuators instead of raw pin numbers, persisted to flash
- **AI Agent** - agentic loop with 17 tools, up to 5 iterations per message
- **Telegram Bot** - chat with your ESP32 from your phone
- **NATS Integration** - device-to-device messaging, commands, and rule-triggered events
- **Serial Interface** - local chat and commands over USB (115200 baud)
- **Conversation History** - 6-turn circular buffer, persisted across reboots

## Hardware

- **Board:** ESP32-C6 (tested on WaveShare ESP32-C6 DevKit, 8MB flash)
- **Platform:** [pioarduino](https://github.com/pioarduino/platform-espressif32) via PlatformIO
- **Requirements:** WiFi network, [OpenRouter](https://openrouter.ai/) API key

The dev board alone is enough to get started - chip temperature sensor and RGB LED work out of the box. Add external sensors and actuators as needed.

## Quick Start

### 1. Install PlatformIO

```
pip install platformio
```

### 2. Configure

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
  "nats_host": "",
  "nats_port": "4222",
  "telegram_token": "",
  "telegram_chat_id": "",
  "telegram_cooldown": "60"
}
```

Leave `telegram_token` empty to disable Telegram. Leave `nats_host` empty to disable NATS.

For Telegram: create a bot via [@BotFather](https://t.me/BotFather), get your chat ID from [@userinfobot](https://t.me/userinfobot).

### 3. Build and Flash

```
pio run -t uploadfs    # upload config + system prompt to filesystem
pio run -t upload      # flash firmware
pio device monitor     # connect via serial (115200 baud)
```

Type a message and press Enter. Or open Telegram and text your bot.

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
| `digital_out` | Actuator | `digitalWrite()` - HIGH or LOW |
| `relay` | Actuator | `digitalWrite()` with optional inverted logic |
| `pwm` | Actuator | `analogWrite()` - 0-255 |

`chip_temp` is auto-registered on first boot. All other devices are registered through conversation with the AI.

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
| `always` | Always fire (useful with intervals) |

### Sensor Sources

Rules need a sensor to monitor. Two options:

- **Named sensor** (`sensor_name`) - a device from the registry (e.g. `chip_temp`, or any registered sensor). Preferred.
- **Raw GPIO pin** (`sensor_pin`) - reads a GPIO directly. Set `sensor_analog=true` for `analogRead()` (0–4095), otherwise `digitalRead()` (0/1).

Multiple rules monitoring the same named sensor see the exact same reading per evaluation cycle (cached internally).

### Actions

Each rule has an **on action** (fires when condition becomes true) and an optional **off action** (fires when condition clears). All parameters below have `on_` and `off_` variants.

| Action | Parameters | Description |
|--------|-----------|-------------|
| `actuator` | `actuator_name` | Set a registered actuator on/off by device name. Simplest option - just provide the actuator name and the rule handles on=1/off=0 automatically. |
| `led_set` | `on_r`, `on_g`, `on_b` (0–255) | Set the onboard RGB LED color. |
| `gpio_write` | `on_pin`, `on_value` (0 or 1) | Write a raw GPIO pin HIGH/LOW. |
| `nats_publish` | `on_nats_subject`, `on_nats_payload` | Publish a message to a NATS subject. |
| `telegram` | `on_telegram_message` | Send a Telegram message. Subject to `telegram_cooldown` (default 60s per rule). |

### Examples

You just describe what you want in natural language. The AI picks the right parameters:

```
"Set GPIO 4 high when chip_temp exceeds 30, low when it drops back."
→ on_action=gpio_write, on_pin=4, on_value=1
  off_action=gpio_write, off_pin=4, off_value=0

"Turn on the fan when temperature exceeds 28."
→ actuator_name=fan (auto on/off)

"Set LED red above 35, green below."
→ on_action=led_set, on_r=255, on_g=0, on_b=0
  off_action=led_set, off_r=0, off_g=255, off_b=0

"Alert me on Telegram when chip_temp goes above 40."
→ on_action=telegram, on_telegram_message="Chip over 40!"
  off_action=telegram, off_telegram_message="Back to normal."
```

### Behavior

- **Edge-triggered** - fires once on threshold crossing, not repeatedly
- **Auto-off** - when using `actuator_name` or `off_action`, the reverse action runs when the condition clears
- **Interval** - configurable per rule (default 5 seconds)
- **Telegram cooldown** - per-rule cooldown prevents message spam when sensor oscillates around threshold (configurable via `telegram_cooldown` in config.json, default 60s, 0 = disabled)
- **Sensor caching** - all rules monitoring the same sensor see the same value per evaluation cycle
- **NATS events** - every rule trigger publishes to `{device_name}.events`
- **Persistence** - rules survive reboots (`/rules.json`)
- **IDs** - auto-assigned: `rule_01`, `rule_02`, etc.

## LLM Tools

17 tools available to the AI:

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
| `rule_delete` | Delete a rule by ID or "all" |
| `rule_enable` | Enable/disable a rule without deleting |
| **System** | |
| `device_info` | Heap, uptime, WiFi, chip info |
| `file_read` | Read a file from LittleFS |
| `file_write` | Write a file to LittleFS |
| `nats_publish` | Publish to a NATS subject |

## Serial Commands

| Command | Description |
|---------|-------------|
| `/status` | Device status (WiFi, heap, NATS, uptime) |
| `/devices` | List registered devices with readings |
| `/rules` | List automation rules with status |
| `/config` | Show loaded configuration |
| `/prompt` | Show system prompt |
| `/history` | Show conversation history |
| `/clear` | Clear conversation history |
| `/heap` | Show free memory |
| `/debug` | Toggle debug output |
| `/reboot` | Restart ESP32 |
| `/help` | List commands |

## NATS Integration

When `nats_host` is configured, the device subscribes to:

| Subject | Description |
|---------|-------------|
| `{device_name}.chat` | Request/reply - send a message, get LLM response |
| `{device_name}.cmd` | Commands: status, clear, heap, debug, devices, rules, reboot |
| `{device_name}.events` | Published events: online, rule triggers, chat responses |

Rule triggers automatically publish events:

```json
{"event":"rule","rule":"cool down","state":"on","reading":29,"threshold":28}
```

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
| `api_key` | [OpenRouter](https://openrouter.ai/) API key |
| `model` | LLM model (e.g. `openai/gpt-4o-mini`) |
| `device_name` | Device name, used as NATS subject prefix |
| `nats_host` | NATS server hostname (empty = disabled) |
| `nats_port` | NATS server port (default: 4222) |
| `telegram_token` | Telegram bot token from [@BotFather](https://t.me/BotFather) (empty = disabled) |
| `telegram_chat_id` | Allowed Telegram chat ID |
| `telegram_cooldown` | Minimum seconds between Telegram messages per rule (default: 60, 0 = disabled) |

Edit `data/system_prompt.txt` to customize the AI's personality and instructions.

## Project Structure

```
WireClaw/
  platformio.ini              # Build config (ESP32-C6, pioarduino)
  data/
    config.json               # Runtime config (gitignored)
    config.json.example       # Config template
    system_prompt.txt         # AI personality/instructions
  include/
    llm_client.h              # LLM client (OpenRouter HTTPS)
    tools.h                   # Tool execution API
    devices.h                 # Device registry API
    rules.h                   # Rule engine API
  src/
    main.cpp                  # WiFi, serial, NATS, Telegram, agentic loop
    llm_client.cpp            # HTTPS client for OpenRouter
    tools.cpp                 # 17 tool definitions and handlers
    devices.cpp               # Device registry + persistence
    rules.cpp                 # Rule engine + persistence
  lib/
    nats/                     # NATS client library (nats-atoms)
```

Runtime data (created automatically, stored on flash):
- `/devices.json` - registered devices
- `/rules.json` - automation rules
- `/history.json` - conversation history

## Resource Usage

```
RAM:   48.9% (160KB of 320KB)
Flash: 36.2% (1.2MB of 3.3MB)
```

Static allocations: device registry (768B), rule engine (6.2KB), LLM request buffer (12KB), conversation history, TLS stack.

## Roadmap

- [x] Rule engine - persistent local automation without LLM
- [x] Device registry - named sensors and actuators
- [x] Telegram alerts - rules send push notifications directly from the ESP32
- [ ] Display dashboard - live sensor readings and rule status on SPI screen
- [ ] Data logging - circular buffer of readings, queryable via LLM
- [ ] Cross-device rules - NATS subscribe as rule trigger

## License

MIT
