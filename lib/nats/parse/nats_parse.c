/**
 * @file nats_parse.c
 * @brief Safe parsing and string utilities - Implementation
 *
 * @author mario@synadia.com
 * @copyright Copyright (c) 2026 Synadia Communications Inc.
 * @license Apache-2.0
 */

#include "nats_parse.h"
#include <limits.h>

/*============================================================================
 * Error Strings
 *============================================================================*/

static const char *const PARSE_ERR_STRINGS[] = {
    "OK", "Invalid argument", "Overflow", "Invalid format", "Truncated"};

const char *nats_parse_err_str(nats_parse_err_t err) {
  switch (err) {
  case NATS_PARSE_OK:
    return PARSE_ERR_STRINGS[0];
  case NATS_PARSE_ERR_INVALID_ARG:
    return PARSE_ERR_STRINGS[1];
  case NATS_PARSE_ERR_OVERFLOW:
    return PARSE_ERR_STRINGS[2];
  case NATS_PARSE_ERR_INVALID_FMT:
    return PARSE_ERR_STRINGS[3];
  case NATS_PARSE_ERR_TRUNCATED:
    return PARSE_ERR_STRINGS[4];
  default:
    return "Unknown error";
  }
}

/*============================================================================
 * Integer Parsing Implementation
 *============================================================================*/

nats_parse_err_t nats_parse_int(const char *str, size_t len, int32_t *out) {
  if ((str == NULL) || (out == NULL) || (len == 0U)) {
    return NATS_PARSE_ERR_INVALID_ARG;
  }

  const char *p = str;
  const char *end = str + len;

  /* Skip leading whitespace */
  while ((p < end) && (*p == ' ')) {
    p++;
  }

  if (p >= end) {
    return NATS_PARSE_ERR_INVALID_FMT;
  }

  /* Handle sign */
  bool negative = false;
  if (*p == '-') {
    negative = true;
    p++;
  } else if (*p == '+') {
    p++;
  }

  if (p >= end) {
    return NATS_PARSE_ERR_INVALID_FMT;
  }

  /* Must start with a digit */
  if ((*p < '0') || (*p > '9')) {
    return NATS_PARSE_ERR_INVALID_FMT;
  }

  /* Parse digits with overflow detection */
  int64_t result = 0;
  bool has_digit = false;

  while ((p < end) && (*p >= '0') && (*p <= '9')) {
    has_digit = true;
    int64_t digit = (int64_t)(*p - '0');

    /* Check for overflow before multiplying */
    if (result > (INT64_MAX / 10)) {
      return NATS_PARSE_ERR_OVERFLOW;
    }
    result = result * 10;

    /* Check for overflow before adding */
    if (result > INT64_MAX - digit) {
      return NATS_PARSE_ERR_OVERFLOW;
    }
    result += digit;

    p++;
  }

  if (!has_digit) {
    return NATS_PARSE_ERR_INVALID_FMT;
  }

  /* Apply sign and check range */
  if (negative) {
    result = -result;
    if (result < INT32_MIN) {
      return NATS_PARSE_ERR_OVERFLOW;
    }
  } else {
    if (result > INT32_MAX) {
      return NATS_PARSE_ERR_OVERFLOW;
    }
  }

  *out = (int32_t)result;
  return NATS_PARSE_OK;
}

nats_parse_err_t nats_parse_uint(const char *str, size_t len, uint32_t *out) {
  if ((str == NULL) || (out == NULL) || (len == 0U)) {
    return NATS_PARSE_ERR_INVALID_ARG;
  }

  const char *p = str;
  const char *end = str + len;

  /* Skip leading whitespace */
  while ((p < end) && (*p == ' ')) {
    p++;
  }

  if (p >= end) {
    return NATS_PARSE_ERR_INVALID_FMT;
  }

  /* Reject negative sign for unsigned */
  if (*p == '-') {
    return NATS_PARSE_ERR_OVERFLOW;
  }

  /* Allow optional plus sign */
  if (*p == '+') {
    p++;
  }

  if (p >= end) {
    return NATS_PARSE_ERR_INVALID_FMT;
  }

  /* Must start with a digit */
  if ((*p < '0') || (*p > '9')) {
    return NATS_PARSE_ERR_INVALID_FMT;
  }

  /* Parse digits with overflow detection */
  uint64_t result = 0U;
  const uint64_t max_before_mul = UINT32_MAX / 10U;
  bool has_digit = false;

  while ((p < end) && (*p >= '0') && (*p <= '9')) {
    has_digit = true;
    uint64_t digit = (uint64_t)(*p - '0');

    /* Check for overflow before multiplying */
    if (result > max_before_mul) {
      return NATS_PARSE_ERR_OVERFLOW;
    }
    result = result * 10U;

    /* Check for overflow before adding */
    if (result > UINT32_MAX - digit) {
      return NATS_PARSE_ERR_OVERFLOW;
    }
    result += digit;

    p++;
  }

  if (!has_digit) {
    return NATS_PARSE_ERR_INVALID_FMT;
  }

  *out = (uint32_t)result;
  return NATS_PARSE_OK;
}

