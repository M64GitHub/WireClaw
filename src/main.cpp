/**
 * @file main.cpp
 * @brief esp-claw - ESP32 AI Agent
 *
 * A reimplementation of PicoClaw's core agent loop for ESP32.
 * Phase 5: Polish - history persistence, LED heartbeat, watchdog.
 *
 * Type a message in the serial monitor, get an LLM response.
 * Use 115200 baud, send with newline.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#if !defined(CONFIG_IDF_TARGET_ESP32)
#include "driver/temperature_sensor.h"
#endif
#include "llm_client.h"
#include "tools.h"
#include "devices.h"
#include "rules.h"
#include "setup_portal.h"
#include <nats_atoms.h>

/*============================================================================
 * Configuration
 *============================================================================*/

#define LED_BRIGHTNESS   20
#define SERIAL_BUF_SIZE  512
#define MAX_HISTORY      6  /* Keep last N user+assistant turns (pairs) */

/* Runtime config - loaded from LittleFS, falls back to secrets.h */
static char cfg_wifi_ssid[64];
static char cfg_wifi_pass[64];
static char cfg_api_key[128];
static char cfg_model[64];
static char cfg_device_name[32];
static char cfg_api_base_url[128];
static char cfg_nats_host[64];
static int  cfg_nats_port = 4222;
static char cfg_telegram_token[64];
static char cfg_telegram_chat_id[16];
static char cfg_system_prompt[4096];
static char cfg_timezone[64];
int cfg_telegram_cooldown = 60;  /* seconds, 0 = disabled */

/* Placeholder defaults - overridden by LittleFS config.json */
static void configDefaults() {
    cfg_wifi_ssid[0] = '\0';
    cfg_wifi_pass[0] = '\0';
    cfg_api_key[0] = '\0';
    strncpy(cfg_model, "openai/gpt-4o-mini", sizeof(cfg_model));
    strncpy(cfg_device_name, "esp-claw", sizeof(cfg_device_name));
    cfg_api_base_url[0] = '\0';
    cfg_nats_host[0] = '\0';
    cfg_nats_port = 4222;
    cfg_telegram_token[0] = '\0';
    cfg_telegram_chat_id[0] = '\0';
    strncpy(cfg_timezone, "UTC0", sizeof(cfg_timezone));
    strncpy(cfg_system_prompt,
        "You are esp-claw, a helpful AI assistant running on an ESP32 microcontroller. "
        "Be concise. Keep responses under 200 words unless asked for detail.",
        sizeof(cfg_system_prompt));
}

/*============================================================================
 * LED Helpers (RGB on C6/S3/C3, on/off fallback on classic ESP32)
 *============================================================================*/

static uint8_t ledBrightness = LED_BRIGHTNESS;

void led(uint8_t r, uint8_t g, uint8_t b) {
    r = (uint8_t)((r * ledBrightness) / 255);
    g = (uint8_t)((g * ledBrightness) / 255);
    b = (uint8_t)((b * ledBrightness) / 255);
#ifdef RGB_BUILTIN
    rgbLedWrite(RGB_BUILTIN, r, g, b);
#elif defined(LED_BUILTIN)
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, (r || g || b) ? HIGH : LOW);
#endif
}

void ledOff()    { led(0, 0, 0); }
void ledRed()    { led(255, 0, 0); }
void ledOrange() { led(255, 80, 0); }
void ledGreen()  { led(0, 255, 0); }
void ledCyan()   { led(0, 255, 255); }
void ledBlue()   { led(0, 0, 255); }
void ledPurple() { led(128, 0, 255); }

/*============================================================================
 * LittleFS Config Loading
 *============================================================================*/

/**
 * Simple JSON string extractor - find "key":"value" and copy value to dst.
 * Returns true if found.
 */
static bool jsonGetString(const char *json, const char *key,
                           char *dst, int dst_len) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;

    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++; /* skip opening quote */

    int w = 0;
    while (*p && *p != '"' && w < dst_len - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++; /* skip backslash, take next char */
        }
        dst[w++] = *p++;
    }
    dst[w] = '\0';
    return w > 0;
}

/**
 * Read a file from LittleFS into a buffer.
 * Returns bytes read, or -1 on error.
 */
static int readFile(const char *path, char *buf, int buf_len) {
    File f = LittleFS.open(path, "r");
    if (!f) return -1;

    int len = f.readBytes(buf, buf_len - 1);
    buf[len] = '\0';
    f.close();
    return len;
}

/**
 * Load config from LittleFS. Falls back to secrets.h defaults.
 */
static bool loadConfig() {
    configDefaults();

    if (!LittleFS.begin(false)) {
        Serial.printf("LittleFS: mount failed (no filesystem?)\n");
        Serial.printf("LittleFS: using compile-time defaults\n");
        return false;
    }

    Serial.printf("LittleFS: mounted OK\n");

    /* Load config.json */
    static char json_buf[1024];
    int len = readFile("/config.json", json_buf, sizeof(json_buf));
    if (len > 0) {
        Serial.printf("LittleFS: loaded config.json (%d bytes)\n", len);
        jsonGetString(json_buf, "wifi_ssid", cfg_wifi_ssid, sizeof(cfg_wifi_ssid));
        jsonGetString(json_buf, "wifi_pass", cfg_wifi_pass, sizeof(cfg_wifi_pass));
        jsonGetString(json_buf, "api_key", cfg_api_key, sizeof(cfg_api_key));
        jsonGetString(json_buf, "model", cfg_model, sizeof(cfg_model));
        jsonGetString(json_buf, "device_name", cfg_device_name, sizeof(cfg_device_name));
        jsonGetString(json_buf, "api_base_url", cfg_api_base_url, sizeof(cfg_api_base_url));
        jsonGetString(json_buf, "nats_host", cfg_nats_host, sizeof(cfg_nats_host));
        char port_buf[8];
        if (jsonGetString(json_buf, "nats_port", port_buf, sizeof(port_buf))) {
            cfg_nats_port = atoi(port_buf);
        }
        jsonGetString(json_buf, "telegram_token", cfg_telegram_token, sizeof(cfg_telegram_token));
        jsonGetString(json_buf, "telegram_chat_id", cfg_telegram_chat_id, sizeof(cfg_telegram_chat_id));
        char cd_buf[8];
        if (jsonGetString(json_buf, "telegram_cooldown", cd_buf, sizeof(cd_buf))) {
            cfg_telegram_cooldown = atoi(cd_buf);
        }
        jsonGetString(json_buf, "timezone", cfg_timezone, sizeof(cfg_timezone));
    } else {
        Serial.printf("LittleFS: no config.json, using defaults\n");
    }

    /* Load system prompt */
    len = readFile("/system_prompt.txt", cfg_system_prompt, sizeof(cfg_system_prompt));
    if (len > 0) {
        Serial.printf("LittleFS: loaded system_prompt.txt (%d bytes)\n", len);
    } else {
        Serial.printf("LittleFS: no system_prompt.txt, using default prompt\n");
    }

    return true;
}

/*============================================================================
 * Globals
 *============================================================================*/

bool g_debug = false;
bool g_led_user = false; /* true when LED was set by a tool - don't overwrite with status */
#if !defined(CONFIG_IDF_TARGET_ESP32)
temperature_sensor_handle_t g_temp_sensor = NULL;
#endif

