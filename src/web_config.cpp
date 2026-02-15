/**
 * @file web_config.cpp
 * @brief Web-based configuration portal for runtime config changes
 *
 * Runs on port 80 during normal operation (not during setup portal).
 * REST API + PROGMEM single-page app for config, prompt, memory, status.
 */

#include "web_config.h"
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <LittleFS.h>
#include "version.h"

/* Externs from main.cpp */
extern char cfg_wifi_ssid[64];
extern char cfg_wifi_pass[64];
extern char cfg_api_key[128];
extern char cfg_model[64];
extern char cfg_device_name[32];
extern char cfg_api_base_url[128];
extern char cfg_nats_host[64];
extern int  cfg_nats_port;
extern char cfg_telegram_token[64];
extern char cfg_telegram_chat_id[16];
extern char cfg_system_prompt[4096];
extern char cfg_timezone[64];
extern int  cfg_telegram_cooldown;
extern bool g_nats_enabled;
extern bool g_nats_connected;
extern bool g_telegram_enabled;

static WebServer server(80);

/*============================================================================
 * Helpers (duplicated from main.cpp / setup_portal.cpp -- they're static there)
 *============================================================================*/

static int wcReadFile(const char *path, char *buf, int buf_len) {
    File f = LittleFS.open(path, "r");
    if (!f) return -1;
    int len = f.readBytes(buf, buf_len - 1);
    buf[len] = '\0';
    f.close();
    return len;
}

static bool wcJsonGetString(const char *json, const char *key,
                             char *dst, int dst_len) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    int w = 0;
    while (*p && *p != '"' && w < dst_len - 1) {
        if (*p == '\\' && *(p + 1)) { p++; }
        dst[w++] = *p++;
    }
    dst[w] = '\0';
    return w > 0;
}

static void wcWriteJsonEscaped(File &f, const char *s) {
    f.print('"');
    while (*s) {
        if (*s == '"' || *s == '\\') f.print('\\');
        if (*s == '\n') { f.print("\\n"); s++; continue; }
        if ((uint8_t)*s >= 0x20) f.print(*s);
        s++;
    }
    f.print('"');
}

/** JSON-escape into a buffer (for API responses) */
static int jsonEscapeBuf(char *dst, int dst_len, const char *src) {
    int w = 0;
    for (int i = 0; src[i] && w < dst_len - 2; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (w + 2 >= dst_len) break;
            dst[w++] = '\\'; dst[w++] = c;
        } else if (c == '\n') {
            if (w + 2 >= dst_len) break;
            dst[w++] = '\\'; dst[w++] = 'n';
        } else if (c == '\r' || (uint8_t)c < 0x20) {
            /* skip control chars */
        } else {
            dst[w++] = c;
        }
    }
    dst[w] = '\0';
    return w;
}

/** Mask a sensitive string: show last 4 chars prefixed with "..." */
static void maskSensitive(const char *src, char *dst, int dst_len) {
    int len = strlen(src);
    if (len == 0) { dst[0] = '\0'; return; }
    if (len <= 4) {
        snprintf(dst, dst_len, "****");
    } else {
        snprintf(dst, dst_len, "...%s", src + len - 4);
    }
}

/*============================================================================
 * REST API Handlers
 *============================================================================*/

static void handleGetConfig() {
    static char buf[1024];
    char masked_key[16], masked_pass[16], masked_tg[16];
    maskSensitive(cfg_api_key, masked_key, sizeof(masked_key));
    maskSensitive(cfg_wifi_pass, masked_pass, sizeof(masked_pass));
    maskSensitive(cfg_telegram_token, masked_tg, sizeof(masked_tg));

    snprintf(buf, sizeof(buf),
        "{"
        "\"wifi_ssid\":\"%s\","
        "\"wifi_pass\":\"%s\","
        "\"api_key\":\"%s\","
        "\"model\":\"%s\","
        "\"device_name\":\"%s\","
        "\"api_base_url\":\"%s\","
        "\"nats_host\":\"%s\","
        "\"nats_port\":\"%d\","
        "\"telegram_token\":\"%s\","
        "\"telegram_chat_id\":\"%s\","
        "\"telegram_cooldown\":\"%d\","
        "\"timezone\":\"%s\""
        "}",
        cfg_wifi_ssid, masked_pass, masked_key, cfg_model,
        cfg_device_name, cfg_api_base_url, cfg_nats_host, cfg_nats_port,
        masked_tg, cfg_telegram_chat_id, cfg_telegram_cooldown, cfg_timezone);

    server.send(200, "application/json", buf);
}

