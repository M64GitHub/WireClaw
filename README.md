# WireClaw 🦀⚡

OpenClaw controls files. WireClaw controls wires.
OpenClaw lives on your laptop. WireClaw lives in the wall.
WireClaw turns intent into signals.

**AI agents for the physical world.**

Chat with your ESP32 from Telegram, serial, or NATS - and let it control real hardware through natural language. A $5 microcontroller that understands "turn on the fan when the temperature exceeds 28°C" and just does it.

<!-- TODO: Add Telegram screenshot here -->
<!-- ![Telegram chat controlling ESP32 hardware](docs/telegram-demo.png) -->

> [OpenClaw](https://github.com/openclaw/openclaw) brought AI agents to your desktop. **esp-claw** brings them to the physical world - GPIO pins, sensors, relays, and LEDs on a microcontroller that fits in your palm.

## What Can It Do?

Talk to it like you'd talk to a person. Via Telegram from your phone, serial over USB, or NATS from anywhere on your network:

| You say | What happens |
|---|---|
| *"Turn the LED red"* | `led_set(r=255, g=0, b=0)` - LED turns red |
| *"Set GPIO 4 high"* | `gpio_write(pin=4, value=1)` - relay clicks on |
| *"Read GPIO 5"* | `gpio_read(pin=5)` - returns the pin state |
| *"How much free memory do you have?"* | `device_info()` - reports heap, uptime, chip info |
| *"Save a note to /notes.txt"* | `file_write(...)` - writes to flash |
| *"Publish 'hello' to home.alert"* | `nats_publish(...)` - sends a NATS message |

The AI runs an agentic loop with tool calling - up to 5 tool-call iterations per message - so it can reason, act, observe, and respond.

## Why Not Just Run the Agent on a Computer?

You could. But then you need a computer running 24/7, a bridge to the hardware, and something to manage the connection. esp-claw is self-contained: flash it, configure WiFi and a Telegram bot token, and chat with it from your phone. No server, no bridge, no hub. The ESP32 *is* the agent.

For multi-device setups, add a [NATS](https://nats.io/) server and the devices coordinate directly - machine-to-machine, sub-millisecond, no cloud required.

## Features

- **Telegram Bot** - chat with your ESP32 from anywhere, directly from your phone
- **Tool Calling** - agentic loop where the LLM controls GPIO, LEDs, filesystem, and NATS
- **NATS Integration** - device-to-device communication, commands, and event publishing
- **Serial Interface** - local chat and commands over USB (115200 baud)
- **LLM Chat** via [OpenRouter](https://openrouter.ai/) API (any OpenAI-compatible provider)
- **Filesystem Config** - WiFi, API key, model, system prompt stored on LittleFS (no recompile)
- **Conversation History** - 6-turn circular buffer, persisted to flash across reboots
- **Watchdog Timer** - auto-recovery from hangs (60s timeout)
- **LED Status** - heartbeat pulse, color feedback for thinking/success/error states

## Hardware

- **Board:** ESP32-C6 (tested on WaveShare ESP32-C6 DevKit with 8MB flash)
- **Platform:** [pioarduino](https://github.com/pioarduino/platform-espressif32) via PlatformIO
- **Requirements:** WiFi network, [OpenRouter](https://openrouter.ai/) API key

Total BOM for a basic setup: ~$5.

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
  "device_name": "esp-claw-01",
  "max_tokens": 2048,
  "temperature": 0.7,
  "nats_host": "",
  "nats_port": "4222",
  "telegram_token": "your-bot-token-from-botfather",
  "telegram_chat_id": "your-chat-id"
}
```

For Telegram: create a bot via [@BotFather](https://t.me/BotFather), get your chat ID from [@userinfobot](https://t.me/userinfobot).

Leave `telegram_token` empty to disable Telegram. Leave `nats_host` empty to disable NATS.

Optionally edit `data/system_prompt.txt` to customize the AI's personality.

### 3. Build and Flash

```
pio run -t uploadfs    # upload config + system prompt
pio run -t upload      # flash firmware
pio device monitor     # connect via serial
```

Type a message and press Enter - or just open Telegram and text your bot.

## Telegram Bot

The most accessible way to use esp-claw. No computer needed - just your phone.

- Polls for messages every 10 seconds (3 seconds during active conversation)
- Only responds to messages from your configured `telegram_chat_id`
- Same agentic loop as serial/NATS - all tools available
- TLS connections are sequential, never concurrent (one connection at a time, ~40% RAM used)

### Setup

1. Message [@BotFather](https://t.me/BotFather) on Telegram → `/newbot` → copy the token
2. Get your chat ID from [@userinfobot](https://t.me/userinfobot)
3. Add both to `config.json`, flash, done

## NATS Integration

NATS is the machine-to-machine backbone. While Telegram is for humans chatting with devices, NATS is for automation: device-to-device events, scripted commands, fleet management.

When `nats_host` is configured, the device subscribes using `device_name` as prefix:

| Subject | Pattern | Description |
|---|---|---|
| `{device_name}.chat` | request/reply | Send a message, get LLM response back |
| `{device_name}.cmd` | fire-and-forget | System commands (status, clear, heap, debug, reboot) |
| `{device_name}.events` | publish | Status events and responses |

```bash
# Chat with the AI (request/reply)
nats req esp-claw-01.chat "What is your free memory?"

# Send a system command
nats req esp-claw-01.cmd "status"

# Watch events from the device
nats sub "esp-claw-01.events"
```

### Cross-Channel Example

Subscribe to NATS events in a terminal, then ask the AI from Telegram (or serial) to publish:

```bash
# Terminal: listen for messages
nats sub "home.>"

# From Telegram: "Send a NATS message to home.alert saying the temperature is 23 degrees"
# Terminal shows:
# [#1] Received on "home.alert"
# The temperature is 23 degrees.
```

The AI has access to all tools regardless of which channel the message came from.

## LLM Tools

| Tool | Description |
|---|---|
| `led_set` | Set RGB LED color (0-255 per channel) |
| `gpio_write` | Set a GPIO pin HIGH or LOW |
| `gpio_read` | Read digital state of a GPIO pin |
| `device_info` | Get heap, uptime, WiFi, chip info |
| `file_read` | Read a file from LittleFS |
| `file_write` | Write a file to LittleFS |
| `nats_publish` | Publish a message to a NATS subject |

## Serial Commands

| Command | Description |
|---|---|
| `/status` | Device status (WiFi, heap, NATS, uptime) |
| `/config` | Show loaded configuration |
| `/prompt` | Show system prompt |
| `/history` | Show conversation history (truncated) |
| `/history full` | Show full conversation history |
| `/clear` | Clear conversation history |
| `/heap` | Show free memory |
| `/debug` | Toggle debug output |
| `/reboot` | Restart ESP32 |
| `/help` | List commands |
| *(anything else)* | Chat with AI |

## Configuration

| Field | Description |
|---|---|
| `wifi_ssid` | WiFi network name |
| `wifi_pass` | WiFi password |
| `api_key` | OpenRouter API key |
| `model` | LLM model identifier (e.g. `openai/gpt-4o-mini`) |
| `device_name` | Device name, used as NATS subject prefix |
| `max_tokens` | Max response tokens (reserved for future use) |
| `temperature` | LLM temperature (reserved for future use) |
| `nats_host` | NATS server IP/hostname (empty = disabled) |
| `nats_port` | NATS server port (default: 4222) |
| `telegram_token` | Telegram Bot API token from [@BotFather](https://t.me/BotFather) (empty = disabled) |
| `telegram_chat_id` | Allowed Telegram chat ID (only this chat can talk to the bot) |

## Architecture

```
Telegram / Serial / NATS
         |
         v
    chatWithLLM()
         |
         v
    System prompt + history + user message
         |
         v
    HTTPS → OpenRouter ─────────────────────┐
         |                                   |
         v                                   |
    Parse response                           |
         |                                   |
    ┌────┴────┐                              |
    │         │                              |
  text    tool_calls                         |
    │         │                              |
    v         v                              |
  done    Execute → append results → loop ───┘
             (max 5 iterations)
```

- **Memory:** static buffers, no dynamic allocation in hot paths (~40% RAM used)
- **TLS:** `WiFiClientSecure` - one connection at a time (Telegram and OpenRouter share sequentially)
- **Watchdog:** 60s task WDT, fed every loop iteration

## Project Structure

```
esp-claw/
  platformio.ini              # Build config (ESP32-C6, pioarduino, LittleFS)
  data/
    config.json               # Runtime config (gitignored)
    config.json.example       # Example config template
    system_prompt.txt         # AI personality/instructions
  include/
    llm_client.h              # LLM client API
    tools.h                   # Tool execution API
  src/
    main.cpp                  # WiFi, serial, NATS, Telegram, agentic loop, history
    llm_client.cpp            # HTTPS client for OpenRouter
    tools.cpp                 # Tool definitions and handlers
  lib/
    nats/                     # NATS client (nats-atoms)
```

## Roadmap

- [ ] **Rule engine** - "if temperature > 28, turn on fan" as persistent local rules evaluated in the main loop, no LLM needed after creation
- [ ] **Sensor abstraction** - named sensors and actuators instead of raw GPIO numbers
- [ ] **Display dashboard** - live sensor readings, rule status, conversation on a small SPI screen
- [ ] **Data logging** - circular buffer of sensor readings, queryable via LLM ("what was the temperature overnight?")
- [ ] **Cross-device rules** - NATS subscribe as rule trigger (device A's sensor controls device B's relay)

## Inspired By

- [OpenClaw](https://github.com/openclaw/openclaw) - the AI agent that controls your digital life

esp-claw takes the same idea to the physical world: not files and calendars, but GPIO pins, sensors, and relays.

## License

MIT
