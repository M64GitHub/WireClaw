# esp-claw

An AI agent running on an ESP32 microcontroller. Chat with an LLM over serial, NATS, or Telegram, and let it control hardware through tool calling.

Inspired by [PicoClaw](https://github.com/pico-claw/picoclaw) (a Go-based AI agent framework by Sipeed), reimplemented from scratch in C++ for ESP32.

## Features

- **LLM Chat** via OpenRouter API (any OpenAI-compatible provider)
- **Tool Calling** — agentic loop where the LLM controls GPIO, LEDs, filesystem, and NATS
- **Serial Interface** — chat and commands over USB serial (115200 baud)
- **Telegram Bot** — chat with your ESP32 from anywhere via Telegram
- **NATS Integration** — remote chat via request/reply, commands, event publishing
- **Filesystem Config** — WiFi, API key, model, system prompt stored on LittleFS (no recompile to change)
- **Conversation History** — 6-turn circular buffer, persisted to flash across reboots
- **Watchdog Timer** — auto-recovery from hangs (60s timeout)
- **LED Status** — heartbeat pulse, color feedback for thinking/success/error states

## Hardware

- **Board:** ESP32-C6 (tested on WaveShare ESP32-C6 DevKit with 8MB flash)
- **Platform:** [pioarduino](https://github.com/pioarduino/platform-espressif32) via PlatformIO
- **Requirements:** WiFi network, [OpenRouter](https://openrouter.ai/) API key

## Quick Start

### 1. Install PlatformIO

```
pip install platformio
```

### 2. Configure

Copy the example config and fill in your values:

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
  "nats_host": "192.168.1.100",
  "nats_port": "4222",
  "telegram_token": "your-bot-token-from-botfather",
  "telegram_chat_id": "your-chat-id"
}
```

Leave `nats_host` empty (`""`) to disable NATS. Leave `telegram_token` empty to disable Telegram.

Optionally edit `data/system_prompt.txt` to customize the AI's personality.

### 3. Upload Filesystem

```
pio run -t uploadfs
```

### 4. Build and Flash

```
pio run -t upload
```

### 5. Connect

```
pio device monitor
```

Type a message and press Enter. The AI responds via the LLM.

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

## LLM Tools

The AI can call these tools during conversation:

| Tool | Description |
|---|---|
| `led_set` | Set RGB LED color (0-255 per channel) |
| `gpio_write` | Set a GPIO pin HIGH or LOW |
| `gpio_read` | Read digital state of a GPIO pin |
| `device_info` | Get heap, uptime, WiFi, chip info |
| `file_read` | Read a file from LittleFS |
| `file_write` | Write a file to LittleFS |
| `nats_publish` | Publish a message to a NATS subject |

Example: *"Turn the LED red"* triggers `led_set(r=255, g=0, b=0)`.

The agentic loop runs up to 5 tool-call iterations per message before returning a final text response.

## NATS Integration

When `nats_host` is configured, the device subscribes to topics using `device_name` as prefix:

| Subject | Pattern | Description |
|---|---|---|
| `{device_name}.chat` | request/reply | Send a message, get LLM response back |
| `{device_name}.cmd` | fire-and-forget | System commands (status, clear, heap, debug, reboot) |
| `{device_name}.events` | publish | Status events and responses |

### Examples (using [NATS CLI](https://github.com/nats-io/natscli)):

```bash
# Chat with the AI (request/reply, waits for response)
nats req esp-claw-01.chat "What is your free memory?"

# Send a command
nats req esp-claw-01.cmd "status"

# Watch events
nats sub "esp-claw-01.events"
```

## Telegram Bot

When `telegram_token` and `telegram_chat_id` are configured, the device polls the Telegram Bot API for incoming messages.

### Setup

1. Message [@BotFather](https://t.me/BotFather) on Telegram and create a new bot with `/newbot`
2. Copy the bot token into `config.json` as `telegram_token`
3. Send a message to your bot, then find your chat ID by visiting `https://api.telegram.org/bot<TOKEN>/getUpdates`
4. Copy your `chat.id` value into `config.json` as `telegram_chat_id`

### How it works

- Polls `getUpdates` every 10 seconds (3 seconds after recent activity)
- Only responds to messages from the configured `telegram_chat_id` (security)
- Runs the same agentic loop as serial/NATS — the AI can use all tools
- TLS connections are sequential (never concurrent with LLM calls)

### Example

Send *"Set the LED to purple"* from Telegram, and the bot responds with confirmation while the LED changes color on the board.

## Project Structure

```
esp-claw/
  platformio.ini              # Build configuration (ESP32-C6, pioarduino, LittleFS)
  data/
    config.json               # Runtime config (gitignored)
    config.json.example       # Example config template
    system_prompt.txt         # AI personality/instructions
  include/
    llm_client.h              # LLM client API (chat, tool calls, message types)
    tools.h                   # Tool execution API
  src/
    main.cpp                  # Application: WiFi, serial, NATS, Telegram, agentic loop, history
    llm_client.cpp            # HTTPS client for OpenRouter (JSON build/parse, streaming)
    tools.cpp                 # Tool definitions and handlers (7 tools)
  lib/
    nats/                     # NATS client library (local copy of nats-atoms)
      nats_atoms.h            # Single-include header
      proto/                  # Core NATS protocol (C)
      parse/                  # Safe string parsing utilities
      json/                   # JSON builder/parser
      transport/              # Arduino WiFiClient transport
      cpp/                    # C++ RAII wrapper (NatsClient)
```

## Architecture

```
Serial/NATS/Telegram Input
       |
       v
  chatWithLLM()
       |
       v
  Build messages (system prompt + history + user message)
       |
       v
  LLM API call (HTTPS to OpenRouter) ──────────────────┐
       |                                                 |
       v                                                 |
  Parse response                                         |
       |                                                 |
   ┌───┴───┐                                            |
   │       │                                            |
 text   tool_calls                                      |
   │       │                                            |
   v       v                                            |
 done   Execute tools → append results → loop back ─────┘
           (max 5 iterations)
```

- **TLS:** `WiFiClientSecure` with `setInsecure()` (no cert pinning yet)
- **HTTP:** reads full response until connection close (handles chunked encoding)
- **Memory:** static buffers throughout, no dynamic allocation in hot paths
- **Watchdog:** 60s task WDT, fed every loop iteration

## License

MIT