LlmClient llm;
char serialBuf[SERIAL_BUF_SIZE];
int  serialPos = 0;

/* NATS client (optional - only used if nats_host is configured) */
NatsClient natsClient;
bool g_nats_enabled = false;
bool g_nats_connected = false;
static unsigned long natsLastReconnect = 0;
#define NATS_RECONNECT_DELAY_MS 5000
static char natsSubjectChat[64];
static char natsSubjectCmd[64];
char natsSubjectEvents[64];

/* Conversation history */
struct Turn {
    char user[256];
    char assistant[LLM_MAX_RESPONSE_LEN];
    bool used;
};

static Turn history[MAX_HISTORY];
static int  historyCount = 0;

/*============================================================================
 * History Persistence (LittleFS)
 *============================================================================*/

#define HISTORY_FILE "/history.json"

static int jsonEscape(char *dst, int dst_len, const char *src) {
    int w = 0;
    for (int i = 0; src[i] && w < dst_len - 1; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (w + 2 >= dst_len) break;
            dst[w++] = '\\';
            dst[w++] = c;
        } else if (c == '\n') {
            if (w + 2 >= dst_len) break;
            dst[w++] = '\\';
            dst[w++] = 'n';
        } else if (c == '\r' || (uint8_t)c < 0x20) {
            /* skip control chars */
        } else {
            dst[w++] = c;
        }
    }
    dst[w] = '\0';
    return w;
}

static void historySave() {
    File f = LittleFS.open(HISTORY_FILE, "w");
    if (!f) return;

    static char escaped[LLM_MAX_RESPONSE_LEN + 512];

    f.print("[");
    for (int i = 0; i < historyCount; i++) {
        if (i > 0) f.print(",");
        f.print("{\"u\":\"");
        jsonEscape(escaped, sizeof(escaped), history[i].user);
        f.print(escaped);
        f.print("\",\"a\":\"");
        jsonEscape(escaped, sizeof(escaped), history[i].assistant);
        f.print(escaped);
        f.print("\"}");
    }
    f.print("]");
    f.close();

    if (g_debug) Serial.printf("History: saved %d turns\n", historyCount);
}

static void historyLoad() {
    static char buf[8192];
    int len = readFile(HISTORY_FILE, buf, sizeof(buf));
    if (len <= 0) return;

    historyCount = 0;
    const char *p = buf;

    while (*p && historyCount < MAX_HISTORY) {
        const char *uStart = strstr(p, "\"u\":\"");
        if (!uStart) break;
        uStart += 5;

        int w = 0;
        const char *s = uStart;
        while (*s && *s != '"' && w < (int)sizeof(history[0].user) - 1) {
            if (*s == '\\' && *(s + 1)) {
                s++;
                if (*s == 'n') history[historyCount].user[w++] = '\n';
                else history[historyCount].user[w++] = *s;
            } else {
                history[historyCount].user[w++] = *s;
            }
            s++;
        }
        history[historyCount].user[w] = '\0';

        const char *aStart = strstr(s, "\"a\":\"");
        if (!aStart) break;
        aStart += 5;

        w = 0;
        s = aStart;
        while (*s && *s != '"' && w < (int)sizeof(history[0].assistant) - 1) {
            if (*s == '\\' && *(s + 1)) {
                s++;
                if (*s == 'n') history[historyCount].assistant[w++] = '\n';
                else history[historyCount].assistant[w++] = *s;
            } else {
                history[historyCount].assistant[w++] = *s;
            }
            s++;
        }
        history[historyCount].assistant[w] = '\0';

        history[historyCount].used = true;
        historyCount++;
        p = s;
    }

    if (historyCount > 0) {
        Serial.printf("History: loaded %d turns from %s\n", historyCount, HISTORY_FILE);
    }
}

/*============================================================================
 * Temperature Sensor
 *============================================================================*/

#if !defined(CONFIG_IDF_TARGET_ESP32)
void initTempSensor() {
    temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&config, &g_temp_sensor);
    if (err != ESP_OK) { Serial.printf("Temp sensor install failed: %d\n", err); return; }
    err = temperature_sensor_enable(g_temp_sensor);
    if (err != ESP_OK) { Serial.printf("Temp sensor enable failed: %d\n", err); }
}
#endif

/*============================================================================
 * WiFi
 *============================================================================*/

bool connectWiFi() {
    Serial.printf("WiFi: Connecting to %s", cfg_wifi_ssid);
    ledOrange();

    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg_wifi_ssid, cfg_wifi_pass);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (attempts % 2 == 0) ledOrange(); else ledOff();
        if (++attempts > 30) {
            Serial.println(" FAILED!");
            ledRed();
            return false;
        }
    }

    Serial.printf(" OK!\n");
    Serial.printf("WiFi: IP = %s\n", WiFi.localIP().toString().c_str());
    ledGreen();
    return true;
}

/*============================================================================
 * Chat with LLM - Agentic Loop with Tool Calling
 *============================================================================*/

#define MAX_AGENT_ITERATIONS 5

/* Static storage for tool call results (persists across loop iterations) */
static char toolResultBufs[LLM_MAX_TOOL_CALLS][TOOL_RESULT_MAX_LEN];
static char toolCallJsonBuf[1024]; /* copy of tool_calls_json for message building */
static char memoryBuf[512]; /* persistent AI memory from /memory.txt */

/**
 * Run the agentic chat loop. Returns pointer to the response text
 * (valid until next call), or nullptr on error.
 */
