# WireClaw – Getting Started Examples

Most examples run on a bare ESP32 dev board – no external sensors needed.
The final example shows how to add external hardware.

---

## Try It With Just a Dev Board

All you need is an ESP32-C6 dev board and a USB cable. [Flash it from your browser](https://wireclaw.io/flash.html), connect to the setup AP from your phone, enter your WiFi and API key, and you're up and running.

The ESP32's internal temperature sensor is pre-registered as `chip_temp`, clock sensors provide the current hour and minute, and the onboard RGB LED is auto-registered as `rgb_led` (also controllable via NATS HAL). No external sensors needed - here's some examples using only the bare dev board:

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

One tool call creates 4 linked rules. Delays are non-blocking. The chain fires again the next time the sensor crosses the threshold. See [docs/RULE-CHAINING.md](RULE-CHAINING.md) for the full reference.

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
              NTC on GND side              NTC on 3.3V side
              (default)                    (inverted=true)

              3.3V ──┐                     3.3V ──┐
                     │                            │
                [10K fixed]                  [NTC 10K]
                     │                            │
              ADC ───┤                     ADC ───┤
                     │                            │
                 [NTC 10K]                   [10K fixed]
                     │                            │
              GND ──┘                      GND ──┘
```

```
You:  "Register an NTC thermistor on pin 4 called 'temperature', unit is C"
AI:   device_register(name="temperature", type="ntc_10k", pin=4, unit="C")
      -> Registered sensor 'temperature' on pin 4

      (If the NTC is on the 3.3V side of the voltage divider, add inverted=true)
AI:   device_register(name="temperature", type="ntc_10k", pin=4, unit="C", inverted=true)
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
