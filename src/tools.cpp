/**
 * @file tools.cpp
 * @brief ESP32 tool definitions and handlers for LLM tool calling
 */

#include "tools.h"
#include "llm_client.h"
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <nats_atoms.h>

/* Forward declarations (defined in main.cpp) */
extern void led(uint8_t r, uint8_t g, uint8_t b);
extern void ledOff();
extern bool g_led_user;
extern NatsClient natsClient;
extern bool g_nats_connected;

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

/*============================================================================
 * Tool Definitions (OpenAI function calling format)
 *============================================================================*/

static const char *TOOLS_JSON = R"JSON([
  {
    "type": "function",
    "function": {
      "name": "led_set",
      "description": "Set the RGB LED color on the ESP32 board. Use r,g,b values 0-255. Set all to 0 to turn off.",
      "parameters": {
        "type": "object",
        "properties": {
          "r": {"type": "integer", "description": "Red component 0-255"},
          "g": {"type": "integer", "description": "Green component 0-255"},
          "b": {"type": "integer", "description": "Blue component 0-255"}
        },
        "required": ["r", "g", "b"]
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "gpio_write",
      "description": "Set a GPIO pin to HIGH (1) or LOW (0). Configures the pin as output first.",
      "parameters": {
        "type": "object",
        "properties": {
          "pin": {"type": "integer", "description": "GPIO pin number"},
          "value": {"type": "integer", "description": "0 for LOW, 1 for HIGH"}
        },
        "required": ["pin", "value"]
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "gpio_read",
      "description": "Read the digital state of a GPIO pin. Returns 0 (LOW) or 1 (HIGH).",
      "parameters": {
        "type": "object",
        "properties": {
          "pin": {"type": "integer", "description": "GPIO pin number"}
        },
        "required": ["pin"]
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "device_info",
      "description": "Get device information: free heap memory, uptime, WiFi status, chip info.",
      "parameters": {
        "type": "object",
        "properties": {}
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "file_read",
      "description": "Read a file from the ESP32 filesystem (LittleFS). Returns file contents.",
      "parameters": {
        "type": "object",
        "properties": {
          "path": {"type": "string", "description": "File path, e.g. /system_prompt.txt"}
        },
        "required": ["path"]
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "file_write",
      "description": "Write content to a file on the ESP32 filesystem (LittleFS). Creates or overwrites the file.",
      "parameters": {
        "type": "object",
        "properties": {
          "path": {"type": "string", "description": "File path, e.g. /notes.txt"},
          "content": {"type": "string", "description": "Content to write"}
        },
        "required": ["path", "content"]
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "nats_publish",
      "description": "Publish a message to a NATS subject. Use this to send alerts, trigger other devices, or communicate with external systems.",
      "parameters": {
        "type": "object",
        "properties": {
          "subject": {"type": "string", "description": "NATS subject to publish to, e.g. home.alert"},
          "payload": {"type": "string", "description": "Message payload string"}
        },
        "required": ["subject", "payload"]
      }
    }
  }
])JSON";

/*============================================================================
 * Tool Handlers
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
    } else {
        snprintf(result, result_len, "Error: unknown tool '%s'", name);
        return false;
    }
    return true;
}