const char *chatWithLLM(const char *userMessage) {
    g_led_user = false; /* Reset - status LEDs allowed until a tool sets the LED */
    ledBlue(); /* Thinking... */

    /*
     * Message array for the full agentic conversation.
     * Layout: system + history pairs + user + [assistant+tool results]*iterations
     * Static to avoid blowing the 8KB loop task stack.
     */
    static LlmMessage messages[LLM_MAX_MESSAGES];
    int msgCount = 0;

    /* System prompt */
    messages[msgCount++] = llmMsg("system", cfg_system_prompt);

    /* Persistent AI memory */
    memoryBuf[0] = '\0';
    int memLen = readFile("/memory.txt", memoryBuf, sizeof(memoryBuf));
    if (memLen > 0) {
        messages[msgCount++] = llmMsg("system", memoryBuf);
    }

    /* History */
    int histStart = msgCount;  /* index where history pairs begin */
    for (int i = 0; i < historyCount && msgCount < LLM_MAX_MESSAGES - 2; i++) {
        messages[msgCount++] = llmMsg("user", history[i].user);
        messages[msgCount++] = llmMsg("assistant", history[i].assistant);
    }
    int histEnd = msgCount;    /* index after last history message */

    /* Current user message */
    messages[msgCount++] = llmMsg("user", userMessage);

    Serial.printf("\n--- Thinking... ---\n");
    unsigned long t0 = millis();

    const char *tools_json = toolsGetDefinitions();
    static LlmResult result;
    int totalPromptTokens = 0;
    int totalCompletionTokens = 0;
    const char *finalContent = nullptr;
    bool ok = false;

    for (int iter = 0; iter < MAX_AGENT_ITERATIONS; iter++) {
        ok = llm.chat(messages, msgCount, tools_json, &result);

        /* If request too large, drop oldest history pair and retry */
        while (!ok && strstr(llm.lastError(), "too large") &&
               histStart + 2 <= histEnd) {
            Serial.printf("[Agent] Request too large, dropping oldest history\n");
            /* Remove 2 messages (user+assistant) at histStart */
            memmove(&messages[histStart], &messages[histStart + 2],
                    (msgCount - histStart - 2) * sizeof(LlmMessage));
            msgCount -= 2;
            histEnd -= 2;
            ok = llm.chat(messages, msgCount, tools_json, &result);
        }
        if (!ok) break;

        totalPromptTokens += result.prompt_tokens;
        totalCompletionTokens += result.completion_tokens;

        /* No tool calls - we're done */
        if (result.tool_call_count == 0) {
            finalContent = result.content;
            break;
        }

        /* Execute tool calls */
        Serial.printf("[Agent] %d tool call(s) in iteration %d:\n",
                      result.tool_call_count, iter + 1);

        /* Save tool_calls_json for message building */
        strncpy(toolCallJsonBuf, result.tool_calls_json, sizeof(toolCallJsonBuf) - 1);
        toolCallJsonBuf[sizeof(toolCallJsonBuf) - 1] = '\0';

        /* Add assistant message with tool calls */
        if (msgCount < LLM_MAX_MESSAGES) {
            messages[msgCount++] = llmToolCallMsg(
                result.content[0] ? result.content : nullptr,
                toolCallJsonBuf);
        }

        /* Execute each tool and add result messages */
        for (int t = 0; t < result.tool_call_count && msgCount < LLM_MAX_MESSAGES; t++) {
            LlmToolCall *tc = &result.tool_calls[t];

            Serial.printf("  -> %s(%s)\n", tc->name, tc->arguments);

            toolExecute(tc->name, tc->arguments,
                        toolResultBufs[t], TOOL_RESULT_MAX_LEN);

            Serial.printf("     = %s\n", toolResultBufs[t]);

            messages[msgCount++] = llmToolResult(tc->id, toolResultBufs[t]);
        }

        if (!g_led_user) ledPurple(); /* Show we're in a tool loop */
    }

    unsigned long elapsed = millis() - t0;

    if (ok && finalContent && finalContent[0]) {
        if (!g_led_user) ledGreen();

        Serial.printf("\n%s\n", finalContent);
        Serial.printf("--- (%lums, %d+%d tokens) ---\n\n",
                      elapsed, totalPromptTokens, totalCompletionTokens);

        /* Save to history (circular buffer) */
        int slot;
        if (historyCount >= MAX_HISTORY) {
            for (int i = 0; i < MAX_HISTORY - 1; i++)
                history[i] = history[i + 1];
            slot = MAX_HISTORY - 1;
        } else {
            slot = historyCount++;
        }
        strncpy(history[slot].user, userMessage, sizeof(history[slot].user) - 1);
        history[slot].user[sizeof(history[slot].user) - 1] = '\0';
        strncpy(history[slot].assistant, finalContent,
                sizeof(history[slot].assistant) - 1);
        history[slot].assistant[sizeof(history[slot].assistant) - 1] = '\0';
        history[slot].used = true;
        historySave();

        return finalContent;

    } else if (ok) {
        /* Tools executed but no final text (LLM only used tools) */
        if (!g_led_user) ledGreen();
        Serial.printf("\n[Agent] Tools executed, no text response.\n");
        Serial.printf("--- (%lums, %d+%d tokens) ---\n\n",
                      elapsed, totalPromptTokens, totalCompletionTokens);
        return "[Tools executed, no text response]";
    } else {
        ledRed();
        Serial.printf("\n[ERROR] LLM call failed: %s\n\n", llm.lastError());
        return nullptr;
    }
}

/*============================================================================
 * NATS Callbacks
 *============================================================================*/

static void onNatsEvent(nats_client_t *client, nats_event_t event,
                        void *userdata) {
    (void)client; (void)userdata;
    switch (event) {
    case NATS_EVENT_CONNECTED:
        Serial.printf("NATS: connected\n");
        g_nats_connected = true;
        break;
    case NATS_EVENT_DISCONNECTED:
        Serial.printf("NATS: disconnected\n");
        g_nats_connected = false;
        break;
    case NATS_EVENT_ERROR:
        Serial.printf("NATS: error: %s\n",
                      nats_err_str(nats_get_last_error(client)));
        break;
    default:
        break;
    }
}

/**
 * NATS chat handler - request/reply. Caller sends a message,
 * we run the agentic loop and respond with the LLM answer.
 */
static void onNatsChat(nats_client_t *client, const nats_msg_t *msg,
                       void *userdata) {
    (void)userdata;
    if (msg->data_len == 0) return;

    /* Copy payload (not null-terminated) */
    static char chatBuf[512];
    size_t len = msg->data_len < sizeof(chatBuf) - 1
                 ? msg->data_len : sizeof(chatBuf) - 1;
    memcpy(chatBuf, msg->data, len);
    chatBuf[len] = '\0';

    Serial.printf("\n[NATS] chat: %s\n", chatBuf);

    const char *response = chatWithLLM(chatBuf);

    /* Reply if caller expects a response */
    if (msg->reply_len > 0) {
        if (response) {
            nats_msg_respond_str(client, msg, response);
        } else {
            nats_msg_respond_str(client, msg, "[error]");
        }
    }

    /* Also publish to events */
    if (response && g_nats_connected) {
        natsClient.publish(natsSubjectEvents, response);
    }

    Serial.printf("> ");
}

/* Shared command response buffer (NATS + Telegram, single-threaded so safe) */
static char cmdResponseBuf[512];

/**
 * Execute a device command, writing compact result to buf.
 * cmd is the command name WITHOUT leading "/" (e.g. "status", "clear").
 * Returns true if the command was recognized and handled.
 */
