/**
 * @file main.cpp
 * @brief esp-claw - ESP32 AI Agent
 *
 * A reimplementation of PicoClaw's core agent loop for ESP32.
 * Phase 1: Serial chat with OpenRouter LLM API.
 *
 * Type a message in the serial monitor, get an LLM response.
 * Use 115200 baud, send with newline.
 */

#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"
#include "llm_client.h"

/*============================================================================
 * Configuration
 *============================================================================*/

#define LED_BRIGHTNESS   20
#define SERIAL_BUF_SIZE  512
#define MAX_HISTORY      6  /* Keep last N user+assistant turns (pairs) */

static const char *SYSTEM_PROMPT =
    "You are esp-claw, a helpful AI assistant running on an ESP32 microcontroller. "
    "You are concise because your responses are transmitted over a slow serial link. "
    "Keep responses under 200 words unless asked for detail. "
    "You have access to GPIO pins, LEDs, and sensors on the ESP32 board. "
    "You are connected via WiFi and can communicate over NATS messaging. "
    "Be friendly, practical, and embedded-systems-aware.";

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
 * Globals
 *============================================================================*/

bool g_debug = false;

LlmClient llm;
char serialBuf[SERIAL_BUF_SIZE];
int  serialPos = 0;

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
    Serial.printf("WiFi: Connecting to %s", WIFI_SSID);
    ledOrange();

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

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
 * Chat with LLM
 *============================================================================*/

void chatWithLLM(const char *userMessage) {
    ledBlue(); /* Thinking... */

    /* Build messages array: system + history + current user message */
    LlmMessage messages[1 + MAX_HISTORY * 2 + 1]; /* system + pairs + current */
    int msgCount = 0;

    /* System prompt */
    messages[msgCount++] = {"system", SYSTEM_PROMPT};

    /* History */
    for (int i = 0; i < historyCount; i++) {
        messages[msgCount++] = {"user", history[i].user};
        messages[msgCount++] = {"assistant", history[i].assistant};
    }

    /* Current user message */
    messages[msgCount++] = {"user", userMessage};

    Serial.printf("\n--- Thinking... ---\n");
    unsigned long t0 = millis();

    LlmResult result;
    bool ok = llm.chat(messages, msgCount, &result);

    unsigned long elapsed = millis() - t0;

    if (ok) {
        ledGreen();

        /* Print response */
        Serial.printf("\n%s\n", result.content);
        Serial.printf("--- (%lums, %d+%d tokens) ---\n\n",
                      elapsed,
                      result.prompt_tokens,
                      result.completion_tokens);

        /* Save to history (circular buffer) */
        int slot = historyCount < MAX_HISTORY ? historyCount : 0;
        if (historyCount >= MAX_HISTORY) {
            /* Shift history down by 1 to drop oldest */
            for (int i = 0; i < MAX_HISTORY - 1; i++) {
                history[i] = history[i + 1];
            }
            slot = MAX_HISTORY - 1;
        } else {
            historyCount++;
        }
        strncpy(history[slot].user, userMessage, sizeof(history[slot].user) - 1);
        history[slot].user[sizeof(history[slot].user) - 1] = '\0';
        strncpy(history[slot].assistant, result.content,
                sizeof(history[slot].assistant) - 1);
        history[slot].assistant[sizeof(history[slot].assistant) - 1] = '\0';
        history[slot].used = true;

    } else {
        ledRed();
        Serial.printf("\n[ERROR] LLM call failed: %s\n\n", llm.lastError());
    }

    Serial.printf("> "); /* Prompt for next input */
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
        Serial.printf("Model: %s\n", OPENROUTER_MODEL);
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

    if (strcmp(input, "/debug") == 0) {
        g_debug = !g_debug;
        Serial.printf("Debug %s\n> ", g_debug ? "ON" : "OFF");
        return;
    }

    if (strcmp(input, "/help") == 0) {
        Serial.printf("--- esp-claw commands ---\n");
        Serial.printf("  /status  - Show device status\n");
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
}

/*============================================================================
 * Setup & Loop
 *============================================================================*/

void setup() {
    Serial.begin(115200);
    delay(5000);

    Serial.printf("\n\n");
    Serial.printf("========================================\n");
    Serial.printf("  esp-claw - ESP32 AI Agent v0.1.0\n");
    Serial.printf("  Model: %s\n", OPENROUTER_MODEL);
    Serial.printf("========================================\n\n");

    /* Connect WiFi */
    if (!connectWiFi()) {
        Serial.printf("WiFi failed. Rebooting in 10s...\n");
        delay(10000);
        ESP.restart();
    }

    /* Init LLM client */
    llm.begin(OPENROUTER_API_KEY, OPENROUTER_MODEL);

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
