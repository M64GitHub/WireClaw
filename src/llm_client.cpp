/**
 * @file llm_client.cpp
 * @brief OpenRouter LLM client for ESP32
 */

#include "llm_client.h"
#include <WiFiClientSecure.h>

/* OpenRouter API endpoint */
static const char *OPENROUTER_HOST = "openrouter.ai";
static const int   OPENROUTER_PORT = 443;
static const char *OPENROUTER_PATH = "/api/v1/chat/completions";

/*
 * ISRG Root X1 - Let's Encrypt root CA (used by openrouter.ai)
 * Valid until 2035-06-04
 */
static const char *ROOT_CA = R"CA(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6
UA5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+s
WT8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qy
HB5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+U
CvdAfPXbIKGSIFBqKwEhHhBMcM6GjM/e28bCKqIZtKx5Gp4BYQbh3R+EBhxmFhpe
BGIC1nGHsra+2sHM2uKYR4MXqobGGiENHY8PIYXGM0eopqSVEvbYMb7HNa0LwBSI
J+HSTHxKYLPmwjM0EhXJ0zQ3bxH5CREfNBp4K5oBB/6SoK8+egCdNOiY6j8qJRhN
Mdo1DC+0C7MeMJyBFj+TK5K1D2g6mSCij9gE3wiuPetNxGFnYHhPyel99f3Lcvn0
S3jq2xJCgn4dJzAUjQ4JEk9tDhnJhPWGCFlaLSjN8RiNjfMA0+WoA9J1/kg7iQco
bMCxLPAsPMzGVP8wIDIqz8YVABEBAAG/HQ4wYjELMAkGA1UEBhMCVVMxKTAnBgNV
BAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2VhcmNoIEdyb3VwMRUwEwYDVQQDEwxJ
U1JHIFJvb3QgWDEwHhcNMjAwOTA0MDAwMDAwWhcNMjUwOTE1MTYwMDAwWjAyMQsw
CQYDVQQGEwJVUzEWMBQGA1UEChMNTGV0J3MgRW5jcnlwdDELMAkGA1UEAxMCUjMw
ggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC7AhUozPaglNMPEuyNVZLD
+ILxmaZ6QoinXSaqtSu5xUyxr45r+XXIo9cPR5QUVTVXjJ6oojkZ9YI8QqlObvU7
wy7bjcCwXPNZOOftz2nwWgsbvsCUJCWH+jdxsxPnHKzhm+/b5DtFUkWWqcFTzjTI
Uu61ru2P3mBw4qVUq7ZtDpelQDRrK9O8ZutmNHz6a4uPVymZ+DAXXbpyb/uBxa3S
hlg9F8fnCbvxK/eG3MHacV3URuPMrSXBiLxgZ3Vms/EY96Jc5lP/Ooi2R6X/ExjQ
oid0X9YRzP1TVhN/LLI9L0GR7PY6Kn7M6L4J+TNnJzHB2W2Ari3kNUMRVlvAgMB
AAGjggEIMIIBBDAOBgNVHQ8BAf8EBAMCAYYwHQYDVR0lBBYwFAYIKwYBBQUHAwIG
CCsGAQUFBwMBMBIGA1UdEwEB/wQIMAYBAf8CAQAwHQYDVR0OBBYEFBQusxe3WFbL
rlAJQOYfr52LFMLGMB8GA1UdIwQYMBaAFHm0WeZ7tuXkAXOACIjIGlj26Ztu
MDcGCCsGAQUFBwEBBCswKTAnBggrBgEFBQcwAoYbaHR0cDovL3gxLmkubGVuY3Iu
b3JnLzAiBgNVHR8EGzAZMBegFaAThidodHRwOi8veDEuYy5sZW5jci5vcmcvMCIG
A1UdIAQbMBkwCAYGZ4EMAQIBMA0GCysGAQQBgt8TAQEBMA0GCSqGSIb3DQEBCwUA
A4ICAQCFyk5HPqP3hUSFvNVneLKYY611TR6WPTNlclQtgaDqw+34IL9fzLdwALdu
o/NNJ/XRH4Y/SOkpZ+rA02b3AR6B+JA=
-----END CERTIFICATE-----
)CA";

/* ---- JSON escaping ---- */

/**
 * Escape a string for JSON embedding.
 * Handles: \ " \n \r \t and control chars.
 * Returns bytes written (not counting NUL), or -1 if buf too small.
 */
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
                if ((unsigned char)c < 0x20) {
                    /* Skip other control chars */
                    continue;
                }
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

/* ---- Simple JSON string value extractor ---- */

/**
 * Find a JSON string value by key in a flat or shallow JSON object.
 * Handles escaped quotes inside values.
 * Returns pointer to start of value (after opening quote), sets *out_len.
 * Returns nullptr if not found.
 */
