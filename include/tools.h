/**
 * @file tools.h
 * @brief ESP32 tool definitions and handlers for LLM tool calling
 *
 * Defines the tools the LLM can call and their execution handlers.
 */

#ifndef TOOLS_H
#define TOOLS_H

#include <Arduino.h>

/* Max length of a tool result string */
#define TOOL_RESULT_MAX_LEN 512

/**
 * Get the JSON array of tool definitions for the LLM API.
 * Returns a static string - do not free.
 */
const char *toolsGetDefinitions();

/**
 * Execute a tool by name with JSON arguments.
 *
 * @param name       Tool name (e.g., "led_set")
 * @param args_json  JSON arguments string (e.g., "{\"r\":255,\"g\":0,\"b\":0}")
 * @param result     Output buffer for result string
 * @param result_len Size of result buffer
 * @return true if tool was found and executed
 */
bool toolExecute(const char *name, const char *args_json,
                  char *result, int result_len);

#endif /* TOOLS_H */
