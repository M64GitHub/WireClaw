# WireClaw – OpenClaw Integration

---

## OpenClaw Integration

[OpenClaw](https://github.com/openclaw) is an open-source AI agent that controls your digital life - email, calendar, GitHub, shell. WireClaw controls the physical world - LEDs, relays, GPIO pins, sensors. This integration connects them over NATS: OpenClaw executes tools directly on WireClaw's hardware without involving WireClaw's own LLM. One LLM call, not two. Fast, cheap, no telephone game between models.

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

The key is `"tool"` (not `"name"`) to avoid collision with tools that have a `"name"` parameter like `device_register`. This works because WireClaw's internal tool handlers use `strstr`-based parsing - they find their keys anywhere in the JSON string, ignore the rest.

### Discovery

Find all WireClaw devices on the network:

```bash
$ nats req _ion.discover "" --replies=0 --timeout=3s
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

All 19 tools are available via `tool_exec` (except `remote_chat` - see [Security](#security)):

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

Rules created via `tool_exec` persist on the ESP32 and run 24/7 - even when OpenClaw, the NATS server, or WiFi is offline.

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

**External data → WireClaw sensor.** The most powerful pattern - OpenClaw pushes data to a NATS subject, WireClaw has a `nats_value` sensor on it, and persistent rules handle the rest autonomously:

```bash
# Step 1: Register a NATS virtual sensor on WireClaw
nats req wireclaw-01.tool_exec '{"tool":"device_register","name":"ci_status","type":"nats_value","subject":"ci.build.status"}'

# Step 2: Create a rule that reacts to it
nats req wireclaw-01.tool_exec '{"tool":"rule_create","rule_name":"ci alert","sensor_name":"ci_status","condition":"eq","threshold":0,"on_action":"led_set","on_r":255,"on_g":0,"on_b":0,"off_action":"led_set","off_r":0,"off_g":255,"off_b":0}'

# Step 3: OpenClaw publishes when CI fails/passes
nats pub ci.build.status "0"   # fail → LED goes red
nats pub ci.build.status "1"   # pass → LED goes green
```

Once the sensor and rule are set up, WireClaw handles everything locally. OpenClaw just pushes data - no tool calls needed after initial setup.

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
13:40:47 Sending request on "_ion.discover"
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

- **`SKILL.md`** - full reference: protocol, all 19 tools with parameters, constraints (one action per rule, edge-triggered behavior, clock sensors), cross-domain patterns, and worked examples. Derived from WireClaw's actual system prompt so OpenClaw generates correct tool calls on the first try.
- **`scripts/wc.sh`** - convenience wrapper with 4 subcommands:

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
| `remote_chat` | Calls `natsClient.process()` in a polling loop - re-entrant NATS processing from within a callback would corrupt internal state. Use `nats req other-device.tool_exec` instead. |
| `file_write` to `/memory.txt` | Internal AI memory, auto-injected into every conversation. External writes would corrupt the device's learned context. |

No authentication in v1 - same as the existing NATS chat and cmd subjects. Anyone on the NATS network can call tools. If your network is untrusted, restrict access at the NATS server level (credentials, subject permissions).