nats_parse_err_t nats_parse_int_range(const char *str, size_t len, int32_t min,
                                      int32_t max, int32_t *out) {
  if (out == NULL) {
    return NATS_PARSE_ERR_INVALID_ARG;
  }

  /* Validate that min <= max to catch caller errors early */
  if (min > max) {
    return NATS_PARSE_ERR_INVALID_ARG;
  }

  int32_t value;
  nats_parse_err_t err = nats_parse_int(str, len, &value);
  if (err != NATS_PARSE_OK) {
    return err;
  }

  if ((value < min) || (value > max)) {
    return NATS_PARSE_ERR_OVERFLOW;
  }

  *out = value;
  return NATS_PARSE_OK;
}

nats_parse_err_t nats_parse_size(const char *str, size_t len, size_t *out) {
  if ((str == NULL) || (out == NULL) || (len == 0U)) {
    return NATS_PARSE_ERR_INVALID_ARG;
  }

  const char *p = str;
  const char *end = str + len;

  /* Skip leading whitespace */
  while ((p < end) && (*p == ' ')) {
    p++;
  }

  if (p >= end) {
    return NATS_PARSE_ERR_INVALID_FMT;
  }

  /* Reject negative sign */
  if (*p == '-') {
    return NATS_PARSE_ERR_OVERFLOW;
  }

  /* Allow optional plus sign */
  if (*p == '+') {
    p++;
  }

  if (p >= end) {
    return NATS_PARSE_ERR_INVALID_FMT;
  }

  /* Must start with a digit */
  if ((*p < '0') || (*p > '9')) {
    return NATS_PARSE_ERR_INVALID_FMT;
  }

  /* Parse digits with overflow detection */
  size_t result = 0U;
  const size_t max_before_mul = SIZE_MAX / 10U;
  bool has_digit = false;

  while ((p < end) && (*p >= '0') && (*p <= '9')) {
    has_digit = true;
    size_t digit = (size_t)(*p - '0');

    /* Check for overflow before multiplying */
    if (result > max_before_mul) {
      return NATS_PARSE_ERR_OVERFLOW;
    }
    result = result * 10U;

    /* Check for overflow before adding */
    if (result > SIZE_MAX - digit) {
      return NATS_PARSE_ERR_OVERFLOW;
    }
    result += digit;

    p++;
  }

  if (!has_digit) {
    return NATS_PARSE_ERR_INVALID_FMT;
  }

  *out = result;
  return NATS_PARSE_OK;
}

/*============================================================================
 * String Utilities Implementation
 *============================================================================*/

int32_t nats_safe_strcpy(char *dst, const char *src, size_t dst_size) {
  if ((dst == NULL) || (src == NULL) || (dst_size == 0U)) {
    return -1; /* Distinct error for invalid args (vs 0 for empty string) */
  }

  size_t i = 0U;
  while ((i < (dst_size - 1U)) && (src[i] != '\0')) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';

  /* Check if truncation occurred */
  if (src[i] != '\0') {
    /* Source had more data - truncated */
    return -((int32_t)i);
  }

  return (int32_t)i;
}

int32_t nats_safe_strcpy_n(char *dst, const char *src, size_t dst_size,
                           size_t src_max_len) {
  if ((dst == NULL) || (src == NULL) || (dst_size == 0U)) {
    return -1; /* Distinct error for invalid args (vs 0 for empty string) */
  }

  size_t max_copy =
      (dst_size - 1U) < src_max_len ? (dst_size - 1U) : src_max_len;
  size_t i = 0U;
  while ((i < max_copy) && (src[i] != '\0')) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';

  /* Check if truncation occurred due to dst_size limit */
  if ((i == (dst_size - 1U)) && (i < src_max_len) && (src[i] != '\0')) {
    return -((int32_t)i);
  }

  return (int32_t)i;
}

/*============================================================================
 * Buffer Scanning Utilities Implementation
 *============================================================================*/

int32_t nats_find_crlf(const uint8_t *buf, size_t len) {
  if ((buf == NULL) || (len < 2U)) {
    return -1;
  }

  for (size_t i = 1U; i < len; i++) {
    if ((buf[i - 1U] == '\r') && (buf[i] == '\n')) {
      /* Return position after \r\n */
      if ((i + 1U) > (size_t)INT32_MAX) {
        return -1; /* Position too large */
      }
      return (int32_t)(i + 1U);
    }
  }
  return -1;
}

const char *nats_skip_space(const char *p, const char *end) {
  if (p == NULL) {
    return NULL;
  }
  if (end == NULL) {
    return p;
  }

  while ((p < end) && (*p == ' ')) {
    p++;
  }
  return p;
}

const char *nats_find_token_end(const char *p, const char *end) {
  if (p == NULL) {
    return NULL;
  }
  if (end == NULL) {
    return p;
  }

  while ((p < end) && (*p != ' ') && (*p != '\0')) {
    p++;
  }
  return p;
}
