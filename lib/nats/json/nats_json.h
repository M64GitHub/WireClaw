/**
 * @file nats_json.h
 * @brief Zero-allocation JSON utilities for NATS embedded clients
 *
 * Minimal JSON parsing and building for common IoT patterns.
 * No dynamic memory allocation - all buffers provided by caller.
 *
 * @author mario@synadia.com
 * @copyright Copyright (c) 2026 Synadia Communications Inc.
 * @license Apache-2.0
 */

#ifndef NATS_JSON_H
#define NATS_JSON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*============================================================================
 * JSON Value Types
 *============================================================================*/

typedef enum {
  NATS_JSON_INVALID = 0, /**< Parse error or key not found */
  NATS_JSON_NULL = 1,    /**< JSON null */
  NATS_JSON_BOOL = 2,    /**< true or false */
  NATS_JSON_INT = 3,     /**< Integer number */
  NATS_JSON_FLOAT = 4,   /**< Floating point number */
  NATS_JSON_STRING = 5,  /**< Quoted string */
  NATS_JSON_ARRAY = 6,   /**< Array (value points to '[') */
  NATS_JSON_OBJECT = 7   /**< Object (value points to '{') */
} nats_json_type_t;

/*============================================================================
 * JSON Parsing (Read)
 *============================================================================*/

/**
 * @brief Get a value by key from JSON object
 *
 * Finds a key in a JSON object and returns pointer to the value.
 * Does NOT handle nested objects - use on top-level keys only.
 *
 * @note The parser is deliberately lenient: it may accept some malformed JSON
 *       (e.g. unclosed strings, missing closing braces). This is by design for
 *       embedded use on trusted server input. Do not rely on this function to
 *       validate JSON structure.
 *
 * @param json      JSON string (null-terminated)
 * @param key       Key to find (without quotes)
 * @param value_out Output: pointer to value start (in json string)
 * @param len_out   Output: length of value (for strings, excludes quotes)
 * @return          Value type, or NATS_JSON_INVALID if not found
 */
nats_json_type_t nats_json_get(const char *json, const char *key,
                               const char **value_out, size_t *len_out);

/**
 * @brief Get integer value by key
 *
 * @param json        JSON string
 * @param key         Key to find
 * @param default_val Value to return if key not found or not an integer
 * @return            Integer value or default
 */
int32_t nats_json_get_int(const char *json, const char *key,
                          int32_t default_val);

/**
 * @brief Get unsigned integer value by key
 *
 * @param json        JSON string
 * @param key         Key to find
 * @param default_val Value to return if key not found
 * @return            Unsigned integer value or default
 */
uint32_t nats_json_get_uint(const char *json, const char *key,
                            uint32_t default_val);

/**
 * @brief Get float value by key
 *
 * @param json        JSON string
 * @param key         Key to find
 * @param default_val Value to return if key not found
 * @return            Float value or default
 */
float nats_json_get_float(const char *json, const char *key, float default_val);

/**
 * @brief Get boolean value by key
 *
 * @param json        JSON string
 * @param key         Key to find
 * @param default_val Value to return if key not found
 * @return            Boolean value or default
 */
bool nats_json_get_bool(const char *json, const char *key, bool default_val);

/**
 * @brief Get string value by key
 *
 * Copies the unquoted string value to the provided buffer.
 *
 * @param json    JSON string
 * @param key     Key to find
 * @param buf     Buffer to copy string into
 * @param buf_len Size of buffer
 * @return        Length of string copied (0 if not found), -1 if truncated
 */
int32_t nats_json_get_string(const char *json, const char *key, char *buf,
                             size_t buf_len);

/*============================================================================
 * JSON Building - Quick API (va_args)
 *============================================================================*/

/**
 * @brief Build JSON object using printf-like syntax
 *
 * Type suffixes in key:
 *   :i  - int32_t
 *   :u  - uint32_t
 *   :f  - float (1 decimal)
 *   :f2 - float (2 decimals)
 *   :f3 - float (3 decimals)
 *   :f6 - float (6 decimals)
 *   :s  - const char* (string)
 *   :b  - bool
 *   :n  - null (no value argument needed)
 *
 * @param buf       Output buffer
 * @param buf_len   Size of buffer
 * @param ...       Pairs of ("key:type", value), terminated by NULL
 * @return          Length written (excluding null), -1 on error
 *
 * @example
 *   nats_json_sprintf(buf, sizeof(buf),
 *       "temp:f", 23.5,
 *       "humidity:i", 65,
 *       "unit:s", "celsius",
 *       "online:b", true,
 *       NULL);
 *   // Result: {"temp":23.5,"humidity":65,"unit":"celsius","online":true}
 */
int32_t nats_json_sprintf(char *buf, size_t buf_len, ...);

/*============================================================================
 * JSON Building - Builder API (for complex/conditional structures)
 *============================================================================*/

/**
 * @brief JSON builder state
 */
typedef struct {
  char *buf;       /**< Output buffer */
  size_t capacity; /**< Buffer capacity */
  size_t len;      /**< Current length */
  bool error;      /**< Error flag (buffer overflow) */
  bool need_comma; /**< Need comma before next element */
  uint8_t depth;   /**< Nesting depth */
} nats_json_builder_t;

/**
 * @brief Initialize JSON builder
 *
 * @param b         Builder to initialize
 * @param buf       Output buffer
 * @param capacity  Buffer size
 */
void nats_json_build_init(nats_json_builder_t *b, char *buf, size_t capacity);

/**
 * @brief Start JSON object '{'
 */
void nats_json_build_object_start(nats_json_builder_t *b);

/**
 * @brief End JSON object '}'
 */
void nats_json_build_object_end(nats_json_builder_t *b);

/**
 * @brief Start JSON array '['
 */
void nats_json_build_array_start(nats_json_builder_t *b, const char *key);

/**
 * @brief End JSON array ']'
 */
void nats_json_build_array_end(nats_json_builder_t *b);

/**
 * @brief Add integer value
 */
void nats_json_build_int(nats_json_builder_t *b, const char *key,
                         int32_t value);

/**
 * @brief Add unsigned integer value
 */
void nats_json_build_uint(nats_json_builder_t *b, const char *key,
                          uint32_t value);

/**
 * @brief Add float value
 *
 * @param decimals  Number of decimal places (0-6)
 */
void nats_json_build_float(nats_json_builder_t *b, const char *key, float value,
                           uint8_t decimals);

/**
 * @brief Add boolean value
 */
void nats_json_build_bool(nats_json_builder_t *b, const char *key, bool value);

/**
 * @brief Add string value
 */
void nats_json_build_string(nats_json_builder_t *b, const char *key,
                            const char *value);

/**
 * @brief Add null value
 */
void nats_json_build_null(nats_json_builder_t *b, const char *key);

/**
 * @brief Add raw JSON value (no escaping)
 */
void nats_json_build_raw(nats_json_builder_t *b, const char *key,
                         const char *raw_json);

/**
 * @brief Finish building and get result
 *
 * @param b     Builder
 * @return      Null-terminated JSON string, or NULL on error
 */
const char *nats_json_build_finish(nats_json_builder_t *b);

/**
 * @brief Check if builder encountered an error
 */
bool nats_json_build_error(const nats_json_builder_t *b);

#ifdef __cplusplus
}
#endif

#endif /* NATS_JSON_H */