static bool handleCommand(const char *cmd, char *buf, int buf_len) {
    if (strcmp(cmd, "status") == 0) {
        snprintf(buf, buf_len,
            "WiFi: %s (%s), Heap: %u/%u, History: %d, Model: %s, "
            "Debug: %s, NATS: %s, Uptime: %lus",
            WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
            WiFi.localIP().toString().c_str(),
            ESP.getFreeHeap(), ESP.getHeapSize(),
            historyCount, cfg_model,
            g_debug ? "ON" : "OFF",
            g_nats_enabled
                ? (g_nats_connected ? "connected" : "disconnected")
                : "disabled",
            millis() / 1000);
        return true;
    }
    if (strcmp(cmd, "clear") == 0) {
        historyCount = 0;
        LittleFS.remove(HISTORY_FILE);
        snprintf(buf, buf_len, "History cleared");
        return true;
    }
    if (strcmp(cmd, "heap") == 0) {
        snprintf(buf, buf_len, "Free heap: %u bytes", ESP.getFreeHeap());
        return true;
    }
    if (strcmp(cmd, "debug") == 0) {
        g_debug = !g_debug;
        snprintf(buf, buf_len, "Debug %s", g_debug ? "ON" : "OFF");
        return true;
    }
    if (strcmp(cmd, "devices") == 0) {
        int w = 0;
        const Device *devs = deviceGetAll();
        for (int i = 0; i < MAX_DEVICES && w < buf_len - 80; i++) {
            if (!devs[i].used) continue;
            const Device *d = &devs[i];
            if (w > 0) w += snprintf(buf + w, buf_len - w, "; ");
            if (d->kind == DEV_SENSOR_SERIAL_TEXT) {
                float val = deviceReadSensor(d);
                w += snprintf(buf + w, buf_len - w,
                    "%s[serial %ubaud]=%.1f%s", d->name, (unsigned)d->baud, val, d->unit);
            } else if (d->kind == DEV_SENSOR_NATS_VALUE) {
                float val = deviceReadSensor(d);
                w += snprintf(buf + w, buf_len - w,
                    "%s[%s]=%.1f%s", d->name, d->nats_subject, val, d->unit);
            } else if (deviceIsSensor(d->kind)) {
                float val = deviceReadSensor(d);
                w += snprintf(buf + w, buf_len - w,
                    "%s=%.1f%s", d->name, val, d->unit);
            } else {
                w += snprintf(buf + w, buf_len - w,
                    "%s(%s)", d->name, deviceKindName(d->kind));
            }
        }
        if (w == 0) snprintf(buf, buf_len, "No devices");
        return true;
    }
    if (strcmp(cmd, "rules") == 0) {
        int w = 0;
        const Rule *rules = ruleGetAll();
        for (int i = 0; i < MAX_RULES && w < buf_len - 60; i++) {
            if (!rules[i].used) continue;
            const Rule *r = &rules[i];
            if (w > 0) w += snprintf(buf + w, buf_len - w, "; ");
            w += snprintf(buf + w, buf_len - w,
                "%s '%s' %s val=%d %s",
                r->id, r->name, r->enabled ? "ON" : "OFF",
                (int)r->last_reading, r->fired ? "FIRED" : "idle");
        }
        if (w == 0) snprintf(buf, buf_len, "No rules");
        return true;
    }
    if (strcmp(cmd, "memory") == 0) {
        int len = readFile("/memory.txt", buf, buf_len);
        if (len <= 0) snprintf(buf, buf_len, "(no memory file)");
        return true;
    }
    if (strcmp(cmd, "time") == 0) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 0)) {
            snprintf(buf, buf_len,
                "%04d-%02d-%02d %02d:%02d:%02d (TZ=%s)",
                timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                cfg_timezone);
        } else {
            snprintf(buf, buf_len, "NTP not synced yet");
        }
        return true;
    }
    if (strcmp(cmd, "history") == 0) {
        if (historyCount == 0) {
            snprintf(buf, buf_len, "No conversation history");
            return true;
        }
        int w = snprintf(buf, buf_len, "History: %d turns. ", historyCount);
        for (int i = 0; i < historyCount && w < buf_len - 60; i++) {
            w += snprintf(buf + w, buf_len - w, "[%d] %.40s%s ",
                i + 1, history[i].user,
                strlen(history[i].user) > 40 ? "..." : "");
        }
        return true;
    }
    if (strcmp(cmd, "help") == 0) {
        snprintf(buf, buf_len,
            "Commands: /status /clear /heap /debug /devices /rules "
            "/memory /time /history /reboot /help");
        return true;
    }
    if (strcmp(cmd, "reboot") == 0) {
        if (g_nats_connected) {
            natsClient.publish(natsSubjectEvents, "Rebooting...");
        }
        delay(500);
        ESP.restart();
        return true; /* never reached */
    }
    return false;
}

/**
 * NATS command handler.
 */
static void onNatsCmd(nats_client_t *client, const nats_msg_t *msg,
                      void *userdata) {
    (void)client; (void)userdata;
    if (msg->data_len == 0) return;

    static char cmdBuf[64];
    size_t len = msg->data_len < sizeof(cmdBuf) - 1
                 ? msg->data_len : sizeof(cmdBuf) - 1;
    memcpy(cmdBuf, msg->data, len);
    cmdBuf[len] = '\0';

    Serial.printf("\n[NATS] cmd: %s\n", cmdBuf);

    if (!handleCommand(cmdBuf, cmdResponseBuf, sizeof(cmdResponseBuf))) {
        snprintf(cmdResponseBuf, sizeof(cmdResponseBuf),
                 "Unknown command: %s (try /help)", cmdBuf);
    }

    Serial.printf("[NATS] -> %s\n> ", cmdResponseBuf);

    if (msg->reply_len > 0) {
        nats_msg_respond_str(natsClient.core(), msg, cmdResponseBuf);
    }
    if (g_nats_connected) {
        natsClient.publish(natsSubjectEvents, cmdResponseBuf);
    }
}

/*============================================================================
 * NATS Virtual Sensor Subscriptions
 *============================================================================*/

static void onNatsValue(nats_client_t *client, const nats_msg_t *msg,
                        void *userdata) {
    (void)client;
    Device *dev = (Device *)userdata;
    if (!dev || !dev->used) return;
    parseNatsPayload(msg->data, msg->data_len,
                     &dev->nats_value, dev->nats_msg, sizeof(dev->nats_msg));
    if (g_debug) Serial.printf("[NATS] %s = %.1f (msg='%s')\n",
                               dev->name, dev->nats_value, dev->nats_msg);
}

void natsSubscribeDeviceSensors() {
    if (!g_nats_connected) return;
    Device *devs = deviceGetAllMutable();
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!devs[i].used) continue;
        if (devs[i].kind != DEV_SENSOR_NATS_VALUE) continue;
        if (devs[i].nats_subject[0] == '\0') continue;
        if (devs[i].nats_sid != 0) continue; /* already subscribed */
        uint16_t sid = 0;
        nats_err_t err = natsClient.subscribe(devs[i].nats_subject,
                                              onNatsValue, &devs[i], &sid);
        if (err == NATS_OK) {
            devs[i].nats_sid = sid;
            Serial.printf("[NATS] Subscribed '%s' -> %s (sid=%d)\n",
                          devs[i].name, devs[i].nats_subject, sid);
        } else {
            Serial.printf("[NATS] Subscribe '%s' failed: %s\n",
                          devs[i].nats_subject, nats_err_str(err));
        }
    }
}

void natsUnsubscribeDevice(const char *name) {
    if (!g_nats_connected) return;
    Device *devs = deviceGetAllMutable();
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!devs[i].used) continue;
        if (strcmp(devs[i].name, name) != 0) continue;
        if (devs[i].nats_sid != 0) {
            natsClient.unsubscribe(devs[i].nats_sid);
            Serial.printf("[NATS] Unsubscribed '%s' (sid=%d)\n",
                          name, devs[i].nats_sid);
            devs[i].nats_sid = 0;
        }
        break;
    }
}

/**
 * Build NATS subject strings from device_name prefix.
 */
static void buildNatsSubjects() {
    snprintf(natsSubjectChat, sizeof(natsSubjectChat),
             "%s.chat", cfg_device_name);
    snprintf(natsSubjectCmd, sizeof(natsSubjectCmd),
             "%s.cmd", cfg_device_name);
    snprintf(natsSubjectEvents, sizeof(natsSubjectEvents),
             "%s.events", cfg_device_name);
}

