/**
 * @file tools.cpp
 * @brief ESP32 tool definitions and handlers for LLM tool calling
 */

#include "tools.h"
#include "llm_client.h"
#include "devices.h"
#include "rules.h"
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <nats_atoms.h>
#include "driver/temperature_sensor.h"

/* Forward declarations (defined in main.cpp) */
extern void led(uint8_t r, uint8_t g, uint8_t b);
extern void ledOff();
extern bool g_led_user;
extern NatsClient natsClient;
extern bool g_nats_connected;
extern temperature_sensor_handle_t g_temp_sensor;

/*============================================================================
 * Simple JSON argument parser (for tool arguments)
 *============================================================================*/

static int jsonArgInt(const char *json, const char *key, int default_val) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    return atoi(p);
}

static bool jsonArgString(const char *json, const char *key,
                            char *dst, int dst_len) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    int w = 0;
    while (*p && *p != '"' && w < dst_len - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        dst[w++] = *p++;
    }
    dst[w] = '\0';
    return w > 0;
}

static bool jsonArgBool(const char *json, const char *key, bool default_val) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return default_val;
}

/* Check if a key exists (even with empty string value or null) */
static bool jsonArgExists(const char *json, const char *key) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern) != nullptr;
}

/*============================================================================
 * Tool Definitions (OpenAI function calling format) - compacted
 *============================================================================*/

