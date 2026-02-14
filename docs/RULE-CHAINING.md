# Rule Chaining

Multi-step automation sequences in a single tool call. When a sensor triggers, execute a series of actions with delays between them - no LLM calls, no network, no blocking.

## Concept

A chain is a sequence of actions that fire in order when a sensor condition triggers:

```
sensor triggers -> step1 fires -> delay -> step2 fires -> delay -> step3 fires
```

The `chain_create` tool takes the full sequence in one call. The firmware creates the rules internally and links them together.

## The `chain_create` Tool

### Trigger Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `sensor_name` | string | yes | Sensor to monitor |
| `condition` | string | yes | `gt`, `lt`, `eq`, `neq`, `change`, `always` |
| `threshold` | integer | yes | Threshold value for condition |
| `interval_seconds` | integer | no | Check interval (default 5s, minimum 5s) |

### Step Parameters

Each step is prefixed with `step1_` through `step5_`. Steps 1 and 2 are required. Steps 3-5 are optional.

| Suffix | Type | Description |
|--------|------|-------------|
| `_action` | string | `telegram`, `led_set`, `gpio_write`, `nats_publish`, `actuator`, `serial_send` |
| `_delay` | integer | Seconds to wait before this step fires (step2/step3 only) |
| `_message` | string | Message text for telegram, nats_publish, or serial_send. Use `{value}` for sensor reading. |
| `_r`, `_g`, `_b` | integer | RGB values 0-255 for `led_set` |
| `_pin` | integer | GPIO pin for `gpio_write` |
| `_value` | integer | Value for `gpio_write` or `actuator` |
| `_actuator` | string | Actuator name for `actuator` action |
| `_nats_subject` | string | NATS subject for `nats_publish` |

## Example

User via Telegram:

> "please send me a telegram message when the test sensor > 100, then wait 5s, then send me another message "hello test", then set the led to green, and after another 10s set the led off"