static const char *json_find_string(const char *json, int json_len,
                                     const char *key, int *out_len) {
    /* Search for "key":" pattern */
    int klen = strlen(key);
    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || plen >= (int)sizeof(pattern)) return nullptr;

    const char *end = json + json_len;
    const char *p = json;

    while (p < end - plen) {
        const char *found = (const char *)memmem(p, end - p, pattern, plen);
        if (!found) return nullptr;

        /* Skip past key and colon */
        const char *after_key = found + plen;
        while (after_key < end && (*after_key == ' ' || *after_key == ':'))
            after_key++;

        if (after_key >= end || *after_key != '"') {
            p = after_key;
            continue;
        }

        /* Extract value between quotes, handling escapes */
        const char *val_start = after_key + 1;
        const char *q = val_start;
        while (q < end) {
            if (*q == '\\' && q + 1 < end) {
                q += 2; /* skip escaped char */
                continue;
            }
            if (*q == '"') break;
            q++;
        }
        *out_len = q - val_start;
        return val_start;
    }
    return nullptr;
}

/**
 * Find a JSON integer value by key.
 * Returns the integer value, or default_val if not found.
 */
static int json_find_int(const char *json, int json_len,
                          const char *key, int default_val) {
    int klen = strlen(key);
    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || plen >= (int)sizeof(pattern)) return default_val;

    const char *end = json + json_len;
    const char *found = (const char *)memmem(json, json_len, pattern, plen);
    if (!found) return default_val;

    const char *after_key = found + plen;
    while (after_key < end && (*after_key == ' ' || *after_key == ':'))
        after_key++;

    if (after_key >= end) return default_val;

    return atoi(after_key);
}

/**
 * Unescape a JSON string value in-place.
 * Converts \n, \t, \\, \", etc. Returns new length.
 */
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

/* ---- LlmClient implementation ---- */

LlmClient::LlmClient()
    : m_api_key(nullptr), m_model(nullptr) {
    m_error[0] = '\0';
}

void LlmClient::begin(const char *api_key, const char *model) {
    m_api_key = api_key;
    m_model = model;
    m_client.setCACert(ROOT_CA);
    m_client.setTimeout(LLM_READ_TIMEOUT_MS / 1000);
}

int LlmClient::buildRequest(char *buf, int buf_len,
                              const LlmMessage *messages, int count) {
    int w = 0;

    /* Opening */
    w += snprintf(buf + w, buf_len - w,
        "{\"model\":\"%s\",\"messages\":[", m_model);

    if (w >= buf_len) return -1;

    /* Messages array */
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            if (w + 1 >= buf_len) return -1;
            buf[w++] = ',';
        }

        w += snprintf(buf + w, buf_len - w,
            "{\"role\":\"%s\",\"content\":\"", messages[i].role);
        if (w >= buf_len) return -1;

        int esc_len = json_escape(buf + w, buf_len - w, messages[i].content);
        if (esc_len < 0) return -1;
        w += esc_len;

        /* Close content string and message object */
        w += snprintf(buf + w, buf_len - w, "\"}");
        if (w >= buf_len) return -1;
    }

    /* Close messages array and request object */
    w += snprintf(buf + w, buf_len - w,
        "],\"max_tokens\":2048,\"temperature\":0.7}");

    if (w >= buf_len) return -1;
    return w;
}

bool LlmClient::parseResponse(const char *body, int body_len, LlmResult *result) {
    result->ok = false;
    result->content[0] = '\0';
    result->content_len = 0;
    result->prompt_tokens = 0;
    result->completion_tokens = 0;

    /* Extract "content" field */
    int clen = 0;
    const char *content = json_find_string(body, body_len, "content", &clen);
    if (!content || clen <= 0) {
        /* Check for error message */
        int elen = 0;
        const char *errmsg = json_find_string(body, body_len, "message", &elen);
        if (errmsg && elen > 0) {
            int copy_len = elen < (int)sizeof(m_error) - 1 ? elen : (int)sizeof(m_error) - 1;
            memcpy(m_error, errmsg, copy_len);
            m_error[copy_len] = '\0';
        } else {
            snprintf(m_error, sizeof(m_error), "No content in response");
        }
        return false;
    }

    /* Copy and unescape content */
    int copy_len = clen < LLM_MAX_RESPONSE_LEN - 1 ? clen : LLM_MAX_RESPONSE_LEN - 1;
    memcpy(result->content, content, copy_len);
    result->content[copy_len] = '\0';
    result->content_len = json_unescape(result->content, copy_len);

    /* Extract token usage (optional) */
    result->prompt_tokens = json_find_int(body, body_len, "prompt_tokens", 0);
    result->completion_tokens = json_find_int(body, body_len, "completion_tokens", 0);

    result->ok = true;
    return true;
}