/** Check if a value is a masked sentinel (starts with "..." or "****") */
static bool isMasked(const char *val) {
    if (val[0] == '\0') return false;
    if (strncmp(val, "...", 3) == 0) return true;
    if (strncmp(val, "****", 4) == 0) return true;
    return false;
}

static void handlePostConfig() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"no body\"}");
        return;
    }

    const String &body = server.arg("plain");

    /* Read existing config to preserve masked fields */
    static char existing[1024];
    int elen = wcReadFile("/config.json", existing, sizeof(existing));
    if (elen <= 0) existing[0] = '\0';

    /* Parse each field from POST body, fall back to existing if masked */
    struct Field {
        const char *key;
        char val[128];
    };
    static Field fields[12];
    const char *keys[] = {
        "wifi_ssid", "wifi_pass", "api_key", "model", "device_name",
        "api_base_url", "nats_host", "nats_port", "telegram_token",
        "telegram_chat_id", "telegram_cooldown", "timezone"
    };

    for (int i = 0; i < 12; i++) {
        fields[i].key = keys[i];
        fields[i].val[0] = '\0';

        char newVal[128] = {0};
        bool hasNew = wcJsonGetString(body.c_str(), keys[i], newVal, sizeof(newVal));

        if (hasNew && !isMasked(newVal)) {
            strncpy(fields[i].val, newVal, sizeof(fields[i].val) - 1);
        } else if (existing[0]) {
            wcJsonGetString(existing, keys[i], fields[i].val, sizeof(fields[i].val));
        }
    }

    /* Write complete config */
    File f = LittleFS.open("/config.json", "w");
    if (!f) {
        server.send(500, "application/json", "{\"error\":\"write failed\"}");
        return;
    }

    f.print("{\n");
    for (int i = 0; i < 12; i++) {
        f.print("  \""); f.print(fields[i].key); f.print("\": ");
        wcWriteJsonEscaped(f, fields[i].val);
        if (i < 11) f.print(",");
        f.print("\n");
    }
    f.print("}\n");
    f.close();

    Serial.printf("[WebConfig] Config saved to /config.json\n");
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"Config saved. Reboot to apply.\"}");
}

static void handleGetPrompt() {
    server.send(200, "text/plain", cfg_system_prompt);
}

static void handlePostPrompt() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "no body");
        return;
    }
    const String &body = server.arg("plain");
    if (body.length() >= sizeof(cfg_system_prompt)) {
        server.send(400, "text/plain", "too large");
        return;
    }

    File f = LittleFS.open("/system_prompt.txt", "w");
    if (!f) {
        server.send(500, "text/plain", "write failed");
        return;
    }
    f.print(body);
    f.close();

    /* Update in-memory (live, no reboot needed) */
    strncpy(cfg_system_prompt, body.c_str(), sizeof(cfg_system_prompt) - 1);
    cfg_system_prompt[sizeof(cfg_system_prompt) - 1] = '\0';

    Serial.printf("[WebConfig] System prompt updated (%d chars)\n", (int)body.length());
    server.send(200, "text/plain", "ok");
}

static void handleGetMemory() {
    static char buf[512];
    int len = wcReadFile("/memory.txt", buf, sizeof(buf));
    if (len <= 0) buf[0] = '\0';
    server.send(200, "text/plain", buf);
}

static void handlePostMemory() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "no body");
        return;
    }
    const String &body = server.arg("plain");

    File f = LittleFS.open("/memory.txt", "w");
    if (!f) {
        server.send(500, "text/plain", "write failed");
        return;
    }
    f.print(body);
    f.close();

    Serial.printf("[WebConfig] Memory updated (%d chars)\n", (int)body.length());
    server.send(200, "text/plain", "ok");
}

static void handleGetStatus() {
    static char buf[512];
    unsigned long uptime = millis() / 1000;
    unsigned long days = uptime / 86400;
    unsigned long hours = (uptime % 86400) / 3600;
    unsigned long mins = (uptime % 3600) / 60;
    unsigned long secs = uptime % 60;

    snprintf(buf, sizeof(buf),
        "{"
        "\"version\":\"%s\","
        "\"device_name\":\"%s\","
        "\"uptime\":\"%lud %luh %lum %lus\","
        "\"uptime_seconds\":%lu,"
        "\"heap_free\":%u,"
        "\"heap_total\":%u,"
        "\"wifi_ssid\":\"%s\","
        "\"wifi_ip\":\"%s\","
        "\"wifi_rssi\":%d,"
        "\"model\":\"%s\","
        "\"nats\":\"%s\","
        "\"telegram\":\"%s\""
        "}",
        WIRECLAW_VERSION, cfg_device_name,
        days, hours, mins, secs, uptime,
        ESP.getFreeHeap(), ESP.getHeapSize(),
        cfg_wifi_ssid, WiFi.localIP().toString().c_str(), WiFi.RSSI(),
        cfg_model,
        g_nats_enabled ? (g_nats_connected ? "connected" : "disconnected") : "disabled",
        g_telegram_enabled ? "enabled" : "disabled");

    server.send(200, "application/json", buf);
}

