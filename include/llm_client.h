/**
 * @file llm_client.h
 * @brief OpenRouter LLM client for ESP32
 *
 * Sends chat completion requests to OpenRouter API over HTTPS.
 * Streams and parses JSON responses with minimal RAM usage.
 */

#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include <Arduino.h>
#include <WiFiClientSecure.h>

/* Global debug flag — toggled via /debug serial command */
extern bool g_debug;

/* Maximum sizes */
#define LLM_MAX_RESPONSE_LEN   4096  /* Max content we extract from response */
#define LLM_MAX_REQUEST_LEN    8192  /* Max JSON request body */
#define LLM_READ_TIMEOUT_MS    30000 /* 30s read timeout for LLM response */
#define LLM_MAX_MESSAGES       12    /* Max messages in conversation */

/* A single chat message */
struct LlmMessage {
    const char *role;     /* "system", "user", "assistant" */
    const char *content;
};

/* Result of an LLM call */
struct LlmResult {
    bool ok;
    char content[LLM_MAX_RESPONSE_LEN];
    int  content_len;
    int  http_status;
    int  prompt_tokens;
    int  completion_tokens;
};

class LlmClient {
public:
    LlmClient();

    /**
     * Initialize with API credentials.
     * Call once after WiFi is connected.
     */
    void begin(const char *api_key, const char *model);

    /**
     * Send a chat completion request.
     *
     * @param messages  Array of messages (system + conversation history)
     * @param count     Number of messages
     * @param result    Output: parsed response
     * @return true on success
     */
    bool chat(const LlmMessage *messages, int count, LlmResult *result);

    /** Get last error description */
    const char *lastError() const { return m_error; }

private:
    WiFiClientSecure m_client;
    const char *m_api_key;
    const char *m_model;
    char m_error[128];

    /* Build JSON request body into buf. Returns length or -1 on error. */
    int buildRequest(char *buf, int buf_len,
                     const LlmMessage *messages, int count);

    /* Parse response body to extract content. */
    bool parseResponse(const char *body, int body_len, LlmResult *result);

    /* Read HTTP response, skip headers, return body length. */
    int readResponse(char *buf, int buf_len);
};

#endif /* LLM_CLIENT_H */