The AI makes a single `chain_create` tool call. The firmware creates 4 linked rules. Both examples are tested with the same prompt — see [Appendix A](#appendix-a-model-comparison) for full serial output from each model.

## How It Works Internally

### Rule Creation (End-First)

`chain_create` creates rules in reverse order internally:

1. **Step 3** (last) → created first as `COND_CHAINED`, no chain link
2. **Step 2** (middle) → created as `COND_CHAINED`, chain links to step 3 with step 3's delay
3. **Step 1** (source) → created with sensor/condition, chain links to step 2 with step 2's delay

This ensures each target exists before it's referenced.

### The `COND_CHAINED` Condition

Rules with `condition="chained"`:
- Are **skipped** during normal sensor evaluation in `rulesEvaluate()`
- Only fire when another rule's chain triggers them
- Do **not** need a `sensor_name` or `sensor_pin`
- Get their `fired` flag reset when enqueued, so they can fire repeatedly

### Pending Chain Queue

When a rule fires and has a chain target, it's added to a static queue of 8 slots (`PendingChain`). Each entry stores the target rule ID and a `fire_at` timestamp (`millis() + delay_ms`).

At the end of every `rulesEvaluate()` call, the queue is processed:
1. For each entry where `millis() >= fire_at`: find the target rule, execute its action, publish its NATS event
2. If the target itself has a chain link, enqueue that too (continuing the chain)
3. Clear the processed entry

Delays are **non-blocking** — the main loop continues running while waiting.

### Deduplication

The same rule ID cannot appear twice in the pending queue. If a chain fires again before the previous delay expires, the second enqueue is silently skipped. This prevents spam when a sensor oscillates around a threshold.

### Depth Limit

A depth counter tracks chain triggers per `rulesEvaluate()` call. If it reaches `MAX_CHAIN_DEPTH` (8), remaining entries are cleared:

```
[Chain] Max depth 8 reached, breaking chain
```

This prevents infinite loops from circular chains.

## Safety Guards

### Self-Reference Rejection

A rule cannot chain to itself. If `chain_rule` equals the newly created rule's own ID, the chain link is silently cleared:

```
[Rule] Warning: chain_id 'rule_01' is self-reference, ignored
```

### Orphan Delay Warning

If `chain_delay_seconds` is set without a `chain_rule`, the tool result warns:

```
(Warning: chain_delay ignored, no chain_rule)
```

### Dangling References

If a chain target rule is deleted, the chain silently becomes a no-op. The source rule continues to function normally — only the chain link is broken.

## Advanced: Manual Chaining with `rule_create`

For use cases not covered by `chain_create` (OFF-chains, custom linking), `rule_create` still supports manual chain parameters:

| Parameter | Type | Description |
|-----------|------|-------------|
| `chain_rule` | string | Rule ID to trigger after ON action (e.g. `"rule_01"`) |
| `chain_delay_seconds` | integer | Delay before ON-chain fires (0 = immediate) |
| `chain_off_rule` | string | Rule ID to trigger after OFF action |
| `chain_off_delay_seconds` | integer | Delay before OFF-chain fires (0 = immediate) |

**ON-chain** fires after the rule's ON action triggers. **OFF-chain** fires after the rule's OFF action triggers (condition clears).

Example — motion sensor with delayed off:

```
rule_create(rule_name="Lights off",
            on_action="actuator", actuator_name="lights")
-> rule_01 (auto-defaults to chained)

rule_create(rule_name="Motion light", sensor_name="motion",
            condition="eq", threshold=1,
            actuator_name="lights",
            off_action="none",
            chain_off_rule="rule_01", chain_off_delay_seconds=30)
-> rule_02
```

Motion detected: lights ON. Motion clears: 30s delay, then lights OFF via OFF-chain.

When using manual chaining, create chain targets **end-first** (last step first, source last) so each target's rule ID exists before it's referenced.

## `/rules` Display

```
> /rules
rule_01 'test step4' [ON]  chained 0 val=0 idle
  on: led_set(0,0,0)
rule_02 'test step3' [ON]  chained 0 val=0 idle
  on: led_set(0,255,0)
  chain: ->rule_01 (10s)
rule_03 'test step2' [ON]  chained 0 val=0 idle
  on: telegram "hello test"
  chain: ->rule_02 (0s)
rule_04 'test step1' [ON] test gt 100 val=1000 FIRED
  on: telegram "Test sensor exceeded 100: {test}"
  chain: ->rule_03 (5s)
```

## JSON Persistence

Chain fields are persisted to `/rules.json` using short keys:

```json
{
  "id": "rule_08",
  "nm": "test step1",
  "co": "gt",
  "ci": "rule_07",
  "cd": 5000,
  "coi": "",
  "cod": 0
}
```

Rules with no chain have empty `"ci"` / `"coi"` strings and 0 delays.

## Limitations

- **Max 5 steps** per `chain_create` call. For longer sequences, use manual `rule_create` with `chain_rule`.
- **Max 8 pending chain entries** at any time (`MAX_PENDING_CHAINS`). If the queue is full, new chain triggers are dropped with a serial warning.
- **Max chain depth 8** per evaluation cycle (`MAX_CHAIN_DEPTH`). Deeper chains or circular chains are broken.
- **One chain target per direction** — each rule can chain to one rule on ON and one on OFF. For fan-out (one rule triggering multiple), create intermediate chained rules.
- **No chain cancellation** — once a chain is queued, it fires when the delay expires even if the source condition clears.
- **Delays are approximate** — the chain fires on the first `rulesEvaluate()` call after the delay expires. With a 5s rule interval, actual delay could be up to 5s longer than specified.
- **Same-target deduplication** — if a rule is already pending in the queue, a second enqueue for the same target is skipped. This prevents spam but means rapid re-triggering may miss a chain.

## RAM Cost

| Component | Bytes |
|-----------|-------|
| `chain_id[12]` x 16 rules | 192 |
| `chain_delay_ms` x 16 rules | 64 |
| `chain_off_id[12]` x 16 rules | 192 |
| `chain_off_delay_ms` x 16 rules | 64 |
| PendingChain queue (8 entries) | 160 |
| **Total** | **~672 bytes** |

0.2% of the ESP32's 320KB RAM.

---

## Appendix A: Model Comparison

Same prompt tested with five models. All produce a working 4-step chain, but with different tool call styles and response times.

### Claude Sonnet 4.5

Sonnet sends only the required parameters — clean and minimal (~400 bytes of arguments).

**Serial output:**

```
[TG] Message from user: please send me a telegram message when the test sensor > 100,
  then wait 5s, then send me another message "hello test", then set the led to green,
  and after another 10s set the led off

--- Thinking... ---
[Agent] 1 tool call(s) in iteration 1:
  -> chain_create({"sensor_name": "test", "condition": "gt", "threshold": 100,
       "step1_action": "telegram", "step1_message": "Test sensor exceeded 100! Value: {value}",
       "step2_action": "telegram", "step2_delay": 5, "step2_message": "hello test",
       "step3_action": "led_set", "step3_delay": 0, "step3_r": 0, "step3_g": 255, "step3_b": 0,
       "step4_action": "led_set", "step4_delay": 10, "step4_r": 0, "step4_g": 0, "step4_b": 0})
     = Chain created: rule_04 test>100 -> telegram -> 5s -> telegram -> LED(0,255,0) -> 10s -> LED(0,0,0)

Perfect! I've created a 4-step automation chain:

1. **When test sensor > 100**: Send you a Telegram alert with the value
2. **After 5 seconds**: Send "hello test" message
3. **Immediately after**: Set LED to green
4. **After 10 more seconds**: Turn LED off

The chain is now active and waiting for the test sensor to exceed 100!
--- (9809ms, 9782+498 tokens) ---
```

**`/rules` output:**

```
rule_01 'test step4' [ON]  chained 0 val=0 idle
  on: led_set(0,0,0)
rule_02 'test step3' [ON]  chained 0 val=0 idle
  on: led_set(0,255,0)
  chain: ->rule_01 (10s)
rule_03 'test step2' [ON]  chained 0 val=0 idle
  on: telegram "hello test"
  chain: ->rule_02 (0s)
rule_04 'test step1' [ON] test gt 100 val=0 idle
  on: telegram "Test sensor exceeded 100! Value: {value}"
  chain: ->rule_03 (5s)
```

---

### Aurora Alpha

Aurora is the fastest — clean parameters like Sonnet, but completes in ~4 seconds (~400 bytes of arguments).

**Serial output:**

```
[TG] Message from user: please send me a telegram message when the test sensor > 100,
  then wait 5s, then send me another message "hello test", then set the led to green,
  and after another 10s set the led off

--- Thinking... ---
[Agent] 1 tool call(s) in iteration 1:
  -> chain_create({"sensor_name": "test", "condition": "gt", "threshold": 100,
       "step1_action": "telegram", "step1_message": "Test sensor: {value}",
       "step2_action": "telegram", "step2_delay": 5, "step2_message": "hello test",
       "step3_action": "led_set", "step3_delay": 0, "step3_r": 0, "step3_g": 255, "step3_b": 0,
       "step4_action": "led_set", "step4_delay": 10, "step4_r": 0, "step4_g": 0, "step4_b": 0})
     = Chain created: rule_04 test>100 -> telegram -> 5s -> telegram -> LED(0,255,0) -> 10s -> LED(0,0,0)

The automation chain has been set up:

1. When **test** > 100 → send a Telegram with the sensor value.
2. After 5 seconds → send a Telegram "hello test".
3. Immediately set the onboard LED to green (0 255 0).
4. After 10 seconds → turn the LED off (0 0 0).

Let me know if you'd like to add or modify anything!
--- (4384ms, 5189+249 tokens) ---
```

**`/rules` output:**

```
rule_01 'test step4' [ON]  chained 0 val=0 idle
  on: led_set(0,0,0)
rule_02 'test step3' [ON]  chained 0 val=0 idle
  on: led_set(0,255,0)
  chain: ->rule_01 (10s)
rule_03 'test step2' [ON]  chained 0 val=0 idle
  on: telegram "hello test"
  chain: ->rule_02 (0s)
rule_04 'test step1' [ON] test gt 100 val=0 idle
  on: telegram "Test sensor: {value}"
  chain: ->rule_03 (5s)
```

---

### GPT-OSS-120B

Clean parameter style like Sonnet and Aurora, completes in ~8 seconds (~400 bytes of arguments).

**Serial output:**

```
[TG] Message from user: please send me a telegram message when the test sensor > 100,
  then wait 5s, then send me another message "hello test", then set the led to green,
  and after another 10s set the led off

--- Thinking... ---
[Agent] 1 tool call(s) in iteration 1:
  -> chain_create({"condition": "gt", "sensor_name": "test", "threshold": 100,
       "step1_action": "telegram", "step1_message": "Test sensor value: {value}",
       "step2_action": "telegram", "step2_message": "hello test", "step2_delay": 5,
       "step3_action": "led_set", "step3_r": 0, "step3_g": 255, "step3_b": 0, "step3_delay": 0,
       "step4_action": "led_set", "step4_r": 0, "step4_g": 0, "step4_b": 0, "step4_delay": 10})
     = Chain created: rule_04 test>100 -> telegram -> 5s -> telegram -> LED(0,255,0) -> 10s -> LED(0,0,0)

The automation chain is set:

1. When **test** > 100, you'll get a Telegram with the sensor value.
2. After 5 seconds, another Telegram saying "hello test".
3. The onboard LED turns **green**.
4. After another 10 seconds, the LED turns **off**.
--- (8378ms, 5199+531 tokens) ---
```

**`/rules` output:**

```
rule_01 'test step4' [ON]  chained 0 val=0 idle
  on: led_set(0,0,0)
rule_02 'test step3' [ON]  chained 0 val=0 idle
  on: led_set(0,255,0)
  chain: ->rule_01 (10s)
rule_03 'test step2' [ON]  chained 0 val=0 idle
  on: telegram "hello test"
  chain: ->rule_02 (0s)
rule_04 'test step1' [ON] test gt 100 val=0 idle
  on: telegram "Test sensor value: {value}"
  chain: ->rule_03 (5s)
```

---

### Gemini 2.5 Flash

Gemini sends only required parameters like Sonnet and Aurora, but in randomized key order. Tied for fastest at ~4 seconds (~400 bytes of arguments). Uses `{test}` (sensor name) as template placeholder.

**Serial output:**

```
[TG] Message from user: please send me a telegram message when the test sensor > 100,
  then wait 5s, then send me another message "hello test", then set the led to green,
  and after another 10s set the led off

--- Thinking... ---
[Agent] 1 tool call(s) in iteration 1:
  -> chain_create({"step3_g": 255, "sensor_name": "test", "step3_action": "led_set",
       "threshold": 100, "step3_delay": 0, "step1_message": "Test sensor value: {test}",
       "step4_g": 0, "step1_action": "telegram", "step2_message": "hello test",
       "condition": "gt", "step3_r": 0, "step4_r": 0, "step4_action": "led_set",
       "step2_action": "telegram", "step4_b": 0, "step4_delay": 10, "step3_b": 0,
       "step2_delay": 5})
     = Chain created: rule_04 test>100 -> telegram -> 5s -> telegram -> LED(0,255,0) -> 10s -> LED(0,0,0)

I've set up a chain of actions for you:

1.  When the 'test' sensor value is greater than 100, a Telegram message will be sent with the sensor's value.
2.  5 seconds later, another Telegram message saying "hello test" will be sent.
3.  Immediately after the second message, the onboard LED will turn green.
4.  After another 10 seconds, the LED will turn off.
--- (4293ms, 4756+195 tokens) ---
```

**`/rules` output:**

```
rule_01 'test step4' [ON]  chained 0 val=0 idle
  on: led_set(0,0,0)
rule_02 'test step3' [ON]  chained 0 val=0 idle
  on: led_set(0,255,0)
  chain: ->rule_01 (10s)
rule_03 'test step2' [ON]  chained 0 val=0 idle
  on: telegram "hello test"
  chain: ->rule_02 (0s)
rule_04 'test step1' [ON] test gt 100 val=0 idle
  on: telegram "Test sensor value: {test}"
  chain: ->rule_03 (5s)
```

---

### GPT-5 Mini

GPT-5 Mini fills in every parameter including unused ones with defaults — functionally identical but verbose (~1000 bytes of arguments, ~2500 bytes JSON-escaped in the echo-back).

**Serial output:**

```
[TG] Message from user: please send me a telegram message when the test sensor > 100,
  then wait 5s, then send me another message "hello test", then set the led to green,
  and after another 10s set the led off

--- Thinking... ---
[Agent] 1 tool call(s) in iteration 1:
  -> chain_create({"sensor_name":"test","condition":"gt","threshold":100,
       "interval_seconds":1,
       "step1_action":"telegram","step1_message":"Test sensor exceeded 100: {test}",
       "step1_r":0,"step1_g":0,"step1_b":0,"step1_pin":0,"step1_value":0,
       "step1_actuator":"","step1_nats_subject":"",
       "step2_action":"telegram","step2_delay":5,"step2_message":"hello test",
       "step2_r":0,"step2_g":0,"step2_b":0,"step2_pin":0,"step2_value":0,
       "step2_actuator":"","step2_nats_subject":"",
       "step3_action":"led_set","step3_delay":0,"step3_message":"",
       "step3_r":0,"step3_g":255,"step3_b":0,"step3_pin":0,"step3_value":0,
       "step3_actuator":"","step3_nats_subject":"",
       "step4_action":"led_set","step4_delay":10,"step4_message":"",
       "step4_r":0,"step4_g":0,"step4_b":0,"step4_pin":0,"step4_value":0,
       "step4_actuator":"","step4_nats_subject":"",
       "step5_action":"","step5_delay":0,"step5_message":"",
       "step5_r":0,"step5_g":0,"step5_b":0,"step5_pin":0,"step5_value":0,
       "step5_actuator":"","step5_nats_subject":""})
     = Chain created: rule_04 test>100 -> telegram -> 5s -> telegram -> LED(0,255,0) -> 10s -> LED(0,0,0)

Done — chain automation created as rule_04: when test > 100 send Telegram, after 5s
send "hello test", then set LED green, and after 10s turn LED off.
--- (16054ms, 2756+45 tokens) ---
```

**Runtime:**

```
[Rule] rule_04 'test step1' TRIGGERED (reading=1000, threshold=100)

--- 5 seconds later ---

[Rule] rule_03 'test step2' CHAIN-TRIGGERED
[Rule] rule_02 'test step3' CHAIN-TRIGGERED

--- 10 seconds later ---

[Rule] rule_01 'test step4' CHAIN-TRIGGERED

--- sensor drops below threshold ---

[Rule] rule_04 'test step1' CLEARED (reading=10)
```

**`/rules` output:**

```
rule_01 'test step4' [ON]  chained 0 val=0 idle
  on: led_set(0,0,0)
rule_02 'test step3' [ON]  chained 0 val=0 idle
  on: led_set(0,255,0)
  chain: ->rule_01 (10s)
rule_03 'test step2' [ON]  chained 0 val=0 idle
  on: telegram "hello test"
  chain: ->rule_02 (0s)
rule_04 'test step1' [ON] test gt 100 val=1000 FIRED
  on: telegram "Test sensor exceeded 100: {test}"
  chain: ->rule_03 (5s)
```

---

### Key Differences

| | Sonnet 4.5 | Aurora Alpha | Gemini 2.5 Flash | GPT-OSS-120B | GPT-5 Mini |
|---|---|---|---|---|---|
| Parameters sent | Only required | Only required | Only required (random key order) | Only required | Every parameter including defaults |
| Tool call arguments | ~400 bytes | ~400 bytes | ~400 bytes | ~400 bytes | ~1000 bytes |
| `tool_calls_json` echoed back | ~600 bytes | ~600 bytes | ~600 bytes | ~600 bytes | ~2500 bytes (JSON-escaped) |
| Sets `interval_seconds` | No (uses default 5s) | No (uses default 5s) | No (uses default 5s) | No (uses default 5s) | Yes (set to 1s) |
| Message template | `{value}` | `{value}` | `{test}` (sensor name) | `{value}` | `{test}` (sensor name) |
| Response time | ~10s (9782 prompt tokens) | **~4s** (5189 prompt tokens) | **~4s** (4756 prompt tokens) | ~8s (5199 prompt tokens) | ~16s (2756 prompt tokens) |
| Response style | Markdown with bold labels | Markdown with bold + arrows | Numbered list, plain text | Markdown with bold | Concise one-liner |

All five produce identical chains. The verbose GPT-5 Mini style required increasing the `tool_calls_json` buffer from 1KB to 4KB to avoid truncation when echoing the tool call back to the API on the follow-up request.
