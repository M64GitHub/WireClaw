/**
 * @file llm_client.cpp
 * @brief OpenRouter LLM client for ESP32 - with tool calling support
 */

#include "llm_client.h"
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>

/* Default OpenRouter API endpoint */
static const char *DEFAULT_HOST = "openrouter.ai";
static const int   DEFAULT_PORT = 443;
static const char *DEFAULT_PATH = "/api/v1/chat/completions";

/* ---- JSON helpers ---- */

static int json_escape(char *dst, int dst_len, const char *src) {
    int w = 0;
    for (int i = 0; src[i] != '\0'; i++) {
        char c = src[i];
        const char *esc = nullptr;
        switch (c) {
            case '\\': esc = "\\\\"; break;
            case '"':  esc = "\\\""; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) continue;
                if (w + 1 >= dst_len) return -1;
                dst[w++] = c;
                continue;
        }
        int elen = strlen(esc);
        if (w + elen >= dst_len) return -1;
        memcpy(dst + w, esc, elen);
        w += elen;
    }
    if (w >= dst_len) return -1;
    dst[w] = '\0';
    return w;
}

static const char *json_find_string(const char *json, int json_len,
                                     const char *key, int *out_len) {
    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || plen >= (int)sizeof(pattern)) return nullptr;

    const char *end = json + json_len;
    const char *p = json;

    while (p < end - plen) {
        const char *found = (const char *)memmem(p, end - p, pattern, plen);
        if (!found) return nullptr;

        const char *after_key = found + plen;
        while (after_key < end && (*after_key == ' ' || *after_key == ':'))
            after_key++;

        if (after_key >= end || *after_key != '"') {
            p = after_key;
            continue;
        }

        const char *val_start = after_key + 1;
        const char *q = val_start;
        while (q < end) {
            if (*q == '\\' && q + 1 < end) { q += 2; continue; }
            if (*q == '"') break;
            q++;
        }
        *out_len = q - val_start;
        return val_start;
    }
    return nullptr;
}

static int json_find_int(const char *json, int json_len,
                          const char *key, int default_val) {
    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || plen >= (int)sizeof(pattern)) return default_val;

    const char *found = (const char *)memmem(json, json_len, pattern, plen);
    if (!found) return default_val;

    const char *after_key = found + plen;
    const char *end = json + json_len;
    while (after_key < end && (*after_key == ' ' || *after_key == ':'))
        after_key++;
    if (after_key >= end) return default_val;

    return atoi(after_key);
}

static int json_unescape(char *buf, int len) {
    int r = 0, w = 0;
    while (r < len) {
        if (buf[r] == '\\' && r + 1 < len) {
            r++;
            switch (buf[r]) {
                case 'n':  buf[w++] = '\n'; break;
                case 'r':  buf[w++] = '\r'; break;
                case 't':  buf[w++] = '\t'; break;
                case '\\': buf[w++] = '\\'; break;
                case '"':  buf[w++] = '"';  break;
                case '/':  buf[w++] = '/';  break;
                default:   buf[w++] = buf[r]; break;
            }
            r++;
        } else {
            buf[w++] = buf[r++];
        }
    }
    buf[w] = '\0';
    return w;
}

/**
 * Find a matching closing brace/bracket, handling nesting and strings.
 * Returns pointer past the closing char, or nullptr.
 */
static const char *json_skip_value(const char *p, const char *end) {
    if (p >= end) return nullptr;

    if (*p == '"') {
        /* Skip string */
        p++;
        while (p < end) {
            if (*p == '\\' && p + 1 < end) { p += 2; continue; }
            if (*p == '"') return p + 1;
            p++;
        }
        return nullptr;
    }

    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (p < end && depth > 0) {
            if (*p == '"') {
                p = json_skip_value(p, end);
                if (!p) return nullptr;
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close) depth--;
            p++;
        }
        return (depth == 0) ? p : nullptr;
    }

    /* Number, bool, null - skip to next delimiter */
    while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != '\n')
        p++;
    return p;
}

