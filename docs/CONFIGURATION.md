# WireClaw – Configuration Reference

---

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

## Configuration Fields

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