/**
 * Connect to NATS server and subscribe to topics.
 */
static bool connectNats() {
    Serial.printf("NATS: connecting to %s:%d...\n", cfg_nats_host, cfg_nats_port);

    natsClient.onEvent(onNatsEvent, nullptr);

    if (!natsClient.connect(cfg_nats_host, (uint16_t)cfg_nats_port, 5000)) {
        Serial.printf("NATS: connection failed\n");
        return false;
    }

    /* Subscribe to chat and cmd */
    nats_err_t err;
    err = natsClient.subscribe(natsSubjectChat, onNatsChat, nullptr);
    if (err != NATS_OK) {
        Serial.printf("NATS: subscribe %s failed: %s\n",
                      natsSubjectChat, nats_err_str(err));
    }

    err = natsClient.subscribe(natsSubjectCmd, onNatsCmd, nullptr);
    if (err != NATS_OK) {
        Serial.printf("NATS: subscribe %s failed: %s\n",
                      natsSubjectCmd, nats_err_str(err));
    }

    /* Publish online event */
    static char onlineMsg[128];
    snprintf(onlineMsg, sizeof(onlineMsg),
             "{\"event\":\"online\",\"device\":\"%s\",\"ip\":\"%s\"}",
             cfg_device_name, WiFi.localIP().toString().c_str());
    natsClient.publish(natsSubjectEvents, onlineMsg);

    Serial.printf("NATS: subscribed to %s, %s\n", natsSubjectChat, natsSubjectCmd);

    /* Subscribe NATS virtual sensors */
    natsSubscribeDeviceSensors();

    return true;
}

/*============================================================================
 * Telegram Bot
 *============================================================================*/

static WiFiClientSecure tgClient;
bool g_telegram_enabled = false;
static int  tgLastUpdateId = 0;
static unsigned long tgLastPoll = 0;

/* Long-poll state machine */
enum TgState { TG_IDLE, TG_WAITING };
static TgState tgState = TG_IDLE;
static unsigned long tgWaitStart = 0;
#define TG_RECONNECT_MS   5000   /* 5s between long-poll cycles */
#define TG_LONG_POLL_S    30     /* Telegram server hold time (seconds) */
#define TG_WAIT_TIMEOUT   35000  /* client-side timeout: long-poll + 5s grace */

static const char *TG_HOST = "api.telegram.org";
static const int   TG_PORT = 443;

/**
 * Make an HTTPS request to Telegram Bot API.
 * Writes response body into buf, returns body length or -1 on error.
 */
static int tgApiCall(const char *method, const char *body, int body_len,
                     char *buf, int buf_len) {
    tgClient.stop(); /* Ensure clean state from any previous connection */

    if (!tgClient.connect(TG_HOST, TG_PORT)) {
        if (g_debug) Serial.printf("[TG] Connect failed\n");
        return -1;
    }

    /* Build full request in one buffer to send at once */
    static char httpReq[512];
    int hdr_len = snprintf(httpReq, sizeof(httpReq),
        "POST /bot%s/%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        cfg_telegram_token, method, TG_HOST, body_len);

    tgClient.write((uint8_t *)httpReq, hdr_len);
    if (body_len > 0) {
        tgClient.write((uint8_t *)body, body_len);
    }

    /* Wait for response */
    unsigned long wait_start = millis();
    while (!tgClient.available()) {
        if (!tgClient.connected()) {
            Serial.printf("[TG] Disconnected while waiting\n");
            tgClient.stop();
            return -1;
        }
        if (millis() - wait_start > 15000) {
            Serial.printf("[TG] Response timeout\n");
            tgClient.stop();
            return -1;
        }
        delay(100);
    }

    /* Read status line and headers, extract Content-Length */
    int content_length = -1;
    while (tgClient.connected()) {
        String line = tgClient.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;
        if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
            content_length = line.substring(15).toInt();
        }
    }

    /* Read body - use Content-Length to avoid SSL error on server close */
    int total = 0;
    if (content_length > 0) {
        int to_read = content_length < buf_len - 1 ? content_length : buf_len - 1;
        total = tgClient.readBytes(buf, to_read);
    } else {
        /* Fallback: read until disconnect */
        unsigned long last_data = millis();
        while (total < buf_len - 1) {
            int avail = tgClient.available();
            if (avail > 0) {
                int rd = tgClient.readBytes(buf + total, min(avail, buf_len - 1 - total));
                total += rd;
                last_data = millis();
            } else if (!tgClient.connected()) {
                break;
            } else if (millis() - last_data > 5000) {
                break;
            } else {
                delay(10);
            }
        }
    }

    tgClient.stop();
    buf[total] = '\0';
    if (g_debug) Serial.printf("[TG] %s: %d bytes\n", method, total);
    return total;
}

/**
 * Send a message to the allowed chat.
 */
bool tgSendMessage(const char *text) {
    static char req[LLM_MAX_RESPONSE_LEN + 256];
    static char escaped[LLM_MAX_RESPONSE_LEN + 128];

    /* Escape the text for JSON */
    int w = 0;
    for (int i = 0; text[i] && w < (int)sizeof(escaped) - 2; i++) {
        char c = text[i];
        if (c == '"' || c == '\\') {
            escaped[w++] = '\\';
            escaped[w++] = c;
        } else if (c == '\n') {
            escaped[w++] = '\\';
            escaped[w++] = 'n';
        } else if ((uint8_t)c >= 0x20) {
            escaped[w++] = c;
        }
    }
    escaped[w] = '\0';

    int req_len = snprintf(req, sizeof(req),
        "{\"chat_id\":%s,\"text\":\"%s\"}", cfg_telegram_chat_id, escaped);

    static char resp[256];
    int rlen = tgApiCall("sendMessage", req, req_len, resp, sizeof(resp));
    if (rlen < 0) {
        Serial.printf("[TG] sendMessage failed\n");
        return false;
    }
    return true;
}

/**
 * Non-blocking Telegram long-poll state machine.
 * Called from loop() every iteration. Keeps the connection open for up to
 * TG_LONG_POLL_S seconds — Telegram's server holds the response until an
 * update arrives or the timeout expires. This reduces TLS handshakes from
 * ~360/hour to ~120/hour and provides near-instant message delivery.
 */
