/**
 * @file setup_portal.cpp
 * @brief WiFi AP captive portal for initial device configuration
 *
 * Starts an open AP ("WireClaw-Setup"), runs a DNS captive portal that
 * redirects all domains to 192.168.4.1, and serves a config form on port 80.
 * When the user submits, writes /config.json to LittleFS and reboots.
 */

#include "setup_portal.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <WiFiServer.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>

/* LED functions from main.cpp */
extern void led(uint8_t r, uint8_t g, uint8_t b);
extern void ledOff();

#define PORTAL_TIMEOUT_MS 300000 /* 5 minutes */

/*============================================================================
 * HTML Pages (stored in flash)
 *============================================================================*/

static const char SETUP_HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WireClaw Setup</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Courier New',monospace;background:#0a0a0a;color:#e0e0e0;padding:20px;max-width:480px;margin:0 auto}
h1{color:#00d4aa;font-size:1.5em;margin-bottom:4px}
.sub{color:#666;font-size:0.85em;margin-bottom:20px}
label{display:block;margin:12px 0 4px;color:#00d4aa;font-size:0.9em}
input[type=text],input[type=password]{width:100%;padding:10px;background:#1a1a1a;border:1px solid #333;color:#fff;font-family:inherit;font-size:0.95em;border-radius:4px}
input:focus{outline:none;border-color:#00d4aa}
.opt{color:#555;font-size:0.8em}
.sep{border-top:1px solid #222;margin:16px 0}
button{width:100%;padding:12px;margin-top:20px;background:#00d4aa;color:#0a0a0a;border:none;font-family:inherit;font-size:1em;font-weight:bold;cursor:pointer;border-radius:4px}
button:hover{background:#00b894}
</style></head><body>
<h1>&gt; WireClaw Setup</h1>
<p class="sub">Configure your device</p>
<form method="POST" action="/save">
<label>WiFi SSID *</label>
<input type="text" name="wifi_ssid" required>
<label>WiFi Password *</label>
<input type="password" name="wifi_pass" required>
<div class="sep"></div>
<label>OpenRouter API Key</label>
<input type="text" name="api_key" placeholder="sk-or-v1-...">
<p class="opt">Required unless using local LLM</p>
<label>Model</label>
<input type="text" name="model" value="openai/gpt-4o-mini">
<label>Device Name</label>
<input type="text" name="device_name" value="wireclaw-01">
<div class="sep"></div>
<label>API Base URL</label>
<input type="text" name="api_base_url" placeholder="http://192.168.1.x:11434/v1">
<p class="opt">For local LLM (Ollama, etc.)</p>
<div class="sep"></div>
<label>NATS Host</label>
<input type="text" name="nats_host" placeholder="192.168.1.x">
<label>NATS Port</label>
<input type="text" name="nats_port" value="4222">
<div class="sep"></div>
<label>Telegram Bot Token</label>
<input type="text" name="telegram_token">
<label>Telegram Chat ID</label>
<input type="text" name="telegram_chat_id">
<div class="sep"></div>
<label>Timezone</label>
<input type="text" name="timezone" value="UTC0">
<p class="opt">POSIX TZ string (e.g. CET-1CEST,M3.5.0,M10.5.0/3)</p>
<button type="submit">Save &amp; Reboot</button>
</form></body></html>)rawhtml";

static const char SAVED_HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WireClaw - Saved</title><style>
body{font-family:'Courier New',monospace;background:#0a0a0a;color:#00d4aa;display:flex;align-items:center;justify-content:center;min-height:100vh;text-align:center}
h1{font-size:1.5em;margin-bottom:8px}p{color:#666}
</style></head><body>
<div><h1>Config saved!</h1><p>Rebooting...</p></div>
</body></html>)rawhtml";

/*============================================================================
 * Helpers
 *============================================================================*/

static void urlDecode(char *dst, const char *src, int maxLen) {
    int w = 0;
    while (*src && w < maxLen - 1) {
        if (*src == '+') {
            dst[w++] = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[w++] = (char)strtol(hex, nullptr, 16);
            src += 3;
        } else {
            dst[w++] = *src++;
        }
    }
    dst[w] = '\0';
}

/**
 * Extract a URL-encoded form field from a POST body.
 * Handles field names correctly (won't match partial names).
 */
static bool formGetField(const char *body, const char *name,
                          char *dst, int maxLen) {
    char key[64];
    snprintf(key, sizeof(key), "%s=", name);
    int keyLen = strlen(key);

    const char *p = body;
    while ((p = strstr(p, key)) != nullptr) {
        /* Ensure it's at start of body or after & */
        if (p == body || *(p - 1) == '&') {
            p += keyLen;
            /* Extract raw value until & or end */
            char encoded[256];
            int w = 0;
            while (*p && *p != '&' && w < (int)sizeof(encoded) - 1) {
                encoded[w++] = *p++;
            }
            encoded[w] = '\0';
            urlDecode(dst, encoded, maxLen);
            return true;
        }
        p++;
    }
    dst[0] = '\0';
    return false;
}

/** Write a JSON-escaped string value to a file */
static void writeJsonEscaped(File &f, const char *s) {
    f.print('"');
    while (*s) {
        if (*s == '"' || *s == '\\') f.print('\\');
        f.print(*s);
        s++;
    }
    f.print('"');
}

/*============================================================================
 * Config Writing
 *============================================================================*/

static bool saveConfig(const char *body) {
    /* Ensure LittleFS is mounted (format if needed for fresh flash) */
    if (!LittleFS.begin(true)) {
        Serial.printf("[Setup] LittleFS mount failed\n");
        return false;
    }

    File f = LittleFS.open("/config.json", "w");
    if (!f) {
        Serial.printf("[Setup] Failed to open /config.json for writing\n");
        return false;
    }

    char val[128];

    f.print("{\n");

    formGetField(body, "wifi_ssid", val, sizeof(val));
    f.print("  \"wifi_ssid\": "); writeJsonEscaped(f, val); f.print(",\n");

    formGetField(body, "wifi_pass", val, sizeof(val));
    f.print("  \"wifi_pass\": "); writeJsonEscaped(f, val); f.print(",\n");

    formGetField(body, "api_key", val, sizeof(val));
    f.print("  \"api_key\": "); writeJsonEscaped(f, val); f.print(",\n");

    formGetField(body, "model", val, sizeof(val));
    if (val[0] == '\0') strncpy(val, "openai/gpt-4o-mini", sizeof(val));
    f.print("  \"model\": "); writeJsonEscaped(f, val); f.print(",\n");

    formGetField(body, "device_name", val, sizeof(val));
    if (val[0] == '\0') strncpy(val, "wireclaw-01", sizeof(val));
    f.print("  \"device_name\": "); writeJsonEscaped(f, val); f.print(",\n");

    formGetField(body, "api_base_url", val, sizeof(val));
    f.print("  \"api_base_url\": "); writeJsonEscaped(f, val); f.print(",\n");

    formGetField(body, "nats_host", val, sizeof(val));
    f.print("  \"nats_host\": "); writeJsonEscaped(f, val); f.print(",\n");

    formGetField(body, "nats_port", val, sizeof(val));
    if (val[0] == '\0') strncpy(val, "4222", sizeof(val));
    f.print("  \"nats_port\": "); writeJsonEscaped(f, val); f.print(",\n");

    formGetField(body, "telegram_token", val, sizeof(val));
    f.print("  \"telegram_token\": "); writeJsonEscaped(f, val); f.print(",\n");

    formGetField(body, "telegram_chat_id", val, sizeof(val));
    f.print("  \"telegram_chat_id\": "); writeJsonEscaped(f, val); f.print(",\n");

    f.print("  \"telegram_cooldown\": \"60\",\n");

    formGetField(body, "timezone", val, sizeof(val));
    if (val[0] == '\0') strncpy(val, "UTC0", sizeof(val));
    f.print("  \"timezone\": "); writeJsonEscaped(f, val); f.print("\n");

    f.print("}\n");
    f.close();

    Serial.printf("[Setup] Config saved to /config.json\n");
    return true;
}

/*============================================================================
 * HTTP Handler
 *============================================================================*/

static void sendHtml(WiFiClient &client, const char *html) {
    int len = strlen(html);
    client.printf("HTTP/1.1 200 OK\r\n"
                  "Content-Type: text/html; charset=utf-8\r\n"
                  "Connection: close\r\n"
                  "Content-Length: %d\r\n\r\n", len);
    /* Send in chunks to avoid WiFiClient buffer limits */
    const uint8_t *p = (const uint8_t *)html;
    while (len > 0) {
        int chunk = len > 1024 ? 1024 : len;
        client.write(p, chunk);
        p += chunk;
        len -= chunk;
    }
}

static bool handleClient(WiFiClient &client) {
    /* Wait for data with timeout */
    unsigned long timeout = millis() + 3000;
    while (!client.available() && millis() < timeout) {
        delay(1);
    }
    if (!client.available()) {
        client.stop();
        return false;
    }

    /* Read request line */
    String requestLine = client.readStringUntil('\n');
    requestLine.trim();

    bool isPost = requestLine.startsWith("POST");

    /* Read headers */
    int contentLength = 0;
    while (client.connected()) {
        String header = client.readStringUntil('\n');
        header.trim();
        if (header.length() == 0) break;
        if (header.startsWith("Content-Length:") ||
            header.startsWith("content-length:")) {
            contentLength = header.substring(15).toInt();
        }
    }

    if (isPost && contentLength > 0) {
        /* Read POST body */
        static char body[2048];
        int toRead = contentLength < (int)sizeof(body) - 1
                     ? contentLength : (int)sizeof(body) - 1;
        int bytesRead = client.readBytes(body, toRead);
        body[bytesRead] = '\0';

        if (saveConfig(body)) {
            sendHtml(client, SAVED_HTML);
            client.stop();
            Serial.printf("[Setup] Config saved, rebooting in 2s...\n");
            delay(2000);
            ESP.restart();
            return true; /* unreachable */
        } else {
            client.print("HTTP/1.1 500 Error\r\n"
                         "Content-Type: text/plain\r\n"
                         "Connection: close\r\n\r\n"
                         "Failed to save config. Try again.");
        }
    } else {
        /* Serve the setup form for any GET request */
        sendHtml(client, SETUP_HTML);
    }

    client.stop();
    return false;
}

/*============================================================================
 * Main Portal Entry Point
 *============================================================================*/

void runSetupPortal() {
    /* Switch to AP mode */
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP("WireClaw-Setup");
    delay(500); /* Let AP settle */

    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("[Setup] AP started on %s\n", apIP.toString().c_str());
    Serial.printf("[Setup] Connect to WiFi 'WireClaw-Setup' to configure\n");
    Serial.printf("[Setup] Portal timeout: %d seconds\n", PORTAL_TIMEOUT_MS / 1000);

    /* DNS captive portal — resolve all domains to our IP */
    DNSServer dns;
    dns.start(53, "*", apIP);

    /* Web server on port 80 */
    WiFiServer server(80);
    server.begin();

    unsigned long startTime = millis();

    while (millis() - startTime < PORTAL_TIMEOUT_MS) {
        esp_task_wdt_reset();
        dns.processNextRequest();

        /* Pulsing cyan LED — triangle wave, 2s period */
        unsigned long ms = millis() % 2000;
        uint8_t brightness = (ms < 1000)
            ? (uint8_t)(ms * 255 / 1000)
            : (uint8_t)((2000 - ms) * 255 / 1000);
        led(0, brightness, brightness);

        WiFiClient client = server.accept();
        if (client) {
            handleClient(client);
        }

        delay(5);
    }

    /* Timeout — reboot and try again */
    Serial.printf("[Setup] Portal timeout, rebooting...\n");
    ledOff();
    delay(1000);
    ESP.restart();
}