/* ---- LlmClient implementation ---- */

LlmClient::LlmClient()
    : m_client(nullptr), m_api_key(nullptr), m_model(nullptr),
      m_port(443), m_use_tls(true) {
    m_error[0] = '\0';
    m_host[0] = '\0';
    m_path[0] = '\0';
}

void LlmClient::begin(const char *api_key, const char *model, const char *base_url) {
    m_api_key = api_key;
    m_model = model;

    /* Parse base_url or use defaults */
    if (base_url && base_url[0]) {
        const char *p = base_url;
        if (strncmp(p, "https://", 8) == 0) {
            m_use_tls = true;
            m_port = 443;
            p += 8;
        } else if (strncmp(p, "http://", 7) == 0) {
            m_use_tls = false;
            m_port = 80;
            p += 7;
        } else {
            m_use_tls = true;
            m_port = 443;
        }

        /* Extract host:port and path */
        const char *slash = strchr(p, '/');
        const char *colon = strchr(p, ':');
        if (colon && (!slash || colon < slash)) {
            int hlen = colon - p;
            if (hlen >= (int)sizeof(m_host)) hlen = sizeof(m_host) - 1;
            memcpy(m_host, p, hlen);
            m_host[hlen] = '\0';
            m_port = atoi(colon + 1);
            if (slash) {
                strncpy(m_path, slash, sizeof(m_path) - 1);
                m_path[sizeof(m_path) - 1] = '\0';
            } else {
                strncpy(m_path, "/", sizeof(m_path));
            }
        } else if (slash) {
            int hlen = slash - p;
            if (hlen >= (int)sizeof(m_host)) hlen = sizeof(m_host) - 1;
            memcpy(m_host, p, hlen);
            m_host[hlen] = '\0';
            strncpy(m_path, slash, sizeof(m_path) - 1);
            m_path[sizeof(m_path) - 1] = '\0';
        } else {
            strncpy(m_host, p, sizeof(m_host) - 1);
            m_host[sizeof(m_host) - 1] = '\0';
            strncpy(m_path, "/", sizeof(m_path));
        }
        Serial.printf("LLM: %s://%s:%d%s\n",
                      m_use_tls ? "https" : "http", m_host, m_port, m_path);
    } else {
        m_use_tls = true;
        strncpy(m_host, DEFAULT_HOST, sizeof(m_host));
        m_port = DEFAULT_PORT;
        strncpy(m_path, DEFAULT_PATH, sizeof(m_path));
    }

    if (m_use_tls) {
        m_secure_client.setInsecure();
        m_secure_client.setTimeout(LLM_READ_TIMEOUT_MS / 1000);
        m_client = &m_secure_client;
    } else {
        m_plain_client.setTimeout(LLM_READ_TIMEOUT_MS / 1000);
        m_client = &m_plain_client;
    }
}

