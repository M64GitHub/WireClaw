/**
 * @file main.cpp
 * @brief esp-claw - ESP32 AI Agent
 *
 * A reimplementation of PicoClaw's core agent loop for ESP32.
 * Phase 4: NATS integration for remote control + tool calling.
 *
 * Type a message in the serial monitor, get an LLM response.
 * Use 115200 baud, send with newline.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include "llm_client.h"
#include "tools.h"
#include <nats_atoms.h>

/*============================================================================
 * Configuration
 *============================================================================*/

#define LED_BRIGHTNESS   20
#define SERIAL_BUF_SIZE  512
#define MAX_HISTORY      6  /* Keep last N user+assistant turns (pairs) */

/* Runtime config — loaded from LittleFS, falls back to secrets.h */
static char cfg_wifi_ssid[64];
static char cfg_wifi_pass[64];
static char cfg_api_key[128];
static char cfg_model[64];
static char cfg_device_name[32];
static char cfg_nats_host[64];
static int  cfg_nats_port = 4222;
static char cfg_system_prompt[4096];

/* Placeholder defaults — overridden by LittleFS config.json */
static void configDefaults() {
    cfg_wifi_ssid[0] = '\0';
    cfg_wifi_pass[0] = '\0';
    cfg_api_key[0] = '\0';
    strncpy(cfg_model, "openai/gpt-4o-mini", sizeof(cfg_model));
    strncpy(cfg_device_name, "esp-claw", sizeof(cfg_device_name));
    cfg_nats_host[0] = '\0';
    cfg_nats_port = 4222;
    strncpy(cfg_system_prompt,
        "You are esp-claw, a helpful AI assistant running on an ESP32 microcontroller. "
        "Be concise. Keep responses under 200 words unless asked for detail.",
        sizeof(cfg_system_prompt));
}

/*============================================================================
 * LED Helpers (WaveShare ESP32-C6 has GRB swap)
 *============================================================================*/

static uint8_t ledBrightness = LED_BRIGHTNESS;

