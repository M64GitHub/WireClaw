# WireClaw – Tools & Serial Commands Reference

---

## LLM Tools

20 tools available to the AI:

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
| `chain_create` | Create a multi-step rule chain (up to 5 steps with delays) |
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