static void telegramTick() {
    unsigned long now = millis();

    switch (tgState) {

    case TG_IDLE: {
        if (now - tgLastPoll < TG_RECONNECT_MS) return;

        tgClient.stop();
        if (!tgClient.connect(TG_HOST, TG_PORT)) {
            if (g_debug) Serial.printf("[TG] Connect failed\n");
            tgLastPoll = now;
            return;
        }

        /* Build getUpdates request with long-poll timeout */
        static char body[128];
        int body_len = snprintf(body, sizeof(body),
            "{\"offset\":%d,\"limit\":1,\"timeout\":%d}",
            tgLastUpdateId + 1, TG_LONG_POLL_S);

        static char httpReq[512];
        int hdr_len = snprintf(httpReq, sizeof(httpReq),
            "POST /bot%s/getUpdates HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            cfg_telegram_token, TG_HOST, body_len);

        tgClient.write((uint8_t *)httpReq, hdr_len);
        tgClient.write((uint8_t *)body, body_len);

        tgState = TG_WAITING;
        tgWaitStart = now;
        if (g_debug) Serial.printf("[TG] Long poll started\n");
        return;
    }

    case TG_WAITING: {
        if (!tgClient.available()) {
            /* Still waiting — check for errors or timeout */
            if (!tgClient.connected()) {
                if (g_debug) Serial.printf("[TG] Disconnected during wait\n");
                tgClient.stop();
                tgState = TG_IDLE;
                tgLastPoll = now;
                return;
            }
            if (now - tgWaitStart > TG_WAIT_TIMEOUT) {
                if (g_debug) Serial.printf("[TG] Long poll timeout\n");
                tgClient.stop();
                tgState = TG_IDLE;
                tgLastPoll = now;
                return;
            }
            return; /* Still waiting — return to loop() */
        }

        /* Data arrived — read response (brief blocking OK, data is TCP-buffered) */
        static char resp[2048];

        /* Read headers */
        int content_length = -1;
        while (tgClient.connected()) {
            String line = tgClient.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) break;
            if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
                content_length = line.substring(15).toInt();
            }
        }

        /* Read body */
        int total = 0;
        if (content_length > 0) {
            int to_read = content_length < (int)sizeof(resp) - 1 ? content_length : (int)sizeof(resp) - 1;
            total = tgClient.readBytes(resp, to_read);
        } else {
            unsigned long last_data = millis();
            while (total < (int)sizeof(resp) - 1) {
                int avail = tgClient.available();
                if (avail > 0) {
                    int rd = tgClient.readBytes(resp + total, min(avail, (int)sizeof(resp) - 1 - total));
                    total += rd;
                    last_data = millis();
                } else if (!tgClient.connected()) {
                    break;
                } else if (millis() - last_data > 5000) {
                    break;
                } else {
                    delay(10);
                }
            }
        }

        tgClient.stop();
        resp[total] = '\0';
        tgState = TG_IDLE;
        tgLastPoll = millis();

        if (g_debug) Serial.printf("[TG] poll: %d bytes\n", total);
        if (total <= 0) return;
        if (g_debug) Serial.printf("[TG] poll: %.200s\n", resp);

        /* Quick check: is there a result with "update_id"? */
        const char *uid_str = strstr(resp, "\"update_id\"");
        if (!uid_str) return; /* No updates - normal */

        /* Parse update_id */
        const char *p = uid_str + 11;
        while (*p == ':' || *p == ' ') p++;
        int update_id = atoi(p);
        if (g_debug) Serial.printf("[TG] update_id=%d (last=%d)\n", update_id, tgLastUpdateId);
        if (update_id <= tgLastUpdateId) return;
        tgLastUpdateId = update_id;

        /* Extract chat_id from message.chat.id */
        const char *chat_id_str = strstr(resp, "\"chat\"");
        if (!chat_id_str) { if (g_debug) Serial.printf("[TG] no chat field\n"); return; }
        const char *id_str = strstr(chat_id_str, "\"id\"");
        if (!id_str) { if (g_debug) Serial.printf("[TG] no id in chat\n"); return; }
        p = id_str + 4;
        while (*p == ':' || *p == ' ') p++;
        char incoming_chat_id[16];
        int cw = 0;
        while ((*p >= '0' && *p <= '9') || *p == '-') {
            if (cw < (int)sizeof(incoming_chat_id) - 1)
                incoming_chat_id[cw++] = *p;
            p++;
        }
        incoming_chat_id[cw] = '\0';

        if (g_debug) Serial.printf("[TG] chat_id=%s (allowed=%s)\n", incoming_chat_id, cfg_telegram_chat_id);

        /* Security: only allow configured chat_id */
        if (strcmp(incoming_chat_id, cfg_telegram_chat_id) != 0) {
            Serial.printf("[TG] Rejected chat %s\n", incoming_chat_id);
            return;
        }

        /* Extract message text - find "text":"..." */
        const char *text_key = strstr(resp, "\"text\"");
        if (!text_key) { if (g_debug) Serial.printf("[TG] no text field\n"); return; }
        p = text_key + 6;
        while (*p == ':' || *p == ' ') p++;
        if (*p != '"') { if (g_debug) Serial.printf("[TG] text not a string\n"); return; }
        p++;

        static char msgBuf[512];
        int mw = 0;
        while (*p && *p != '"' && mw < (int)sizeof(msgBuf) - 1) {
            if (*p == '\\' && *(p + 1)) {
                p++;
                if (*p == 'n') msgBuf[mw++] = '\n';
                else msgBuf[mw++] = *p;
            } else {
                msgBuf[mw++] = *p;
            }
            p++;
        }
        msgBuf[mw] = '\0';

        if (mw == 0) { if (g_debug) Serial.printf("[TG] empty text\n"); return; }

        Serial.printf("\n[TG] Message from %s: %s\n", incoming_chat_id, msgBuf);

        /* Slash command? Execute locally, no LLM call */
        if (msgBuf[0] == '/') {
            const char *cmd = msgBuf + 1;
            /* Strip @botname suffix (Telegram sends "/status@MyBot" in groups) */
            static char cmdCopy[64];
            strncpy(cmdCopy, cmd, sizeof(cmdCopy) - 1);
            cmdCopy[sizeof(cmdCopy) - 1] = '\0';
            char *at = strchr(cmdCopy, '@');
            if (at) *at = '\0';

            if (handleCommand(cmdCopy, cmdResponseBuf, sizeof(cmdResponseBuf))) {
                Serial.printf("[TG] cmd: /%s -> %s\n", cmdCopy, cmdResponseBuf);
                tgSendMessage(cmdResponseBuf);
            } else {
                snprintf(cmdResponseBuf, sizeof(cmdResponseBuf),
                         "Unknown command: /%s (try /help)", cmdCopy);
                tgSendMessage(cmdResponseBuf);
            }
            Serial.printf("> ");
            return;
        }

        /* Run chat */
        const char *response = chatWithLLM(msgBuf);

        /* Send response back to Telegram */
        if (response) {
            tgSendMessage(response);
        } else {
            tgSendMessage("[error: LLM call failed]");
        }

        Serial.printf("> ");
        return;
    }
    }
}

/*============================================================================
 * Serial Commands
 *============================================================================*/