int LlmClient::buildRequest(char *buf, int buf_len,
                              const LlmMessage *messages, int count,
                              const char *tools_json) {
    int w = 0;

    w += snprintf(buf + w, buf_len - w,
        "{\"model\":\"%s\",\"messages\":[", m_model);
    if (w >= buf_len) return -1;

    for (int i = 0; i < count; i++) {
        if (i > 0) {
            if (w + 1 >= buf_len) return -1;
            buf[w++] = ',';
        }

        const LlmMessage *msg = &messages[i];

        if (msg->type == LLM_MSG_TOOL_CALL) {
            /* Assistant message with tool_calls */
            w += snprintf(buf + w, buf_len - w,
                "{\"role\":\"assistant\"");
            if (w >= buf_len) return -1;

            if (msg->content && msg->content[0]) {
                w += snprintf(buf + w, buf_len - w, ",\"content\":\"");
                if (w >= buf_len) return -1;
                int esc = json_escape(buf + w, buf_len - w, msg->content);
                if (esc < 0) return -1;
                w += esc;
                w += snprintf(buf + w, buf_len - w, "\"");
            } else {
                w += snprintf(buf + w, buf_len - w, ",\"content\":null");
            }
            if (w >= buf_len) return -1;

            if (msg->tool_calls_json) {
                w += snprintf(buf + w, buf_len - w,
                    ",\"tool_calls\":%s", msg->tool_calls_json);
            }
            if (w >= buf_len) return -1;
            w += snprintf(buf + w, buf_len - w, "}");

        } else if (msg->type == LLM_MSG_TOOL_RESULT) {
            /* Tool result message */
            w += snprintf(buf + w, buf_len - w,
                "{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"",
                msg->tool_call_id ? msg->tool_call_id : "");
            if (w >= buf_len) return -1;

            int esc = json_escape(buf + w, buf_len - w,
                                   msg->content ? msg->content : "");
            if (esc < 0) return -1;
            w += esc;
            w += snprintf(buf + w, buf_len - w, "\"}");

        } else {
            /* Normal message */
            w += snprintf(buf + w, buf_len - w,
                "{\"role\":\"%s\",\"content\":\"", msg->role);
            if (w >= buf_len) return -1;

            int esc = json_escape(buf + w, buf_len - w,
                                   msg->content ? msg->content : "");
            if (esc < 0) return -1;
            w += esc;
            w += snprintf(buf + w, buf_len - w, "\"}");
        }

        if (w >= buf_len) return -1;
    }

    w += snprintf(buf + w, buf_len - w, "]");
    if (w >= buf_len) return -1;

    /* Add tools if provided */
    if (tools_json && tools_json[0]) {
        w += snprintf(buf + w, buf_len - w,
            ",\"tools\":%s,\"tool_choice\":\"auto\"", tools_json);
        if (w >= buf_len) return -1;
    }

    w += snprintf(buf + w, buf_len - w,
        ",\"max_tokens\":2048,\"temperature\":0.7}");

    if (w >= buf_len) return -1;
    return w;
}

int LlmClient::parseToolCalls(const char *body, int body_len, LlmResult *result) {
    result->tool_call_count = 0;
    result->tool_calls_json[0] = '\0';

    /* Find "tool_calls" key */
    const char *tc_key = "\"tool_calls\"";
    const char *found = (const char *)memmem(body, body_len, tc_key, strlen(tc_key));
    if (!found) return 0;

    /* Skip to the array start '[' */
    const char *end = body + body_len;
    const char *p = found + strlen(tc_key);
    while (p < end && *p != '[') p++;
    if (p >= end) return 0;

    const char *arr_start = p;

    /* Find matching ']' */
    const char *arr_end = json_skip_value(p, end);
    if (!arr_end) return 0;

    /* Save raw tool_calls JSON for echoing back */
    int tc_json_len = arr_end - arr_start;
    if (tc_json_len < (int)sizeof(result->tool_calls_json) - 1) {
        memcpy(result->tool_calls_json, arr_start, tc_json_len);
        result->tool_calls_json[tc_json_len] = '\0';
    } else {
        Serial.printf("[LLM] Warning: tool_calls_json too large (%d bytes, max %d)\n",
                      tc_json_len, (int)sizeof(result->tool_calls_json) - 1);
    }

    /* Parse individual tool calls from the array */
    p = arr_start + 1; /* skip '[' */
    int count = 0;

    while (p < arr_end && count < LLM_MAX_TOOL_CALLS) {
        /* Find next '{' */
        while (p < arr_end && *p != '{') p++;
        if (p >= arr_end) break;

        const char *obj_start = p;
        const char *obj_end = json_skip_value(p, arr_end);
        if (!obj_end) break;

        int obj_len = obj_end - obj_start;
        LlmToolCall *tc = &result->tool_calls[count];

        /* Extract "id" */
        int id_len = 0;
        const char *id = json_find_string(obj_start, obj_len, "id", &id_len);
        if (id && id_len > 0) {
            int clen = id_len < (int)sizeof(tc->id) - 1 ? id_len : (int)sizeof(tc->id) - 1;
            memcpy(tc->id, id, clen);
            tc->id[clen] = '\0';
        } else {
            tc->id[0] = '\0';
        }

        /* Extract function name - find "function" object first */
        int name_len = 0;
        const char *name = json_find_string(obj_start, obj_len, "name", &name_len);
        if (name && name_len > 0) {
            int clen = name_len < (int)sizeof(tc->name) - 1 ? name_len : (int)sizeof(tc->name) - 1;
            memcpy(tc->name, name, clen);
            tc->name[clen] = '\0';
        } else {
            tc->name[0] = '\0';
        }

        /* Extract arguments - it's a JSON string value containing JSON */
        int args_len = 0;
        const char *args = json_find_string(obj_start, obj_len, "arguments", &args_len);
        if (args && args_len > 0) {
            int clen = args_len < (int)sizeof(tc->arguments) - 1 ? args_len : (int)sizeof(tc->arguments) - 1;
            memcpy(tc->arguments, args, clen);
            tc->arguments[clen] = '\0';
            /* Unescape the JSON string (arguments contain escaped JSON) */
            json_unescape(tc->arguments, clen);
        } else {
            tc->arguments[0] = '\0';
        }

        count++;
        p = obj_end;
    }

    result->tool_call_count = count;
    return count;
}

