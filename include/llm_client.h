/**
 * @file llm_client.h
 * @brief OpenRouter LLM client for ESP32
 *
 * Sends chat completion requests to OpenRouter API over HTTPS.
 * Supports tool calling for the agentic loop.
 */

#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include <Arduino.h>
#include <WiFiClientSecure.h>

/* Global debug flag - toggled via /debug serial command */
extern bool g_debug;

/* Maximum sizes */
#define LLM_MAX_RESPONSE_LEN   4096  /* Max content we extract from response */
#define LLM_MAX_REQUEST_LEN    12288 /* Max JSON request body */
#define LLM_READ_TIMEOUT_MS    30000 /* 30s read timeout for LLM response */
#define LLM_MAX_MESSAGES       24    /* Max messages in conversation (more for tool loops) */
#define LLM_MAX_TOOL_CALLS     4     /* Max tool calls per LLM response */

/* A single tool call parsed from LLM response */
struct LlmToolCall {
    char id[64];           /* "call_abc123" */
    char name[32];         /* "led_set" */
    char arguments[512];   /* Raw JSON: "{\"r\":255,\"g\":0,\"b\":0}" */
};

/* Message types for the agentic loop */
#define LLM_MSG_NORMAL      0  /* Regular message: role + content */
#define LLM_MSG_TOOL_CALL   1  /* Assistant message with tool calls (content may be empty) */
#define LLM_MSG_TOOL_RESULT 2  /* Tool result: role=tool, has tool_call_id */

/* A single chat message - supports regular, tool-call, and tool-result types */
struct LlmMessage {
    int         type;           /* LLM_MSG_NORMAL, LLM_MSG_TOOL_CALL, LLM_MSG_TOOL_RESULT */
    const char *role;           /* "system", "user", "assistant", "tool" */
    const char *content;        /* Message content (may be nullptr for tool call msgs) */
    const char *tool_call_id;   /* For type=TOOL_RESULT: which call this answers */
    const char *tool_calls_json;/* For type=TOOL_CALL: raw JSON array of tool calls */
};

/* Result of an LLM call */
struct LlmResult {
    bool ok;
    char content[LLM_MAX_RESPONSE_LEN];
    int  content_len;
    int  http_status;
    int  prompt_tokens;
    int  completion_tokens;

    /* Tool calls (if any) */
    LlmToolCall tool_calls[LLM_MAX_TOOL_CALLS];
    int  tool_call_count;

    /* Raw tool_calls JSON for echoing back in next request */
    char tool_calls_json[1024];
};

class LlmClient {
public:
    LlmClient();

    void begin(const char *api_key, const char *model);

    /**
     * Send a chat completion request, optionally with tools.
     *
     * @param messages   Array of messages
     * @param count      Number of messages
     * @param tools_json Raw JSON string of tools array (or nullptr for no tools)
     * @param result     Output: parsed response with content and/or tool calls
     * @return true on success
     */
    bool chat(const LlmMessage *messages, int count,
              const char *tools_json, LlmResult *result);

    const char *lastError() const { return m_error; }

private:
    WiFiClientSecure m_client;
    const char *m_api_key;
    const char *m_model;
    char m_error[128];

    int buildRequest(char *buf, int buf_len,
                     const LlmMessage *messages, int count,
                     const char *tools_json);

    bool parseResponse(const char *body, int body_len, LlmResult *result);
    int readResponse(char *buf, int buf_len);

    /* Parse tool_calls array from response body */
    int parseToolCalls(const char *body, int body_len, LlmResult *result);
};

/* Helper to make a normal message */
inline LlmMessage llmMsg(const char *role, const char *content) {
    return {LLM_MSG_NORMAL, role, content, nullptr, nullptr};
}

/* Helper to make a tool result message */
inline LlmMessage llmToolResult(const char *tool_call_id, const char *content) {
    return {LLM_MSG_TOOL_RESULT, "tool", content, tool_call_id, nullptr};
}

/* Helper to make an assistant message with tool calls */
inline LlmMessage llmToolCallMsg(const char *content, const char *tool_calls_json) {
    return {LLM_MSG_TOOL_CALL, "assistant", content, nullptr, tool_calls_json};
}

#endif /* LLM_CLIENT_H */