void handleSerialCommand(const char *input) {
    if (strcmp(input, "/status") == 0) {
        Serial.printf("--- esp-claw status ---\n");
        Serial.printf("WiFi: %s (%s)\n",
                      WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
                      WiFi.localIP().toString().c_str());
        Serial.printf("Heap: %u free / %u total\n",
                      ESP.getFreeHeap(), ESP.getHeapSize());
        Serial.printf("History: %d turns\n", historyCount);
        Serial.printf("Model: %s\n", cfg_model);
        Serial.printf("Debug: %s\n", g_debug ? "ON" : "OFF");
        Serial.printf("NATS: %s\n", g_nats_enabled
            ? (g_nats_connected ? "connected" : "disconnected")
            : "disabled");
        Serial.printf("Telegram: %s\n", g_telegram_enabled ? "enabled" : "disabled");
        Serial.printf("Uptime: %lus\n", millis() / 1000);
        Serial.printf("> ");
        return;
    }

    if (strcmp(input, "/clear") == 0) {
        historyCount = 0;
        LittleFS.remove(HISTORY_FILE);
        Serial.printf("Conversation history cleared.\n> ");
        return;
    }

    if (strcmp(input, "/heap") == 0) {
        Serial.printf("Free heap: %u bytes\n> ", ESP.getFreeHeap());
        return;
    }

    if (strcmp(input, "/reboot") == 0) {
        Serial.printf("Rebooting...\n");
        delay(500);
        ESP.restart();
        return;
    }

    if (strcmp(input, "/config") == 0) {
        Serial.printf("--- config ---\n");
        Serial.printf("WiFi SSID: %s\n", cfg_wifi_ssid);
        Serial.printf("API key:   %.8s...\n", cfg_api_key);
        Serial.printf("Model:     %s\n", cfg_model);
        Serial.printf("Device:    %s\n", cfg_device_name);
        Serial.printf("NATS:      %s:%d (%s)\n", cfg_nats_host, cfg_nats_port,
                      g_nats_enabled ? "enabled" : "disabled");
        Serial.printf("Telegram:  %s\n", g_telegram_enabled ? "enabled" : "disabled");
        Serial.printf("Prompt:    %d chars\n", (int)strlen(cfg_system_prompt));
        Serial.printf("> ");
        return;
    }

    if (strcmp(input, "/prompt") == 0) {
        Serial.printf("--- system prompt ---\n%s\n---\n> ", cfg_system_prompt);
        return;
    }

    if (strcmp(input, "/history") == 0 || strcmp(input, "/history full") == 0) {
        bool full = (strcmp(input, "/history full") == 0);
        if (historyCount == 0) {
            Serial.printf("No conversation history.\n> ");
            return;
        }
        Serial.printf("--- history (%d turns) ---\n", historyCount);
        for (int i = 0; i < historyCount; i++) {
            Serial.printf("[%d] User: %s\n", i + 1, history[i].user);
            if (full) {
                Serial.printf("[%d] Assistant: %s\n\n", i + 1, history[i].assistant);
            } else {
                int len = strlen(history[i].assistant);
                if (len > 200) {
                    Serial.printf("[%d] Assistant: %.200s... (%d chars)\n\n",
                                  i + 1, history[i].assistant, len);
                } else {
                    Serial.printf("[%d] Assistant: %s\n\n", i + 1, history[i].assistant);
                }
            }
        }
        Serial.printf("---\n> ");
        return;
    }

    if (strcmp(input, "/memory") == 0) {
        static char memBuf[512];
        int len = readFile("/memory.txt", memBuf, sizeof(memBuf));
        if (len > 0) {
            Serial.printf("--- memory (%d bytes) ---\n%s\n---\n> ", len, memBuf);
        } else {
            Serial.printf("(no memory file)\n> ");
        }
        return;
    }

    if (strcmp(input, "/time") == 0) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 0)) {
            Serial.printf("%04d-%02d-%02d %02d:%02d:%02d (TZ=%s)\n> ",
                          timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                          cfg_timezone);
        } else {
            Serial.printf("NTP not synced yet\n> ");
        }
        return;
    }

    if (strcmp(input, "/debug") == 0) {
        g_debug = !g_debug;
        Serial.printf("Debug %s\n> ", g_debug ? "ON" : "OFF");
        return;
    }

    if (strcmp(input, "/devices") == 0) {
        const Device *devs = deviceGetAll();
        int count = 0;
        Serial.printf("--- devices ---\n");
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (!devs[i].used) continue;
            const Device *d = &devs[i];
            if (d->kind == DEV_SENSOR_SERIAL_TEXT) {
                float val = deviceReadSensor(d);
                Serial.printf("  %s [serial_text] %ubaud  = %.1f %s  msg='%s'\n",
                              d->name, (unsigned)d->baud, val, d->unit,
                              serialTextGetMsg());
            } else if (d->kind == DEV_SENSOR_NATS_VALUE) {
                float val = deviceReadSensor(d);
                Serial.printf("  %s [nats_value] nats=%s  = %.1f %s\n",
                              d->name, d->nats_subject, val, d->unit);
            } else if (deviceIsSensor(d->kind)) {
                float val = deviceReadSensor(d);
                Serial.printf("  %s [%s] pin=%d  = %.1f %s\n",
                              d->name, deviceKindName(d->kind), d->pin, val, d->unit);
            } else {
                Serial.printf("  %s [%s] pin=%d%s\n",
                              d->name, deviceKindName(d->kind), d->pin,
                              d->inverted ? " (inverted)" : "");
            }
            count++;
        }
        if (count == 0) Serial.printf("  (none)\n");
        Serial.printf("---\n> ");
        return;
    }

    if (strcmp(input, "/rules") == 0) {
        const Rule *rules = ruleGetAll();
        int count = 0;
        Serial.printf("--- rules ---\n");
        for (int i = 0; i < MAX_RULES; i++) {
            if (!rules[i].used) continue;
            const Rule *r = &rules[i];
            Serial.printf("  %s '%s' [%s] %s %s %d val=%d %s\n",
                          r->id, r->name,
                          r->enabled ? "ON" : "OFF",
                          r->sensor_name[0] ? r->sensor_name : "gpio",
                          conditionOpName(r->condition),
                          (int)r->threshold,
                          (int)r->last_reading,
                          r->fired ? "FIRED" : "idle");
            Serial.printf("    on:  %s", actionTypeName(r->on_action));
            if (r->on_action == ACT_LED_SET) {
                int32_t v = r->on_value;
                Serial.printf("(%d,%d,%d)", (v>>16)&0xFF, (v>>8)&0xFF, v&0xFF);
            } else if (r->on_action == ACT_TELEGRAM || r->on_action == ACT_NATS_PUBLISH
                       || r->on_action == ACT_SERIAL_SEND) {
                Serial.printf(" \"%s\"", r->on_nats_pay);
            } else if (r->on_action == ACT_ACTUATOR) {
                Serial.printf(" %s", r->on_actuator);
            } else if (r->on_action == ACT_GPIO_WRITE) {
                Serial.printf(" pin=%d val=%d", r->on_pin, (int)r->on_value);
            }
            Serial.printf("\n");
            if (r->has_off_action) {
                Serial.printf("    off: %s", actionTypeName(r->off_action));
                if (r->off_action == ACT_LED_SET) {
                    int32_t v = r->off_value;
                    Serial.printf("(%d,%d,%d)", (v>>16)&0xFF, (v>>8)&0xFF, v&0xFF);
                } else if (r->off_action == ACT_TELEGRAM || r->off_action == ACT_NATS_PUBLISH
                           || r->off_action == ACT_SERIAL_SEND) {
                    Serial.printf(" \"%s\"", r->off_nats_pay);
                } else if (r->off_action == ACT_ACTUATOR) {
                    Serial.printf(" %s", r->off_actuator);
                } else if (r->off_action == ACT_GPIO_WRITE) {
                    Serial.printf(" pin=%d val=%d", r->off_pin, (int)r->off_value);
                }
                Serial.printf("\n");
            }
            count++;
        }
        if (count == 0) Serial.printf("  (none)\n");
        Serial.printf("---\n> ");
        return;
    }

    if (strcmp(input, "/setup") == 0) {
        Serial.printf("Starting setup portal...\n");
        runSetupPortal(); /* blocks until config saved + reboot */
        return;
    }

    if (strcmp(input, "/help") == 0) {
        Serial.printf("--- esp-claw commands ---\n");
        Serial.printf("  /status  - Show device status\n");
        Serial.printf("  /config  - Show loaded config (serial only)\n");
        Serial.printf("  /prompt  - Show system prompt (serial only)\n");
        Serial.printf("  /history - Show conversation history (add 'full' for complete text)\n");
        Serial.printf("  /clear   - Clear conversation history\n");
        Serial.printf("  /devices - List registered devices with readings\n");
        Serial.printf("  /rules   - List automation rules with status\n");
        Serial.printf("  /memory  - Show AI persistent memory\n");
        Serial.printf("  /time    - Show current time and timezone\n");
        Serial.printf("  /heap    - Show free memory\n");
        Serial.printf("  /debug   - Toggle debug output\n");
        Serial.printf("  /setup   - Start WiFi setup portal (serial only)\n");
        Serial.printf("  /reboot  - Restart ESP32\n");
        Serial.printf("  /help    - This help\n");
        Serial.printf("  (anything else) - Chat with AI\n");
        Serial.printf("  Commands also work via Telegram and NATS.\n");
        Serial.printf("> ");
        return;
    }

    /* Unknown command - treat as chat */
    chatWithLLM(input);
    Serial.printf("> ");
}