void led(uint8_t r, uint8_t g, uint8_t b) {
    r = (uint8_t)((r * ledBrightness) / 255);
    g = (uint8_t)((g * ledBrightness) / 255);
    b = (uint8_t)((b * ledBrightness) / 255);
#ifdef RGB_BUILTIN
    rgbLedWrite(RGB_BUILTIN, g, r, b);
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
 * Simple JSON string extractor — find "key":"value" and copy value to dst.
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
        jsonGetString(json_buf, "nats_host", cfg_nats_host, sizeof(cfg_nats_host));
        char port_buf[8];
        if (jsonGetString(json_buf, "nats_port", port_buf, sizeof(port_buf))) {
            cfg_nats_port = atoi(port_buf);
        }
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
bool g_led_user = false; /* true when LED was set by a tool — don't overwrite with status */

LlmClient llm;
char serialBuf[SERIAL_BUF_SIZE];
int  serialPos = 0;

/* NATS client (optional — only used if nats_host is configured) */
NatsClient natsClient;
bool g_nats_enabled = false;
bool g_nats_connected = false;
static unsigned long natsLastReconnect = 0;
#define NATS_RECONNECT_DELAY_MS 5000
static char natsSubjectChat[64];
static char natsSubjectCmd[64];
static char natsSubjectEvents[64];

/* Conversation history */
struct Turn {
    char user[256];
    char assistant[LLM_MAX_RESPONSE_LEN];
    bool used;
};

static Turn history[MAX_HISTORY];
static int  historyCount = 0;

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
 * Chat with LLM — Agentic Loop with Tool Calling
 *============================================================================*/

#define MAX_AGENT_ITERATIONS 5

/* Static storage for tool call results (persists across loop iterations) */
static char toolResultBufs[LLM_MAX_TOOL_CALLS][TOOL_RESULT_MAX_LEN];
static char toolCallJsonBuf[1024]; /* copy of tool_calls_json for message building */

/**
 * Run the agentic chat loop. Returns pointer to the response text
 * (valid until next call), or nullptr on error.
 */
const char *chatWithLLM(const char *userMessage) {
    g_led_user = false; /* Reset — status LEDs allowed until a tool sets the LED */
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

    /* History */
    for (int i = 0; i < historyCount && msgCount < LLM_MAX_MESSAGES - 2; i++) {
        messages[msgCount++] = llmMsg("user", history[i].user);
        messages[msgCount++] = llmMsg("assistant", history[i].assistant);
    }

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
        if (!ok) break;

        totalPromptTokens += result.prompt_tokens;
        totalCompletionTokens += result.completion_tokens;

        /* No tool calls — we're done */
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
 * NATS chat handler — request/reply. Caller sends a message,
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

/**
 * NATS command handler — same commands as serial.
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

    static char responseBuf[512];

    if (strcmp(cmdBuf, "status") == 0) {
        snprintf(responseBuf, sizeof(responseBuf),
            "WiFi: %s (%s), Heap: %u/%u, History: %d, Model: %s, "
            "Debug: %s, NATS: %s, Uptime: %lus",
            WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
            WiFi.localIP().toString().c_str(),
            ESP.getFreeHeap(), ESP.getHeapSize(),
            historyCount, cfg_model,
            g_debug ? "ON" : "OFF",
            g_nats_connected ? "connected" : "disconnected",
            millis() / 1000);
    } else if (strcmp(cmdBuf, "clear") == 0) {
        historyCount = 0;
        snprintf(responseBuf, sizeof(responseBuf), "History cleared");
    } else if (strcmp(cmdBuf, "heap") == 0) {
        snprintf(responseBuf, sizeof(responseBuf), "Free heap: %u bytes",
                 ESP.getFreeHeap());
    } else if (strcmp(cmdBuf, "debug") == 0) {
        g_debug = !g_debug;
        snprintf(responseBuf, sizeof(responseBuf), "Debug %s",
                 g_debug ? "ON" : "OFF");
    } else if (strcmp(cmdBuf, "reboot") == 0) {
        if (g_nats_connected) {
            natsClient.publish(natsSubjectEvents, "Rebooting...");
        }
        delay(500);
        ESP.restart();
        return;
    } else {
        snprintf(responseBuf, sizeof(responseBuf),
                 "Unknown command: %s (try: status, clear, heap, debug, reboot)",
                 cmdBuf);
    }

    Serial.printf("[NATS] -> %s\n> ", responseBuf);

    /* Reply if request/reply, also publish to events */
    if (msg->reply_len > 0) {
        nats_msg_respond_str(natsClient.core(), msg, responseBuf);
    }
    if (g_nats_connected) {
        natsClient.publish(natsSubjectEvents, responseBuf);
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
    return true;
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
        Serial.printf("Uptime: %lus\n", millis() / 1000);
        Serial.printf("> ");
        return;
    }

    if (strcmp(input, "/clear") == 0) {
        historyCount = 0;
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
        Serial.printf("Prompt:    %d chars\n", (int)strlen(cfg_system_prompt));
        Serial.printf("> ");
        return;
    }

    if (strcmp(input, "/prompt") == 0) {
        Serial.printf("--- system prompt ---\n%s\n---\n> ", cfg_system_prompt);
        return;
    }

    if (strcmp(input, "/debug") == 0) {
        g_debug = !g_debug;
        Serial.printf("Debug %s\n> ", g_debug ? "ON" : "OFF");
        return;
    }

    if (strcmp(input, "/help") == 0) {
        Serial.printf("--- esp-claw commands ---\n");
        Serial.printf("  /status  - Show device status\n");
        Serial.printf("  /config  - Show loaded config\n");
        Serial.printf("  /prompt  - Show system prompt\n");
        Serial.printf("  /clear   - Clear conversation history\n");
        Serial.printf("  /heap    - Show free memory\n");
        Serial.printf("  /debug   - Toggle debug output\n");
        Serial.printf("  /reboot  - Restart ESP32\n");
        Serial.printf("  /help    - This help\n");
        Serial.printf("  (anything else) - Chat with AI\n");
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
    Serial.printf("  esp-claw - ESP32 AI Agent v0.4.0\n");
    Serial.printf("========================================\n\n");

    /* Load config from LittleFS */
    loadConfig();
    Serial.printf("Model: %s\n", cfg_model);

    if (cfg_wifi_ssid[0] == '\0' || cfg_api_key[0] == '\0') {
        Serial.printf("\n[ERROR] Missing config! Upload filesystem:\n");
        Serial.printf("  pio run -t uploadfs\n");
        Serial.printf("Halted.\n");
        ledRed();
        while (true) delay(1000);
    }

    /* Connect WiFi */
    if (!connectWiFi()) {
        Serial.printf("WiFi failed. Rebooting in 10s...\n");
        delay(10000);
        ESP.restart();
    }

    /* Init LLM client */
    llm.begin(cfg_api_key, cfg_model);

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

    Serial.printf("\nReady! Free heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("Type a message and press Enter. /help for commands.\n\n");
    Serial.printf("> ");
}

void loop() {
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