static const char *TOOLS_JSON = R"JSON([
{"type":"function","function":{"name":"led_set","description":"Set RGB LED color 0-255. Set all to 0 to turn off.","parameters":{"type":"object","properties":{"r":{"type":"integer"},"g":{"type":"integer"},"b":{"type":"integer"}},"required":["r","g","b"]}}},
{"type":"function","function":{"name":"gpio_write","description":"Set a GPIO pin HIGH(1) or LOW(0).","parameters":{"type":"object","properties":{"pin":{"type":"integer"},"value":{"type":"integer","description":"0=LOW, 1=HIGH"}},"required":["pin","value"]}}},
{"type":"function","function":{"name":"gpio_read","description":"Read digital state of a GPIO pin.","parameters":{"type":"object","properties":{"pin":{"type":"integer"}},"required":["pin"]}}},
{"type":"function","function":{"name":"device_info","description":"Get device info: heap, uptime, WiFi, chip.","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"file_read","description":"Read a file from LittleFS.","parameters":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}}},
{"type":"function","function":{"name":"file_write","description":"Write content to a LittleFS file.","parameters":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]}}},
{"type":"function","function":{"name":"nats_publish","description":"Publish a message to a NATS subject.","parameters":{"type":"object","properties":{"subject":{"type":"string"},"payload":{"type":"string"}},"required":["subject","payload"]}}},
{"type":"function","function":{"name":"temperature_read","description":"Read ESP32 internal chip temperature (Celsius).","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"device_register","description":"Register a named sensor or actuator. Types: digital_in, analog_in, ntc_10k, ldr, digital_out, relay, pwm.","parameters":{"type":"object","properties":{"name":{"type":"string","description":"Device name, e.g. 'temperature'"},"type":{"type":"string","description":"Device type: digital_in, analog_in, ntc_10k, ldr, digital_out, relay, pwm"},"pin":{"type":"integer","description":"GPIO pin number"},"unit":{"type":"string","description":"Unit label, e.g. C, lux"},"inverted":{"type":"boolean","description":"For relays: LOW=ON"}},"required":["name","type","pin"]}}},
{"type":"function","function":{"name":"device_list","description":"List all registered devices with current readings.","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"device_remove","description":"Remove a registered device by name.","parameters":{"type":"object","properties":{"name":{"type":"string"}},"required":["name"]}}},
{"type":"function","function":{"name":"sensor_read","description":"Read a named sensor. Returns value with unit.","parameters":{"type":"object","properties":{"name":{"type":"string","description":"Sensor device name"}},"required":["name"]}}},
{"type":"function","function":{"name":"actuator_set","description":"Set a named actuator. Value: 0/1 for digital/relay, 0-255 for PWM.","parameters":{"type":"object","properties":{"name":{"type":"string"},"value":{"type":"integer"}},"required":["name","value"]}}},
{"type":"function","function":{"name":"rule_create","description":"Create an automation rule. Evaluates sensor condition periodically, executes action on trigger. Use actuator_name for device registry actuators, or on_action with on_pin/on_value for raw GPIO. For LED actions use on_action='led_set' with on_r/on_g/on_b (0-255). off_action 'auto' reverses when condition clears.","parameters":{"type":"object","properties":{"rule_name":{"type":"string","description":"Rule name"},"sensor_name":{"type":"string","description":"Named sensor from device registry"},"sensor_pin":{"type":"integer","description":"Raw GPIO pin (if no sensor_name)"},"condition":{"type":"string","description":"gt, lt, eq, neq, change, always"},"threshold":{"type":"integer","description":"Threshold value"},"interval_seconds":{"type":"integer","description":"Check interval in seconds (default 5)"},"actuator_name":{"type":"string","description":"Named actuator - auto-sets on/off actions"},"on_action":{"type":"string","description":"gpio_write, led_set, nats_publish, actuator, telegram"},"on_pin":{"type":"integer"},"on_value":{"type":"integer"},"on_r":{"type":"integer","description":"LED red 0-255 (for led_set)"},"on_g":{"type":"integer","description":"LED green 0-255 (for led_set)"},"on_b":{"type":"integer","description":"LED blue 0-255 (for led_set)"},"on_nats_subject":{"type":"string"},"on_nats_payload":{"type":"string"},"on_telegram_message":{"type":"string","description":"Message to send via Telegram (for telegram action)"},"off_action":{"type":"string","description":"auto, none, gpio_write, led_set, nats_publish, actuator, telegram"},"off_pin":{"type":"integer"},"off_value":{"type":"integer"},"off_r":{"type":"integer","description":"LED red 0-255 (for led_set)"},"off_g":{"type":"integer","description":"LED green 0-255 (for led_set)"},"off_b":{"type":"integer","description":"LED blue 0-255 (for led_set)"},"off_nats_subject":{"type":"string"},"off_nats_payload":{"type":"string"},"off_telegram_message":{"type":"string","description":"Message to send via Telegram (for telegram action)"}},"required":["rule_name","condition","threshold"]}}},
{"type":"function","function":{"name":"rule_list","description":"List all rules with status, last readings, triggered state.","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"rule_delete","description":"Delete a rule by ID (e.g. rule_01) or 'all'.","parameters":{"type":"object","properties":{"rule_id":{"type":"string"}},"required":["rule_id"]}}},
{"type":"function","function":{"name":"rule_enable","description":"Enable or disable a rule without deleting it.","parameters":{"type":"object","properties":{"rule_id":{"type":"string"},"enabled":{"type":"boolean"}},"required":["rule_id","enabled"]}}}
])JSON";

/*============================================================================
 * Original Tool Handlers
 *============================================================================*/

static void tool_led_set(const char *args, char *result, int result_len) {
    int r = jsonArgInt(args, "r", 0);
    int g = jsonArgInt(args, "g", 0);
    int b = jsonArgInt(args, "b", 0);

    r = constrain(r, 0, 255);
    g = constrain(g, 0, 255);
    b = constrain(b, 0, 255);

    led((uint8_t)r, (uint8_t)g, (uint8_t)b);
    g_led_user = true;
    snprintf(result, result_len, "LED set to RGB(%d, %d, %d)", r, g, b);
}

static void tool_gpio_write(const char *args, char *result, int result_len) {
    int pin = jsonArgInt(args, "pin", -1);
    int value = jsonArgInt(args, "value", 0);

    if (pin < 0 || pin > 30) {
        snprintf(result, result_len, "Error: invalid pin %d (must be 0-30)", pin);
        return;
    }

    pinMode(pin, OUTPUT);
    digitalWrite(pin, value ? HIGH : LOW);
    snprintf(result, result_len, "GPIO %d set to %s", pin, value ? "HIGH" : "LOW");
}

static void tool_gpio_read(const char *args, char *result, int result_len) {
    int pin = jsonArgInt(args, "pin", -1);

    if (pin < 0 || pin > 30) {
        snprintf(result, result_len, "Error: invalid pin %d (must be 0-30)", pin);
        return;
    }

    pinMode(pin, INPUT);
    int value = digitalRead(pin);
    snprintf(result, result_len, "GPIO %d = %d (%s)", pin, value,
             value ? "HIGH" : "LOW");
}

static void tool_device_info(const char *args, char *result, int result_len) {
    (void)args;
    snprintf(result, result_len,
        "Free heap: %u bytes, Total heap: %u bytes, "
        "Uptime: %lu seconds, "
        "WiFi: %s, IP: %s, "
        "Chip: %s rev %d, %d cores, %lu MHz",
        ESP.getFreeHeap(), ESP.getHeapSize(),
        millis() / 1000,
        WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
        WiFi.localIP().toString().c_str(),
        ESP.getChipModel(), ESP.getChipRevision(),
        ESP.getChipCores(), ESP.getCpuFreqMHz());
}

static void tool_file_read(const char *args, char *result, int result_len) {
    char path[64];
    if (!jsonArgString(args, "path", path, sizeof(path))) {
        snprintf(result, result_len, "Error: missing 'path' argument");
        return;
    }

    File f = LittleFS.open(path, "r");
    if (!f) {
        snprintf(result, result_len, "Error: file not found: %s", path);
        return;
    }

    int len = f.readBytes(result, result_len - 1);
    result[len] = '\0';
    f.close();
}

static void tool_file_write(const char *args, char *result, int result_len) {
    char path[64];
    char content[512];

    if (!jsonArgString(args, "path", path, sizeof(path))) {
        snprintf(result, result_len, "Error: missing 'path' argument");
        return;
    }

    /* Protect config.json from being overwritten */
    if (strcmp(path, "/config.json") == 0) {
        snprintf(result, result_len, "Error: cannot overwrite config.json via tool");
        return;
    }

    if (!jsonArgString(args, "content", content, sizeof(content))) {
        snprintf(result, result_len, "Error: missing 'content' argument");
        return;
    }

    File f = LittleFS.open(path, "w");
    if (!f) {
        snprintf(result, result_len, "Error: cannot open %s for writing", path);
        return;
    }

    f.print(content);
    f.close();
    snprintf(result, result_len, "Wrote %d bytes to %s", (int)strlen(content), path);
}

static void tool_nats_publish(const char *args, char *result, int result_len) {
    if (!g_nats_connected) {
        snprintf(result, result_len, "Error: NATS not connected");
        return;
    }

    char subject[128];
    char payload[256];

    if (!jsonArgString(args, "subject", subject, sizeof(subject))) {
        snprintf(result, result_len, "Error: missing 'subject' argument");
        return;
    }

    if (!jsonArgString(args, "payload", payload, sizeof(payload))) {
        snprintf(result, result_len, "Error: missing 'payload' argument");
        return;
    }

    nats_err_t err = natsClient.publish(subject, payload);
    if (err == NATS_OK) {
        snprintf(result, result_len, "Published to %s: %s", subject, payload);
    } else {
        snprintf(result, result_len, "Error: publish failed: %s",
                 nats_err_str(err));
    }
}

static void tool_temperature_read(const char *args, char *result, int result_len) {
    (void)args;
    float temp = 0.0f;
    if (g_temp_sensor != NULL) {
        temperature_sensor_get_celsius(g_temp_sensor, &temp);
    }
    snprintf(result, result_len, "Chip temperature: %.1f C", temp);
}

/*============================================================================
 * Device Registry Tool Handlers
 *============================================================================*/

static void tool_device_register(const char *args, char *result, int result_len) {
    char name[DEV_NAME_LEN];
    char type_str[24];

    if (!jsonArgString(args, "name", name, sizeof(name))) {
        snprintf(result, result_len, "Error: missing 'name'");
        return;
    }
    if (!jsonArgString(args, "type", type_str, sizeof(type_str))) {
        snprintf(result, result_len, "Error: missing 'type'");
        return;
    }

    int pin = jsonArgInt(args, "pin", PIN_NONE);

    char unit[DEV_UNIT_LEN] = "";
    jsonArgString(args, "unit", unit, sizeof(unit));
    bool inverted = jsonArgBool(args, "inverted", false);

    /* Map type string to DeviceKind */
    DeviceKind kind;
    if (strcmp(type_str, "digital_in") == 0)         kind = DEV_SENSOR_DIGITAL;
    else if (strcmp(type_str, "analog_in") == 0)     kind = DEV_SENSOR_ANALOG_RAW;
    else if (strcmp(type_str, "ntc_10k") == 0)       kind = DEV_SENSOR_NTC_10K;
    else if (strcmp(type_str, "ldr") == 0)            kind = DEV_SENSOR_LDR;
    else if (strcmp(type_str, "digital_out") == 0)   kind = DEV_ACTUATOR_DIGITAL;
    else if (strcmp(type_str, "relay") == 0)          kind = DEV_ACTUATOR_RELAY;
    else if (strcmp(type_str, "pwm") == 0)            kind = DEV_ACTUATOR_PWM;
    else {
        snprintf(result, result_len, "Error: unknown type '%s'", type_str);
        return;
    }

    if (!deviceRegister(name, kind, (uint8_t)pin, unit, inverted)) {
        snprintf(result, result_len, "Error: register failed (duplicate name or full)");
        return;
    }

    devicesSave();
    snprintf(result, result_len, "Registered %s '%s' on pin %d",
             deviceIsSensor(kind) ? "sensor" : "actuator", name, pin);
}

static void tool_device_list(const char *args, char *result, int result_len) {
    (void)args;
    const Device *devs = deviceGetAll();
    int w = 0;
    int count = 0;

    for (int i = 0; i < MAX_DEVICES && w < result_len - 40; i++) {
        if (!devs[i].used) continue;
        const Device *d = &devs[i];

        if (count > 0) w += snprintf(result + w, result_len - w, "; ");

        if (deviceIsSensor(d->kind)) {
            float val = deviceReadSensor(d);
            w += snprintf(result + w, result_len - w, "%s(%s pin%d)=%.1f%s",
                         d->name, deviceKindName(d->kind), d->pin, val, d->unit);
        } else {
            w += snprintf(result + w, result_len - w, "%s(%s pin%d%s)",
                         d->name, deviceKindName(d->kind), d->pin,
                         d->inverted ? " inv" : "");
        }
        count++;
    }

    if (count == 0) {
        snprintf(result, result_len, "No devices registered");
    }
}

static void tool_device_remove(const char *args, char *result, int result_len) {
    char name[DEV_NAME_LEN];
    if (!jsonArgString(args, "name", name, sizeof(name))) {
        snprintf(result, result_len, "Error: missing 'name'");
        return;
    }

    if (!deviceRemove(name)) {
        snprintf(result, result_len, "Error: device '%s' not found", name);
        return;
    }

    devicesSave();
    snprintf(result, result_len, "Removed device '%s'", name);
}

static void tool_sensor_read(const char *args, char *result, int result_len) {
    char name[DEV_NAME_LEN];
    if (!jsonArgString(args, "name", name, sizeof(name))) {
        snprintf(result, result_len, "Error: missing 'name'");
        return;
    }

    Device *dev = deviceFind(name);
    if (!dev) {
        snprintf(result, result_len, "Error: sensor '%s' not found", name);
        return;
    }
    if (!deviceIsSensor(dev->kind)) {
        snprintf(result, result_len, "Error: '%s' is not a sensor", name);
        return;
    }

    float val = deviceReadSensor(dev);
    snprintf(result, result_len, "%s: %.1f %s", name, val, dev->unit);
}

static void tool_actuator_set(const char *args, char *result, int result_len) {
    char name[DEV_NAME_LEN];
    if (!jsonArgString(args, "name", name, sizeof(name))) {
        snprintf(result, result_len, "Error: missing 'name'");
        return;
    }

    int value = jsonArgInt(args, "value", 0);

    Device *dev = deviceFind(name);
    if (!dev) {
        snprintf(result, result_len, "Error: actuator '%s' not found", name);
        return;
    }
    if (!deviceIsActuator(dev->kind)) {
        snprintf(result, result_len, "Error: '%s' is not an actuator", name);
        return;
    }

    if (!deviceSetActuator(dev, value)) {
        snprintf(result, result_len, "Error: failed to set '%s'", name);
        return;
    }

    snprintf(result, result_len, "Set %s to %d", name, value);
}

/*============================================================================
 * Rule Engine Tool Handlers
 *============================================================================*/

static ActionType parseActionType(const char *s) {
    if (strcmp(s, "gpio_write") == 0)   return ACT_GPIO_WRITE;
    if (strcmp(s, "led_set") == 0)      return ACT_LED_SET;
    if (strcmp(s, "nats_publish") == 0) return ACT_NATS_PUBLISH;
    if (strcmp(s, "actuator") == 0)     return ACT_ACTUATOR;
    if (strcmp(s, "telegram") == 0)     return ACT_TELEGRAM;
    return ACT_GPIO_WRITE;
}

static ConditionOp parseConditionOp(const char *s) {
    if (strcmp(s, "gt") == 0)     return COND_GT;
    if (strcmp(s, "lt") == 0)     return COND_LT;
    if (strcmp(s, "eq") == 0)     return COND_EQ;
    if (strcmp(s, "neq") == 0)    return COND_NEQ;
    if (strcmp(s, "change") == 0) return COND_CHANGE;
    if (strcmp(s, "always") == 0) return COND_ALWAYS;
    return COND_GT;
}

static void tool_rule_create(const char *args, char *result, int result_len) {
    char rule_name[RULE_NAME_LEN];
    if (!jsonArgString(args, "rule_name", rule_name, sizeof(rule_name))) {
        snprintf(result, result_len, "Error: missing 'rule_name'");
        return;
    }

    char cond_str[16];
    if (!jsonArgString(args, "condition", cond_str, sizeof(cond_str))) {
        snprintf(result, result_len, "Error: missing 'condition'");
        return;
    }
    ConditionOp condition = parseConditionOp(cond_str);
    int32_t threshold = jsonArgInt(args, "threshold", 0);

    /* Sensor source: device name or raw pin */
    char sensor_name[DEV_NAME_LEN] = "";
    jsonArgString(args, "sensor_name", sensor_name, sizeof(sensor_name));
    uint8_t sensor_pin = (uint8_t)jsonArgInt(args, "sensor_pin", PIN_NONE);
    bool sensor_analog = false;

    /* Validate sensor source */
    if (sensor_name[0]) {
        Device *dev = deviceFind(sensor_name);
        if (!dev) {
            snprintf(result, result_len, "Error: sensor '%s' not found in device registry", sensor_name);
            return;
        }
        if (!deviceIsSensor(dev->kind)) {
            snprintf(result, result_len, "Error: '%s' is not a sensor", sensor_name);
            return;
        }
    } else if (sensor_pin == PIN_NONE) {
        snprintf(result, result_len, "Error: provide sensor_name or sensor_pin");
        return;
    }

    int interval_s = jsonArgInt(args, "interval_seconds", 5);
    uint32_t interval_ms = (uint32_t)interval_s * 1000;

    /* Determine ON action */
    ActionType on_action = ACT_GPIO_WRITE;
    char on_actuator[DEV_NAME_LEN] = "";
    uint8_t on_pin = 0;
    int32_t on_value = 1;
    char on_nats_subj[RULE_NATS_SUBJ_LEN] = "";
    char on_nats_pay[RULE_NATS_PAY_LEN] = "";

    /* Determine OFF action */
    bool has_off = false;
    ActionType off_action = ACT_GPIO_WRITE;
    char off_actuator[DEV_NAME_LEN] = "";
    uint8_t off_pin = 0;
    int32_t off_value = 0;
    char off_nats_subj[RULE_NATS_SUBJ_LEN] = "";
    char off_nats_pay[RULE_NATS_PAY_LEN] = "";

    /* Check for actuator_name shorthand */
    char actuator_name[DEV_NAME_LEN] = "";
    jsonArgString(args, "actuator_name", actuator_name, sizeof(actuator_name));

    if (actuator_name[0]) {
        /* Validate actuator exists */
        Device *act = deviceFind(actuator_name);
        if (!act) {
            snprintf(result, result_len, "Error: actuator '%s' not found", actuator_name);
            return;
        }
        if (!deviceIsActuator(act->kind)) {
            snprintf(result, result_len, "Error: '%s' is not an actuator", actuator_name);
            return;
        }

        on_action = ACT_ACTUATOR;
        strncpy(on_actuator, actuator_name, DEV_NAME_LEN - 1);

        /* Auto-set off action */
        has_off = true;
        off_action = ACT_ACTUATOR;
        strncpy(off_actuator, actuator_name, DEV_NAME_LEN - 1);
    } else {
        /* Explicit on_action */
        char act_str[24] = "";
        jsonArgString(args, "on_action", act_str, sizeof(act_str));
        if (act_str[0]) {
            on_action = parseActionType(act_str);
        }

        on_pin = (uint8_t)jsonArgInt(args, "on_pin", 0);
        on_value = jsonArgInt(args, "on_value", 1);
        jsonArgString(args, "on_nats_subject", on_nats_subj, sizeof(on_nats_subj));
        jsonArgString(args, "on_nats_payload", on_nats_pay, sizeof(on_nats_pay));

        /* For telegram action, store message in on_nats_pay (reused field) */
        if (on_action == ACT_TELEGRAM) {
            jsonArgString(args, "on_telegram_message", on_nats_pay, sizeof(on_nats_pay));
        }

        /* Pack on_r/on_g/on_b into on_value for led_set */
        if (on_action == ACT_LED_SET && jsonArgExists(args, "on_r")) {
            int r = constrain(jsonArgInt(args, "on_r", 0), 0, 255);
            int g = constrain(jsonArgInt(args, "on_g", 0), 0, 255);
            int b = constrain(jsonArgInt(args, "on_b", 0), 0, 255);
            on_value = (r << 16) | (g << 8) | b;
        }
    }

    /* Check for explicit off_action */
    char off_act_str[24] = "";
    jsonArgString(args, "off_action", off_act_str, sizeof(off_act_str));
    if (off_act_str[0]) {
        if (strcmp(off_act_str, "none") == 0) {
            has_off = false;
        } else if (strcmp(off_act_str, "auto") == 0) {
            has_off = true;
            off_action = on_action;
            strncpy(off_actuator, on_actuator, DEV_NAME_LEN - 1);
            off_pin = on_pin;
            off_value = 0;
            /* For led_set auto-off, check if off_r/off_g/off_b provided */
            if (off_action == ACT_LED_SET && jsonArgExists(args, "off_r")) {
                int r = constrain(jsonArgInt(args, "off_r", 0), 0, 255);
                int g = constrain(jsonArgInt(args, "off_g", 0), 0, 255);
                int b = constrain(jsonArgInt(args, "off_b", 0), 0, 255);
                off_value = (r << 16) | (g << 8) | b;
            }
            /* For telegram auto-off, check if off_telegram_message provided */
            if (off_action == ACT_TELEGRAM) {
                jsonArgString(args, "off_telegram_message", off_nats_pay, sizeof(off_nats_pay));
            }
        } else {
            has_off = true;
            off_action = parseActionType(off_act_str);
            off_pin = (uint8_t)jsonArgInt(args, "off_pin", 0);
            off_value = jsonArgInt(args, "off_value", 0);
            jsonArgString(args, "off_nats_subject", off_nats_subj, sizeof(off_nats_subj));
            jsonArgString(args, "off_nats_payload", off_nats_pay, sizeof(off_nats_pay));
            /* Pack off_r/off_g/off_b into off_value for led_set */
            if (off_action == ACT_LED_SET && jsonArgExists(args, "off_r")) {
                int r = constrain(jsonArgInt(args, "off_r", 0), 0, 255);
                int g = constrain(jsonArgInt(args, "off_g", 0), 0, 255);
                int b = constrain(jsonArgInt(args, "off_b", 0), 0, 255);
                off_value = (r << 16) | (g << 8) | b;
            }
            /* For telegram action, store message in off_nats_pay */
            if (off_action == ACT_TELEGRAM) {
                jsonArgString(args, "off_telegram_message", off_nats_pay, sizeof(off_nats_pay));
            }
        }
    }

    const char *id = ruleCreate(rule_name, sensor_name, sensor_pin, sensor_analog,
                                condition, threshold, interval_ms,
                                on_action, on_actuator, on_pin, on_value,
                                on_nats_subj, on_nats_pay,
                                has_off, off_action, off_actuator,
                                off_pin, off_value, off_nats_subj, off_nats_pay);

    if (!id) {
        snprintf(result, result_len, "Error: rule creation failed (max %d rules)", MAX_RULES);
        return;
    }

    rulesSave();

    /* Build descriptive response */
    const char *cond_sym = "?";
    switch (condition) {
        case COND_GT: cond_sym = ">"; break;
        case COND_LT: cond_sym = "<"; break;
        case COND_EQ: cond_sym = "=="; break;
        case COND_NEQ: cond_sym = "!="; break;
        case COND_CHANGE: cond_sym = "changed"; break;
        case COND_ALWAYS: cond_sym = "always"; break;
    }

    const char *src = sensor_name[0] ? sensor_name : "pin";
    snprintf(result, result_len, "Rule created: %s '%s' - %s %s %d (every %ds)%s",
             id, rule_name, src, cond_sym, (int)threshold, interval_s,
             has_off ? " with auto-off" : "");
}

static void tool_rule_list(const char *args, char *result, int result_len) {
    (void)args;
    const Rule *rules = ruleGetAll();
    int w = 0;
    int count = 0;

    for (int i = 0; i < MAX_RULES && w < result_len - 60; i++) {
        if (!rules[i].used) continue;
        const Rule *r = &rules[i];

        if (count > 0) w += snprintf(result + w, result_len - w, "; ");

        w += snprintf(result + w, result_len - w,
            "%s '%s' %s %s%s %d val=%d %s",
            r->id, r->name,
            r->enabled ? "ON" : "OFF",
            r->sensor_name[0] ? r->sensor_name : "pin",
            r->sensor_name[0] ? "" : "",
            (int)r->threshold,
            (int)r->last_reading,
            r->fired ? "FIRED" : "idle");
        count++;
    }

    if (count == 0) {
        snprintf(result, result_len, "No rules defined");
    }
}

static void tool_rule_delete(const char *args, char *result, int result_len) {
    char rule_id[RULE_ID_LEN];
    if (!jsonArgString(args, "rule_id", rule_id, sizeof(rule_id))) {
        snprintf(result, result_len, "Error: missing 'rule_id'");
        return;
    }

    if (!ruleDelete(rule_id)) {
        snprintf(result, result_len, "Error: rule '%s' not found", rule_id);
        return;
    }

    rulesSave();

    if (strcmp(rule_id, "all") == 0)
        snprintf(result, result_len, "All rules deleted");
    else
        snprintf(result, result_len, "Deleted rule %s", rule_id);
}

static void tool_rule_enable(const char *args, char *result, int result_len) {
    char rule_id[RULE_ID_LEN];
    if (!jsonArgString(args, "rule_id", rule_id, sizeof(rule_id))) {
        snprintf(result, result_len, "Error: missing 'rule_id'");
        return;
    }

    bool enabled = jsonArgBool(args, "enabled", true);

    if (!ruleEnable(rule_id, enabled)) {
        snprintf(result, result_len, "Error: rule '%s' not found", rule_id);
        return;
    }

    rulesSave();
    snprintf(result, result_len, "Rule %s %s", rule_id, enabled ? "enabled" : "disabled");
}

/*============================================================================
 * Public API
 *============================================================================*/

const char *toolsGetDefinitions() {
    return TOOLS_JSON;
}

bool toolExecute(const char *name, const char *args_json,
                  char *result, int result_len) {
    if (strcmp(name, "led_set") == 0) {
        tool_led_set(args_json, result, result_len);
    } else if (strcmp(name, "gpio_write") == 0) {
        tool_gpio_write(args_json, result, result_len);
    } else if (strcmp(name, "gpio_read") == 0) {
        tool_gpio_read(args_json, result, result_len);
    } else if (strcmp(name, "device_info") == 0) {
        tool_device_info(args_json, result, result_len);
    } else if (strcmp(name, "file_read") == 0) {
        tool_file_read(args_json, result, result_len);
    } else if (strcmp(name, "file_write") == 0) {
        tool_file_write(args_json, result, result_len);
    } else if (strcmp(name, "nats_publish") == 0) {
        tool_nats_publish(args_json, result, result_len);
    } else if (strcmp(name, "temperature_read") == 0) {
        tool_temperature_read(args_json, result, result_len);
    /* Device registry tools */
    } else if (strcmp(name, "device_register") == 0) {
        tool_device_register(args_json, result, result_len);
    } else if (strcmp(name, "device_list") == 0) {
        tool_device_list(args_json, result, result_len);
    } else if (strcmp(name, "device_remove") == 0) {
        tool_device_remove(args_json, result, result_len);
    } else if (strcmp(name, "sensor_read") == 0) {
        tool_sensor_read(args_json, result, result_len);
    } else if (strcmp(name, "actuator_set") == 0) {
        tool_actuator_set(args_json, result, result_len);
    /* Rule engine tools */
    } else if (strcmp(name, "rule_create") == 0) {
        tool_rule_create(args_json, result, result_len);
    } else if (strcmp(name, "rule_list") == 0) {
        tool_rule_list(args_json, result, result_len);
    } else if (strcmp(name, "rule_delete") == 0) {
        tool_rule_delete(args_json, result, result_len);
    } else if (strcmp(name, "rule_enable") == 0) {
        tool_rule_enable(args_json, result, result_len);
    } else {
        snprintf(result, result_len, "Error: unknown tool '%s'", name);
        return false;
    }
    return true;
}