/*============================================================================
 * Setup & Loop
 *============================================================================*/

void setup() {
    Serial.begin(115200);
    delay(5000);

    Serial.printf("\n\n");
    Serial.printf("========================================\n");
    Serial.printf("  esp-claw - ESP32 AI Agent v0.7.0\n");
    Serial.printf("========================================\n\n");

    /* Load config from LittleFS */
    loadConfig();
    historyLoad();
    Serial.printf("Model: %s\n", cfg_model);

    /* Initialize temperature sensor (not available on classic ESP32) */
#if !defined(CONFIG_IDF_TARGET_ESP32)
    initTempSensor();
    if (g_temp_sensor) {
        float temp = 0.0f;
        temperature_sensor_get_celsius(g_temp_sensor, &temp);
        Serial.printf("Chip temp: %.1f C\n", temp);
    }
#endif

    /* Initialize device registry and rule engine */
    devicesInit();
    rulesInit();

    if (cfg_wifi_ssid[0] == '\0') {
        Serial.printf("\n[!] No WiFi config — starting setup portal\n");
        runSetupPortal(); /* blocks until config saved + reboot */
    }

    /* Connect WiFi */
    if (!connectWiFi()) {
        Serial.printf("[!] WiFi failed — starting setup portal\n");
        runSetupPortal(); /* blocks until config saved + reboot */
    }

    /* NTP time sync */
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", cfg_timezone, 1);
    tzset();
    Serial.printf("NTP: syncing (TZ=%s)...\n", cfg_timezone);

    /* Init LLM client */
    llm.begin(cfg_api_key, cfg_model, cfg_api_base_url);

    /* Watchdog - reconfigure to 60s (Arduino already inits WDT at 5s) */
    esp_task_wdt_config_t wdt_cfg = { .timeout_ms = 60000, .idle_core_mask = 0,
                                       .trigger_panic = true };
    esp_task_wdt_reconfigure(&wdt_cfg);
    esp_task_wdt_add(NULL); /* Add loop task */

    /* Connect NATS (optional) */
    if (cfg_nats_host[0] != '\0') {
        g_nats_enabled = true;
        buildNatsSubjects();
        if (!connectNats()) {
            Serial.printf("NATS: will retry in background\n");
        }
    } else {
        Serial.printf("NATS: disabled (no nats_host in config)\n");
    }

    /* Telegram (optional) */
    if (cfg_telegram_token[0] != '\0' && cfg_telegram_chat_id[0] != '\0') {
        g_telegram_enabled = true;
        tgClient.setInsecure();
        tgClient.setTimeout(30); /* seconds - matches LLM client pattern */
        tgLastPoll = millis();   /* delay first poll by one interval */
        Serial.printf("Telegram: enabled (chat_id %s)\n", cfg_telegram_chat_id);
    } else {
        Serial.printf("Telegram: disabled (no telegram_token/telegram_chat_id in config)\n");
    }

    Serial.printf("\nReady! Free heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("Type a message and press Enter. /help for commands.\n\n");
    Serial.printf("> ");
}

/* LED heartbeat state */
static unsigned long lastHeartbeat = 0;
#define HEARTBEAT_INTERVAL_MS 3000

void loop() {
    esp_task_wdt_reset(); /* Feed the watchdog */

    /* LED heartbeat - brief dim green blink when idle */
    if (!g_led_user) {
        unsigned long now = millis();
        if (now - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
            lastHeartbeat = now;
            led(0, 40, 0); /* dim green */
            delay(50);
            ledOff();
        }
    }

    /* Check WiFi */
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("\nWiFi disconnected! Reconnecting...\n");
        ledRed();
        if (!connectWiFi()) {
            delay(5000);
            return;
        }
        Serial.printf("> ");
    }

    /* Process NATS */
    if (g_nats_enabled) {
        if (natsClient.connected()) {
            nats_err_t err = natsClient.process();
            if (err != NATS_OK && err != NATS_ERR_WOULD_BLOCK) {
                if (g_debug) Serial.printf("NATS: process error: %s\n",
                                           nats_err_str(err));
            }
        } else {
            /* Reconnect with backoff */
            unsigned long now = millis();
            if (now - natsLastReconnect > NATS_RECONNECT_DELAY_MS) {
                natsLastReconnect = now;
                connectNats();
            }
        }
    }

    /* Poll Telegram */
    if (g_telegram_enabled) {
        telegramTick();
    }

    /* Evaluate automation rules */
    rulesEvaluate();

    /* Poll serial_text UART for incoming data */
    serialTextPoll();

    /* Read serial input character by character */
    while (Serial.available()) {
        char c = Serial.read();

        /* Handle backspace */
        if (c == '\b' || c == 127) {
            if (serialPos > 0) {
                serialPos--;
                Serial.print("\b \b");
            }
            continue;
        }

        /* Handle enter */
        if (c == '\n' || c == '\r') {
            if (serialPos == 0) continue; /* Ignore empty lines */

            serialBuf[serialPos] = '\0';
            serialPos = 0;
            Serial.println(); /* Echo newline */

            /* Trim whitespace */
            char *input = serialBuf;
            while (*input == ' ') input++;
            int len = strlen(input);
            while (len > 0 && input[len - 1] == ' ') input[--len] = '\0';

            if (len == 0) {
                Serial.printf("> ");
                continue;
            }

            /* Process input */
            if (input[0] == '/') {
                handleSerialCommand(input);
            } else {
                chatWithLLM(input);
                Serial.printf("> ");
            }
            continue;
        }

        /* Buffer character */
        if (serialPos < SERIAL_BUF_SIZE - 1) {
            serialBuf[serialPos++] = c;
            Serial.print(c); /* Echo */
        }
    }

    delay(10); /* Yield */
}
