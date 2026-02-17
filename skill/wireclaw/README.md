# WireClaw - OpenClaw Skill

Control physical hardware from OpenClaw. WireClaw runs on ESP32 microcontrollers
and executes tool calls directly on LEDs, GPIO pins, relays, and sensors via NATS.

## Requirements

- [NATS CLI](https://github.com/nats-io/natscli) (`nats` binary in PATH)
- NATS server accessible from both OpenClaw and WireClaw (default port 4222)
- One or more WireClaw devices on the same network

## Installation

```bash
openclaw install wireclaw
```

Or manually copy the `skill/wireclaw/` folder to `~/.openclaw/workspace/skills/wireclaw/`.

## Configuration

Set `WIRECLAW_NATS_URL` if your NATS server is not at `localhost:4222`:

```bash
export WIRECLAW_NATS_URL="nats://192.168.1.100:4222"
```

## Quick Start

```bash
# Discover devices on the network
scripts/wc.sh discover

# Set LED green on a device
scripts/wc.sh exec wireclaw-01 led_set '{"r":0,"g":255,"b":0}'

# Read chip temperature
scripts/wc.sh exec wireclaw-01 sensor_read '{"name":"chip_temp"}'

# Query device capabilities
scripts/wc.sh caps wireclaw-01

# Create a persistent automation rule
scripts/wc.sh exec wireclaw-01 rule_create '{"rule_name":"heat alert","sensor_name":"chip_temp","condition":"gt","threshold":35,"on_action":"led_set","on_r":255,"on_g":0,"on_b":0,"off_action":"led_set","off_r":0,"off_g":255,"off_b":0}'
```

See `SKILL.md` for the full tool reference and automation patterns.

## Links

- [WireClaw](https://wireclaw.io) - Project homepage
- [WireClaw GitHub](https://github.com/M64GitHub/WireClaw) - Source code