bool LlmClient::parseResponse(const char *body, int body_len, LlmResult *result) {
    result->ok = false;
    result->content[0] = '\0';
    result->content_len = 0;
    result->prompt_tokens = 0;
    result->completion_tokens = 0;
    result->tool_call_count = 0;
    result->tool_calls_json[0] = '\0';

    /* Check for tool calls first */
    int tc_count = parseToolCalls(body, body_len, result);

    /* Extract "content" field */
    int clen = 0;
    const char *content = json_find_string(body, body_len, "content", &clen);
    if (content && clen > 0) {
        int copy_len = clen < LLM_MAX_RESPONSE_LEN - 1 ? clen : LLM_MAX_RESPONSE_LEN - 1;
        memcpy(result->content, content, copy_len);
        result->content[copy_len] = '\0';
        result->content_len = json_unescape(result->content, copy_len);
    }

    /* If we got tool calls, that's a success even without content */
    if (tc_count > 0) {
        result->ok = true;
        result->prompt_tokens = json_find_int(body, body_len, "prompt_tokens", 0);
        result->completion_tokens = json_find_int(body, body_len, "completion_tokens", 0);
        return true;
    }

    /* No tool calls - need content */
    if (result->content_len <= 0) {
        int elen = 0;
        const char *errmsg = json_find_string(body, body_len, "message", &elen);
        if (errmsg && elen > 0) {
            int copy = elen < (int)sizeof(m_error) - 1 ? elen : (int)sizeof(m_error) - 1;
            memcpy(m_error, errmsg, copy);
            m_error[copy] = '\0';
        } else {
            snprintf(m_error, sizeof(m_error), "No content in response");
        }
        return false;
    }

    result->prompt_tokens = json_find_int(body, body_len, "prompt_tokens", 0);
    result->completion_tokens = json_find_int(body, body_len, "completion_tokens", 0);
    result->ok = true;
    return true;
}