int LlmClient::readResponse(char *buf, int buf_len) {
    /*
     * Read HTTP response line by line.
     * Skip headers, find Content-Length or read until connection close.
     * Return body in buf, length as return value.
     */
    int content_length = -1;
    bool chunked = false;
    int http_status = 0;

    /* Read status line */
    String status_line = m_client.readStringUntil('\n');
    if (status_line.length() < 12) {
        snprintf(m_error, sizeof(m_error), "Invalid HTTP response");
        return -1;
    }
    http_status = status_line.substring(9, 12).toInt();

    /* Read headers */
    while (m_client.connected()) {
        String header = m_client.readStringUntil('\n');
        header.trim();
        if (header.length() == 0) break; /* End of headers */

        if (header.startsWith("Content-Length:") ||
            header.startsWith("content-length:")) {
            content_length = header.substring(15).toInt();
        }
        if (header.indexOf("chunked") >= 0) {
            chunked = true;
        }
    }

    /* Read body */
    int total = 0;

    if (chunked) {
        /* Chunked transfer encoding */
        while (m_client.connected() && total < buf_len - 1) {
            String chunk_size_str = m_client.readStringUntil('\n');
            chunk_size_str.trim();
            int chunk_size = strtol(chunk_size_str.c_str(), nullptr, 16);
            if (chunk_size <= 0) break;

            int to_read = chunk_size < (buf_len - 1 - total) ?
                          chunk_size : (buf_len - 1 - total);
            int rd = m_client.readBytes(buf + total, to_read);
            total += rd;

            /* Skip remaining chunk data if buffer full */
            int skip = chunk_size - to_read;
            while (skip > 0) {
                uint8_t tmp[256];
                int s = m_client.readBytes(tmp, skip < 256 ? skip : 256);
                if (s <= 0) break;
                skip -= s;
            }

            /* Read trailing \r\n after chunk */
            m_client.readStringUntil('\n');
        }
    } else if (content_length > 0) {
        int to_read = content_length < (buf_len - 1) ?
                      content_length : (buf_len - 1);
        total = m_client.readBytes(buf, to_read);
    } else {
        /* Read until connection closes or buffer full */
        unsigned long start = millis();
        while (m_client.connected() && total < buf_len - 1) {
            if (m_client.available()) {
                buf[total++] = m_client.read();
                start = millis();
            } else if (millis() - start > 5000) {
                break;
            } else {
                delay(10);
            }
        }
    }

    buf[total] = '\0';
    return total;
}

bool LlmClient::chat(const LlmMessage *messages, int count, LlmResult *result) {
    result->ok = false;
    result->content[0] = '\0';
    result->content_len = 0;
    result->http_status = 0;

    /* Build request JSON */
    static char request_buf[LLM_MAX_REQUEST_LEN];
    int req_len = buildRequest(request_buf, sizeof(request_buf), messages, count);
    if (req_len < 0) {
        snprintf(m_error, sizeof(m_error), "Request too large for buffer");
        return false;
    }

    Serial.printf("[LLM] Connecting to %s...\n", OPENROUTER_HOST);
    unsigned long t0 = millis();

    if (!m_client.connect(OPENROUTER_HOST, OPENROUTER_PORT)) {
        snprintf(m_error, sizeof(m_error), "TLS connect failed");
        return false;
    }

    Serial.printf("[LLM] Connected (%lums). Sending %d bytes...\n",
                  millis() - t0, req_len);

    /* Send HTTP request */
    m_client.printf("POST %s HTTP/1.1\r\n", OPENROUTER_PATH);
    m_client.printf("Host: %s\r\n", OPENROUTER_HOST);
    m_client.printf("Authorization: Bearer %s\r\n", m_api_key);
    m_client.printf("Content-Type: application/json\r\n");
    m_client.printf("Content-Length: %d\r\n", req_len);
    m_client.printf("Connection: close\r\n");
    m_client.printf("\r\n");
    m_client.write((uint8_t *)request_buf, req_len);

    Serial.printf("[LLM] Request sent. Waiting for response...\n");

    /* Wait for response */
    unsigned long wait_start = millis();
    while (!m_client.available()) {
        if (millis() - wait_start > LLM_READ_TIMEOUT_MS) {
            snprintf(m_error, sizeof(m_error), "Response timeout (%ds)",
                     LLM_READ_TIMEOUT_MS / 1000);
            m_client.stop();
            return false;
        }
        delay(50);
    }

    /* Read and parse response */
    static char response_buf[LLM_MAX_RESPONSE_LEN + 1024]; /* extra for JSON envelope */
    int body_len = readResponse(response_buf, sizeof(response_buf));

    m_client.stop();

    if (body_len <= 0) {
        snprintf(m_error, sizeof(m_error), "Empty response body");
        return false;
    }

    Serial.printf("[LLM] Response: %d bytes (%lums total)\n",
                  body_len, millis() - t0);

    return parseResponse(response_buf, body_len, result);
}