static void handleReboot() {
    extern bool          g_reboot_pending;
    extern unsigned long g_reboot_at;
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"Rebooting...\"}");
    g_reboot_pending = true;
    g_reboot_at = millis() + 2000; /* 2s: enough for HTTP response to flush */
}

/*============================================================================
 * HTML UI (PROGMEM)
 *============================================================================*/

static const char WEB_CONFIG_HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WireClaw Config</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{
--bg:#08090e;--bg2:#0d1019;--bg3:#141822;
--accent:#00d4aa;--accent-dim:rgba(0,212,170,0.15);--accent-glow:rgba(0,212,170,0.25);
--text:#e8eaf0;--text2:#8b92a8;--text3:#4a5068;
--border:rgba(255,255,255,0.06);--border-a:rgba(0,212,170,0.25);
--red:#ff4757;
--font:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
--mono:"SF Mono","Cascadia Code","Fira Code",Consolas,monospace;
}
body{font-family:var(--font);background:var(--bg);color:var(--text);min-height:100vh}
.wrap{max-width:640px;margin:0 auto;padding:1rem}
header{display:flex;align-items:center;gap:0.75rem;margin-bottom:1.5rem}
header h1{font-size:1.25rem;font-weight:700}
header .ver{font-family:var(--mono);font-size:0.75rem;color:var(--accent);background:var(--accent-dim);
border:1px solid var(--border-a);border-radius:9999px;padding:0.2rem 0.6rem}
nav{display:flex;gap:0.5rem;margin-bottom:1.5rem;border-bottom:1px solid var(--border);padding-bottom:0.5rem}
nav button{background:none;border:none;color:var(--text2);font-family:var(--font);font-size:0.9rem;
font-weight:500;padding:0.5rem 1rem;cursor:pointer;border-radius:8px 8px 0 0;
border-bottom:2px solid transparent;transition:all 0.15s}
nav button:hover{color:var(--text)}
nav button.active{color:var(--accent);border-bottom-color:var(--accent)}
.tab{display:none}.tab.active{display:block}
.card{background:var(--bg3);border:1px solid var(--border);border-radius:12px;padding:1.5rem;margin-bottom:1rem}
label{display:block;font-size:0.8rem;color:var(--accent);font-weight:600;margin:1rem 0 0.25rem;
font-family:var(--mono);text-transform:uppercase;letter-spacing:0.04em}
label:first-child{margin-top:0}
input[type=text],input[type=password],input[type=number]{width:100%;padding:0.6rem 0.75rem;
background:var(--bg2);border:1px solid var(--border);border-radius:8px;color:var(--text);
font-family:var(--mono);font-size:0.85rem;transition:border-color 0.15s}
input:focus{outline:none;border-color:var(--border-a)}
textarea{width:100%;padding:0.75rem;background:var(--bg2);border:1px solid var(--border);
border-radius:8px;color:var(--text);font-family:var(--mono);font-size:0.85rem;
resize:vertical;min-height:200px;line-height:1.6;transition:border-color 0.15s}
textarea:focus{outline:none;border-color:var(--border-a)}
.hint{font-size:0.75rem;color:var(--text3);margin-top:0.2rem}
.btn{display:inline-flex;align-items:center;gap:0.5rem;padding:0.6rem 1.25rem;border:none;
border-radius:8px;font-weight:600;font-size:0.9rem;cursor:pointer;transition:all 0.2s;
font-family:var(--font)}
.btn-primary{background:var(--accent);color:var(--bg)}
.btn-primary:hover{box-shadow:0 0 20px var(--accent-glow)}
.btn-danger{background:var(--red);color:#fff}
.btn-danger:hover{box-shadow:0 0 20px rgba(255,71,87,0.3)}
.btn-outline{background:transparent;color:var(--text);border:1px solid var(--border-a)}
.btn-outline:hover{border-color:var(--accent);color:var(--accent)}
.actions{display:flex;gap:0.75rem;margin-top:1.25rem;flex-wrap:wrap}
.sep{border-top:1px solid var(--border);margin:1rem 0}
.toast{position:fixed;bottom:1.5rem;left:50%;transform:translateX(-50%);padding:0.6rem 1.25rem;
border-radius:8px;font-size:0.85rem;font-weight:500;z-index:999;opacity:0;
transition:opacity 0.3s;pointer-events:none}
.toast.show{opacity:1}
.toast.ok{background:var(--accent);color:var(--bg)}
.toast.err{background:var(--red);color:#fff}
.status-grid{display:grid;grid-template-columns:1fr 1fr;gap:0.75rem}
.status-item{background:var(--bg2);border:1px solid var(--border);border-radius:8px;padding:0.75rem}
.status-item .label{font-size:0.7rem;color:var(--text3);font-family:var(--mono);
text-transform:uppercase;letter-spacing:0.04em;margin-bottom:0.25rem}
.status-item .value{font-size:0.95rem;color:var(--text);font-family:var(--mono);word-break:break-all}
.status-item .value.accent{color:var(--accent)}
.status-item.full{grid-column:1/-1}
@media(max-width:480px){
.wrap{padding:0.75rem}
.card{padding:1rem}
.status-grid{grid-template-columns:1fr}
nav button{padding:0.4rem 0.6rem;font-size:0.8rem}
}
</style></head><body>
<div class="wrap">
<header>
<h1>WireClaw</h1>
<span class="ver" id="hdr-ver"></span>
</header>
<nav>
<button class="active" onclick="showTab('config')">Config</button>
<button onclick="showTab('prompt')">Prompt</button>
<button onclick="showTab('memory')">Memory</button>
<button onclick="showTab('status')">Status</button>
</nav>

<div id="config" class="tab active">
<div class="card">
<label>WiFi SSID</label>
<input type="text" id="c_wifi_ssid">
<label>WiFi Password</label>
<input type="password" id="c_wifi_pass">
<div class="sep"></div>
<label>API Key</label>
<input type="password" id="c_api_key">
<label>Model</label>
<input type="text" id="c_model">
<label>Device Name</label>
<input type="text" id="c_device_name">
<label>API Base URL</label>
<input type="text" id="c_api_base_url" placeholder="http://192.168.1.x:11434/v1">
<p class="hint">For local LLM. Leave empty for OpenRouter.</p>
<div class="sep"></div>
<label>NATS Host</label>
<input type="text" id="c_nats_host">
<label>NATS Port</label>
<input type="number" id="c_nats_port">
<div class="sep"></div>
<label>Telegram Bot Token</label>
<input type="password" id="c_telegram_token">
<label>Telegram Chat ID</label>
<input type="text" id="c_telegram_chat_id">
<label>Telegram Cooldown (seconds)</label>
<input type="number" id="c_telegram_cooldown">
<div class="sep"></div>
<label>Timezone</label>
<input type="text" id="c_timezone">
<p class="hint">POSIX TZ string, e.g. CET-1CEST,M3.5.0,M10.5.0/3</p>
<div class="actions">
<button class="btn btn-primary" onclick="saveConfig()">Save Config</button>
<button class="btn btn-danger" onclick="reboot()">Reboot</button>
</div>
<p class="hint" style="margin-top:0.75rem">Reboot required to apply config changes.</p>
</div>
</div>

<div id="prompt" class="tab">
<div class="card">
<label>System Prompt</label>
<textarea id="prompt_text" rows="12" maxlength="4095"></textarea>
<p class="hint">Applied immediately, no reboot needed. Max 4095 chars.</p>
<div class="actions">
<button class="btn btn-primary" onclick="savePrompt()">Save Prompt</button>
</div>
</div>
</div>

<div id="memory" class="tab">
<div class="card">
<label>AI Memory</label>
<textarea id="memory_text" rows="8" maxlength="511"></textarea>
<p class="hint">Persistent context injected into every conversation. Active on next chat.</p>
<div class="actions">
<button class="btn btn-primary" onclick="saveMemory()">Save Memory</button>
</div>
</div>
</div>

<div id="status" class="tab">
<div class="card">
<div class="status-grid" id="status-grid"></div>
<div class="actions">
<button class="btn btn-outline" onclick="loadStatus()">Refresh</button>
<button class="btn btn-danger" onclick="reboot()">Reboot</button>
</div>
</div>
</div>
</div>

<div class="toast" id="toast"></div>

<script>
function showTab(id){
document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
document.querySelectorAll('nav button').forEach(b=>b.classList.remove('active'));
document.getElementById(id).classList.add('active');
event.target.classList.add('active');
if(id==='status')loadStatus();
if(id==='prompt')loadPrompt();
if(id==='memory')loadMemory();
}
function toast(msg,ok){
var t=document.getElementById('toast');
t.textContent=msg;t.className='toast show '+(ok?'ok':'err');
setTimeout(function(){t.className='toast'},2500);
}
function loadConfig(){
fetch('/api/config').then(r=>r.json()).then(d=>{
var f=['wifi_ssid','wifi_pass','api_key','model','device_name','api_base_url',
'nats_host','nats_port','telegram_token','telegram_chat_id','telegram_cooldown','timezone'];
f.forEach(k=>{var el=document.getElementById('c_'+k);if(el)el.value=d[k]||''});
}).catch(e=>toast('Failed to load config',false));
}
function saveConfig(){
var f=['wifi_ssid','wifi_pass','api_key','model','device_name','api_base_url',
'nats_host','nats_port','telegram_token','telegram_chat_id','telegram_cooldown','timezone'];
var d={};f.forEach(k=>{d[k]=document.getElementById('c_'+k).value});
fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify(d)}).then(r=>r.json()).then(j=>{
toast(j.message||'Saved',j.ok!==false);
}).catch(e=>toast('Save failed',false));
}
function loadPrompt(){
fetch('/api/prompt').then(r=>r.text()).then(t=>{
document.getElementById('prompt_text').value=t;
}).catch(e=>toast('Failed to load prompt',false));
}
function savePrompt(){
var t=document.getElementById('prompt_text').value;
fetch('/api/prompt',{method:'POST',headers:{'Content-Type':'text/plain'},
body:t}).then(r=>{if(r.ok)toast('Prompt saved',true);else toast('Save failed',false);
}).catch(e=>toast('Save failed',false));
}
function loadMemory(){
fetch('/api/memory').then(r=>r.text()).then(t=>{
document.getElementById('memory_text').value=t;
}).catch(e=>toast('Failed to load memory',false));
}
function saveMemory(){
var t=document.getElementById('memory_text').value;
fetch('/api/memory',{method:'POST',headers:{'Content-Type':'text/plain'},
body:t}).then(r=>{if(r.ok)toast('Memory saved',true);else toast('Save failed',false);
}).catch(e=>toast('Save failed',false));
}
function loadStatus(){
fetch('/api/status').then(r=>r.json()).then(d=>{
var items=[
{l:'Version',v:d.version,cls:'accent'},
{l:'Device',v:d.device_name},
{l:'Uptime',v:d.uptime},
{l:'Heap',v:Math.round(d.heap_free/1024)+'KB / '+Math.round(d.heap_total/1024)+'KB'},
{l:'WiFi',v:d.wifi_ssid+' ('+d.wifi_rssi+'dBm)',full:true},
{l:'IP Address',v:d.wifi_ip,cls:'accent'},
{l:'Model',v:d.model,full:true},
{l:'NATS',v:d.nats},
{l:'Telegram',v:d.telegram}
];
var h='';items.forEach(i=>{
h+='<div class="status-item'+(i.full?' full':'')+'"><div class="label">'+i.l+
'</div><div class="value'+(i.cls?' '+i.cls:'')+'">'+i.v+'</div></div>';
});
document.getElementById('status-grid').innerHTML=h;
}).catch(e=>toast('Failed to load status',false));
}
function reboot(){
if(!confirm('Reboot device?'))return;
fetch('/api/reboot',{method:'POST'}).then(()=>{
toast('Rebooting...',true);
}).catch(()=>toast('Rebooting...',true));
}
loadConfig();
fetch('/api/status').then(r=>r.json()).then(d=>{
document.getElementById('hdr-ver').textContent='v'+d.version;
}).catch(()=>{});
</script>
</body></html>)rawhtml";

/*============================================================================
 * Setup & Loop
 *============================================================================*/

void webConfigSetup() {
    /* mDNS */
    if (MDNS.begin(cfg_device_name)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS: http://%s.local/\n", cfg_device_name);
    } else {
        Serial.printf("mDNS: failed to start\n");
    }

    /* Routes */
    server.on("/", HTTP_GET, []() {
        server.send_P(200, "text/html", WEB_CONFIG_HTML);
    });

    server.on("/api/config", HTTP_GET, handleGetConfig);
    server.on("/api/config", HTTP_POST, handlePostConfig);
    server.on("/api/prompt", HTTP_GET, handleGetPrompt);
    server.on("/api/prompt", HTTP_POST, handlePostPrompt);
    server.on("/api/memory", HTTP_GET, handleGetMemory);
    server.on("/api/memory", HTTP_POST, handlePostMemory);
    server.on("/api/status", HTTP_GET, handleGetStatus);
    server.on("/api/reboot", HTTP_POST, handleReboot);

    server.begin();
    Serial.printf("WebConfig: http://%s/\n", WiFi.localIP().toString().c_str());
}

void webConfigLoop() {
    server.handleClient();
}