int LlmClient::readResponse(char *buf, int buf_len) {
    int content_length = -1;
    bool chunked = false;

    String status_line = m_client->readStringUntil('\n');
    if (status_line.length() < 12) {
        snprintf(m_error, sizeof(m_error), "Invalid HTTP response");
        return -1;
    }
    int http_status = status_line.substring(9, 12).toInt();
    if (g_debug) Serial.printf("[LLM] HTTP %d\n", http_status);

    while (m_client->connected()) {
        String header = m_client->readStringUntil('\n');
        header.trim();
        if (header.length() == 0) break;

        if (header.startsWith("Content-Length:") ||
            header.startsWith("content-length:")) {
            content_length = header.substring(15).toInt();
        }
        if (header.indexOf("chunked") >= 0) {
            chunked = true;
        }
    }
    if (g_debug) Serial.printf("[LLM] content_length=%d chunked=%d\n", content_length, chunked);

    int total = 0;
    int target = (content_length > 0 && !chunked)
                 ? (content_length < buf_len - 1 ? content_length : buf_len - 1)
                 : buf_len - 1;

    unsigned long last_data = millis();
    while (total < target) {
        esp_task_wdt_reset();
        int avail = m_client->available();
        if (avail > 0) {
            int to_read = avail < (target - total) ? avail : (target - total);
            int rd = m_client->readBytes(buf + total, to_read);
            total += rd;
            last_data = millis();
        } else if (!m_client->connected()) {
            break;
        } else if (millis() - last_data > 10000) {
            if (g_debug) Serial.printf("[LLM] Read timeout after %d bytes\n", total);
            break;
        } else {
            delay(10);
        }
    }

    buf[total] = '\0';
    return total;
}

bool LlmClient::chat(const LlmMessage *messages, int count,
                       const char *tools_json, LlmResult *result) {
    result->ok = false;
    result->content[0] = '\0';
    result->content_len = 0;
    result->http_status = 0;
    result->tool_call_count = 0;

    static char request_buf[LLM_MAX_REQUEST_LEN];
    int req_len = buildRequest(request_buf, sizeof(request_buf),
                                messages, count, tools_json);
    if (req_len < 0) {
        snprintf(m_error, sizeof(m_error), "Request too large for buffer");
        return false;
    }

    if (g_debug) Serial.printf("[LLM] Connecting to %s:%d...\n", m_host, m_port);
    unsigned long t0 = millis();

    if (!m_client->connect(m_host, m_port)) {
        snprintf(m_error, sizeof(m_error), "%s connect failed",
                 m_use_tls ? "TLS" : "TCP");
        return false;
    }

    if (g_debug) Serial.printf("[LLM] Connected (%lums). Sending %d bytes...\n",
                               millis() - t0, req_len);

    m_client->printf("POST %s HTTP/1.1\r\n", m_path);
    m_client->printf("Host: %s\r\n", m_host);
    if (m_api_key && m_api_key[0])
        m_client->printf("Authorization: Bearer %s\r\n", m_api_key);
    m_client->printf("Content-Type: application/json\r\n");
    m_client->printf("Content-Length: %d\r\n", req_len);
    m_client->printf("Connection: close\r\n");
    m_client->printf("\r\n");
    m_client->write((uint8_t *)request_buf, req_len);

    if (g_debug) Serial.printf("[LLM] Request sent. Waiting for response...\n");

    unsigned long wait_start = millis();
    while (!m_client->available()) {
        esp_task_wdt_reset();
        if (millis() - wait_start > LLM_READ_TIMEOUT_MS) {
            snprintf(m_error, sizeof(m_error), "Response timeout (%ds)",
                     LLM_READ_TIMEOUT_MS / 1000);
            m_client->stop();
            return false;
        }
        delay(50);
    }

    static char response_buf[LLM_MAX_RESPONSE_LEN + 2048];
    int body_len = readResponse(response_buf, sizeof(response_buf));

    m_client->stop();

    if (body_len <= 0) {
        snprintf(m_error, sizeof(m_error), "Empty response body");
        return false;
    }

    if (g_debug) {
        Serial.printf("[LLM] Response: %d bytes (%lums total)\n",
                      body_len, millis() - t0);
        Serial.printf("[LLM] Body: %.*s\n", body_len < 500 ? body_len : 500,
                      response_buf);
    }

    return parseResponse(response_buf, body_len, result);
}
