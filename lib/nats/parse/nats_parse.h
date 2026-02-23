/**
 * @file nats_parse.h
 * @brief Safe, MISRA-compliant parsing and string utilities
 *
 * This module provides zero-allocation, bounds-checked parsing functions
 * for use in embedded systems. All functions are MISRA C:2012 compliant.
 *
 * Design Principles:
 * - Zero allocation: All functions work with caller-provided buffers
 * - Explicit lengths: Never rely on null-termination for untrusted input
 * - Error returns: Parse failures return error codes, not sentinel values
 * - Output unchanged on error: Caller's output variable untouched if parsing
 * fails
 * - Bounded by design: All operations have explicit size limits
 *
 * @author Mario Schallner
 * @copyright Copyright (c) 2026 Mario Schallner
 */

#ifndef NATS_PARSE_H
#define NATS_PARSE_H

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Includes
 *============================================================================*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*============================================================================
 * Error Codes
 *============================================================================*/

/**
 * @brief Parse error codes
 *
 * These mirror the main NATS error codes for compatibility.
 * Use nats_parse_err_str() to get human-readable strings.
 */
typedef enum {
  NATS_PARSE_OK = 0, /**< Success */
  NATS_PARSE_ERR_INVALID_ARG =
      200,                       /**< Invalid argument (NULL ptr, zero len) */
  NATS_PARSE_ERR_OVERFLOW = 202, /**< Value exceeds type range */
  NATS_PARSE_ERR_INVALID_FMT = 102, /**< Invalid format (non-numeric, etc.) */
  NATS_PARSE_ERR_TRUNCATED = 201    /**< Output was truncated */
} nats_parse_err_t;

/*============================================================================
 * Integer Parsing Functions
 *============================================================================*/

/**
 * @brief Parse signed 32-bit integer from string
 *
 * Parses a decimal integer from the input string. Handles leading whitespace,
 * optional sign (+ or -), and stops at first non-digit character.
 *
 * @param str    Input string (not required to be null-terminated)
 * @param len    Maximum length of input to parse
 * @param out    Output value (unchanged on error)
 * @return       NATS_PARSE_OK on success, error code otherwise
 *
 * @note Replaces atoi()
 *
 * Example:
 * @code
 *   int32_t value;
 *   if (nats_parse_int("123", 3, &value) == NATS_PARSE_OK) {
 *       // value is 123
 *   }
 * @endcode
 */
nats_parse_err_t nats_parse_int(const char *str, size_t len, int32_t *out);

/**
 * @brief Parse unsigned 32-bit integer from string
 *
 * Parses a decimal unsigned integer from the input string. Handles leading
 * whitespace and stops at first non-digit character. Rejects negative signs.
 *
 * @param str    Input string (not required to be null-terminated)
 * @param len    Maximum length of input to parse
 * @param out    Output value (unchanged on error)
 * @return       NATS_PARSE_OK on success, error code otherwise
 */
nats_parse_err_t nats_parse_uint(const char *str, size_t len, uint32_t *out);

/**
 * @brief Parse signed integer with range validation
 *
 * Parses integer and validates it falls within [min, max] inclusive.
 *
 * @param str    Input string (not required to be null-terminated)
 * @param len    Maximum length of input to parse
 * @param min    Minimum allowed value (inclusive, must be <= max)
 * @param max    Maximum allowed value (inclusive, must be >= min)
 * @param out    Output value (unchanged on error)
 * @return       NATS_PARSE_OK on success, NATS_PARSE_ERR_INVALID_ARG if
 *               min > max, error code otherwise
 *
 * Example:
 * @code
 *   int32_t brightness;
 *   // Parse and validate 0-255 range
 *   if (nats_parse_int_range("128", 3, 0, 255, &brightness) == NATS_PARSE_OK) {
 *       // brightness is 128
 *   }
 * @endcode
 */
nats_parse_err_t nats_parse_int_range(const char *str, size_t len, int32_t min,
                                      int32_t max, int32_t *out);

/**
 * @brief Parse size_t from string with overflow detection
 *
 * Parses an unsigned integer suitable for sizes/lengths. Returns SIZE_MAX
 * as a saturating overflow indicator (check return code for real success).
 *
 * @param str    Input string (not required to be null-terminated)
 * @param len    Maximum length of input to parse
 * @param out    Output value (unchanged on error)
 * @return       NATS_PARSE_OK on success, error code otherwise
 *
 * @note This function is for internal use where size_t values are expected.
 *       For most user-facing parsing, use nats_parse_uint().
 */
nats_parse_err_t nats_parse_size(const char *str, size_t len, size_t *out);

/*============================================================================
 * String Utilities
 *============================================================================*/

/**
 * @brief Safe string copy with null termination
 *
 * Copies at most dst_size-1 characters from src to dst, always null-terminates.
 *
 * @param dst       Destination buffer
 * @param src       Source string (MUST be null-terminated)
 * @param dst_size  Size of destination buffer
 * @return          Number of chars copied (excluding null), negative if
 *                  truncated, or -1 if invalid args (NULL pointers, zero size)
 *
 * @warning src MUST be null-terminated. For untrusted input with known maximum
 *          length, use nats_safe_strcpy_n() instead to avoid reading past the
 *          intended buffer boundary.
 */
int32_t nats_safe_strcpy(char *dst, const char *src, size_t dst_size);

/**
 * @brief Safe bounded string copy with null termination
 *
 * Copies at most min(dst_size-1, src_max_len) characters, always
 * null-terminates. Use when source string may not be null-terminated or has
 * known max length.
 *
 * @param dst         Destination buffer
 * @param src         Source string (may not be null-terminated)
 * @param dst_size    Size of destination buffer
 * @param src_max_len Maximum chars to read from source
 * @return            Number of chars copied (excluding null), negative if
 *                    truncated, or -1 if invalid args (NULL pointers, zero size)
 */
int32_t nats_safe_strcpy_n(char *dst, const char *src, size_t dst_size,
                           size_t src_max_len);

/*============================================================================
 * Buffer Scanning Utilities
 *============================================================================*/

/**
 * @brief Find \r\n (CRLF) sequence in buffer
 *
 * Scans buffer for the first occurrence of \r\n.
 *
 * @param buf   Buffer to search
 * @param len   Length of buffer
 * @return      Position after \r\n (index of first byte after \n),
 *              or -1 if not found
 */
int32_t nats_find_crlf(const uint8_t *buf, size_t len);

/**
 * @brief Skip whitespace characters (bounded)
 *
 * Advances pointer past ASCII space characters (0x20).
 *
 * @param p     Current position
 * @param end   End of buffer (exclusive)
 * @return      Pointer to first non-space or end
 */
const char *nats_skip_space(const char *p, const char *end);

/**
 * @brief Find end of token (bounded)
 *
 * Finds the end of a token (space, null, or end of buffer).
 *
 * @param p     Current position
 * @param end   End of buffer (exclusive)
 * @return      Pointer to first space/null or end
 */
const char *nats_find_token_end(const char *p, const char *end);

/*============================================================================
 * Error String
 *============================================================================*/

/**
 * @brief Get human-readable error string
 *
 * @param err   Error code
 * @return      Static error description string
 */
const char *nats_parse_err_str(nats_parse_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* NATS_PARSE_H */
