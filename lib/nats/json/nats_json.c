/**
 * @file nats_json.c
 * @brief Zero-allocation JSON utilities implementation
 *
 * @author mario@synadia.com
 * @copyright Copyright (c) 2026 Synadia Communications Inc.
 * @license Apache-2.0
 */

#include "nats_json.h"
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*============================================================================
 * Internal Helpers
 *============================================================================*/

/**
 * @brief Skip whitespace with bounds checking
 * @param p   Current position
 * @param end End of buffer (exclusive)
 * @return Pointer to first non-whitespace or end if all whitespace
 */
static const char *skip_ws(const char *p, const char *end) {
  while ((p < end) &&
         ((*p == ' ') || (*p == '\t') || (*p == '\n') || (*p == '\r'))) {
    p++;
  }
  return p;
}

/**
 * @brief Find end of JSON value with bounds checking
 * @param p    Current position (for strings: past opening quote)
 * @param end  End of buffer (exclusive)
 * @param type Value type
 * @return Pointer to end of value
 */
static const char *find_value_end(const char *p, const char *end,
                                  nats_json_type_t type) {
  int depth = 0;
  bool in_string = false;
  bool escape = false;

  if (type == NATS_JSON_STRING) {
    /* Already past opening quote, find closing quote */
    while (p < end) {
      if (escape) {
        escape = false;
      } else if (*p == '\\') {
        escape = true;
      } else if (*p == '"') {
        return p;
      }
      p++;
    }
    return p;
  }

  /* For objects/arrays, track nesting */
  if ((type == NATS_JSON_OBJECT) || (type == NATS_JSON_ARRAY)) {
    char open = (type == NATS_JSON_OBJECT) ? '{' : '[';
    char close = (type == NATS_JSON_OBJECT) ? '}' : ']';
    depth = 1;

    p++; /* Skip opening bracket */
    while (p < end) {
      if (in_string) {
        if (escape) {
          escape = false;
        } else if (*p == '\\') {
          escape = true;
        } else if (*p == '"') {
          in_string = false;
        }
      } else {
        if (*p == '"') {
          in_string = true;
        } else if (*p == open) {
          depth++;
        } else if (*p == close) {
          depth--;
          if (depth == 0) {
            return p + 1;
          }
        }
      }
      p++;
    }
    return p;
  }

  /* For primitives (numbers, true, false, null), find delimiter */
  while (p < end) {
    if ((*p == ',') || (*p == '}') || (*p == ']') || (*p == ' ') ||
        (*p == '\t') || (*p == '\n') || (*p == '\r')) {
      return p;
    }
    p++;
  }
  return p;
}

/**
 * @brief Detect value type from first character with bounds checking
 * @param p   Current position
 * @param end End of buffer (exclusive)
 * @return Detected type
 */
static nats_json_type_t detect_type(const char *p, const char *end) {
  p = skip_ws(p, end);

  if (p >= end) {
    return NATS_JSON_INVALID;
  }

  switch (*p) {
  case '"':
    return NATS_JSON_STRING;
  case '{':
    return NATS_JSON_OBJECT;
  case '[':
    return NATS_JSON_ARRAY;
  case 't':
  case 'f':
    return NATS_JSON_BOOL;
  case 'n':
    return NATS_JSON_NULL;
  case '-':
  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9':
    /* Check if float (has '.' or 'e/E') */
    {
      const char *scan = p;
      while ((scan < end) &&
             ((*scan >= '0' && *scan <= '9') || *scan == '-' || *scan == '+')) {
        scan++;
      }
      if ((scan < end) &&
          ((*scan == '.') || (*scan == 'e') || (*scan == 'E'))) {
        return NATS_JSON_FLOAT;
      }
      return NATS_JSON_INT;
    }
  default:
    return NATS_JSON_INVALID;
  }
}

/**
 * @brief Simple atof implementation for embedded with scientific notation
 */
static float parse_float(const char *p, size_t len) {
  float result = 0.0f;
  float sign = 1.0f;
  float decimal = 0.0f;
  float decimal_place = 0.1f;
  bool in_decimal = false;
  size_t i = 0;

  /* Handle sign */
  if ((i < len) && (p[i] == '-')) {
    sign = -1.0f;
    i++;
  } else if ((i < len) && (p[i] == '+')) {
    i++;
  }

  /* Parse mantissa (integer and decimal parts) */
  while (i < len) {
    char c = p[i];
    if ((c >= '0') && (c <= '9')) {
      if (in_decimal) {
        decimal += (float)(c - '0') * decimal_place;
        decimal_place *= 0.1f;
      } else {
        result = result * 10.0f + (float)(c - '0');
      }
    } else if (c == '.') {
      in_decimal = true;
    } else if ((c == 'e') || (c == 'E')) {
      /* Scientific notation: parse exponent */
      i++;
      int32_t exp_sign = 1;
      int32_t exponent = 0;

      /* Handle exponent sign */
      if ((i < len) && (p[i] == '-')) {
        exp_sign = -1;
        i++;
      } else if ((i < len) && (p[i] == '+')) {
        i++;
      }

      /* Parse exponent digits */
      while ((i < len) && (p[i] >= '0') && (p[i] <= '9')) {
        exponent = exponent * 10 + (p[i] - '0');
        i++;
      }

      /* Apply exponent: multiply or divide by 10^exponent */
      float mantissa = result + decimal;
      float multiplier = 1.0f;
      for (int32_t e = 0; e < exponent; e++) {
        multiplier *= 10.0f;
      }

      if (exp_sign < 0) {
        return sign * mantissa / multiplier;
      } else {
        return sign * mantissa * multiplier;
      }
    } else {
      break;
    }
    i++;
  }

  return sign * (result + decimal);
}

/*============================================================================
 * JSON Parsing Implementation
 *============================================================================*/

nats_json_type_t nats_json_get(const char *json, const char *key,
                               const char **value_out, size_t *len_out) {
  if ((json == NULL) || (key == NULL)) {
    return NATS_JSON_INVALID;
  }

  size_t key_len = strlen(key);
  size_t json_len = strlen(json);
  const char *p = json;
  const char *json_end = json + json_len;

  /* Find opening brace */
  p = skip_ws(p, json_end);
  if ((p >= json_end) || (*p != '{')) {
    return NATS_JSON_INVALID;
  }
  p++;

  /* Search for key */
  while (p < json_end) {
    p = skip_ws(p, json_end);

    if (p >= json_end) {
      break;
    }

    if (*p == '}') {
      break; /* End of object */
    }

    if (*p == ',') {
      p++;
      continue;
    }

    /* Expect quoted key */
    if (*p != '"') {
      return NATS_JSON_INVALID;
    }
    p++;

    /* Bounds check for key comparison */
    if ((size_t)(json_end - p) < key_len) {
      return NATS_JSON_INVALID; /* Not enough bytes for key */
    }

    /* Check if this is our key */
    if ((strncmp(p, key, key_len) == 0) && ((p + key_len) < json_end) &&
        (p[key_len] == '"')) {
      /* Found key, skip to value */
      p += key_len + 1;
      p = skip_ws(p, json_end);

      if ((p >= json_end) || (*p != ':')) {
        return NATS_JSON_INVALID;
      }
      p++;
      p = skip_ws(p, json_end);

      if (p >= json_end) {
        return NATS_JSON_INVALID;
      }

      /* Detect value type */
      nats_json_type_t type = detect_type(p, json_end);
      if (type == NATS_JSON_INVALID) {
        return NATS_JSON_INVALID;
      }

      /* Set output pointers */
      if (type == NATS_JSON_STRING) {
        /* Skip opening quote */
        p++;
        if (value_out != NULL) {
          *value_out = p;
        }
        const char *val_end = find_value_end(p, json_end, type);
        if (len_out != NULL) {
          *len_out = (size_t)(val_end - p);
        }
      } else {
        if (value_out != NULL) {
          *value_out = p;
        }
        const char *val_end = find_value_end(p, json_end, type);
        if (len_out != NULL) {
          *len_out = (size_t)(val_end - p);
        }
      }

      return type;
    }

    /* Not our key, skip to end of key string */
    while ((p < json_end) && (*p != '"')) {
      if (*p == '\\') {
        p++; /* Skip escape */
      }
      if (p < json_end) {
        p++;
      }
    }
    if ((p < json_end) && (*p == '"')) {
      p++;
    }

    /* Skip colon and value */
    p = skip_ws(p, json_end);
    if ((p >= json_end) || (*p != ':')) {
      return NATS_JSON_INVALID;
    }
    p++;
    p = skip_ws(p, json_end);

    if (p >= json_end) {
      return NATS_JSON_INVALID;
    }

    /* Skip value */
    nats_json_type_t skip_type = detect_type(p, json_end);
    if (skip_type == NATS_JSON_STRING) {
      p++; /* Skip opening quote */
    }
    p = find_value_end(p, json_end, skip_type);
    if ((skip_type == NATS_JSON_STRING) && (p < json_end) && (*p == '"')) {
      p++; /* Skip closing quote */
    }
  }

  return NATS_JSON_INVALID; /* Key not found */
}

int32_t nats_json_get_int(const char *json, const char *key,
                          int32_t default_val) {
  const char *value;
  size_t len;

  nats_json_type_t type = nats_json_get(json, key, &value, &len);
  if ((type != NATS_JSON_INT) && (type != NATS_JSON_FLOAT)) {
    return default_val;
  }

  /* Parse integer with overflow detection using wider type */
  int64_t result = 0;
  bool negative = false;
  size_t i = 0;

  if ((i < len) && (value[i] == '-')) {
    negative = true;
    i++;
  }

  while ((i < len) && (value[i] >= '0') && (value[i] <= '9')) {
    result = result * 10 + (value[i] - '0');

    /* Check for overflow: INT32_MAX is 2147483647, INT32_MIN is -2147483648 */
    if (!negative) {
      if (result > (int64_t)INT32_MAX) {
        return default_val; /* Positive overflow */
      }
    } else {
      /* For negative, allow up to INT32_MAX + 1 (which is -INT32_MIN) */
      if (result > (int64_t)INT32_MAX + 1) {
        return default_val; /* Negative overflow */
      }
    }
    i++;
  }

  /* Handle INT32_MIN as special case to avoid signed overflow */
  if (negative) {
    if (result == (int64_t)INT32_MAX + 1) {
      return INT32_MIN; /* Exact minimum value */
    }
    return -(int32_t)result;
  }
  return (int32_t)result;
}

uint32_t nats_json_get_uint(const char *json, const char *key,
                            uint32_t default_val) {
  const char *value;
  size_t len;

  nats_json_type_t type = nats_json_get(json, key, &value, &len);
  if ((type != NATS_JSON_INT) && (type != NATS_JSON_FLOAT)) {
    return default_val;
  }

  /* Parse unsigned integer with overflow detection */
  uint64_t result = 0;
  size_t i = 0;

  /* Negative numbers are invalid for unsigned */
  if ((i < len) && (value[i] == '-')) {
    return default_val;
  }

  while ((i < len) && (value[i] >= '0') && (value[i] <= '9')) {
    result = result * 10U + (uint64_t)(value[i] - '0');

    /* Check for overflow: UINT32_MAX is 4294967295 */
    if (result > (uint64_t)UINT32_MAX) {
      return default_val; /* Overflow */
    }
    i++;
  }

  return (uint32_t)result;
}

float nats_json_get_float(const char *json, const char *key,
                          float default_val) {
  const char *value;
  size_t len;

  nats_json_type_t type = nats_json_get(json, key, &value, &len);
  if ((type != NATS_JSON_INT) && (type != NATS_JSON_FLOAT)) {
    return default_val;
  }

  return parse_float(value, len);
}

bool nats_json_get_bool(const char *json, const char *key, bool default_val) {
  const char *value;
  size_t len;

  nats_json_type_t type = nats_json_get(json, key, &value, &len);
  if (type != NATS_JSON_BOOL) {
    return default_val;
  }

  return (value[0] == 't');
}

int32_t nats_json_get_string(const char *json, const char *key, char *buf,
                             size_t buf_len) {
  if ((buf == NULL) || (buf_len == 0)) {
    return -1;
  }

  const char *value;
  size_t len;

  nats_json_type_t type = nats_json_get(json, key, &value, &len);
  if (type != NATS_JSON_STRING) {
    buf[0] = '\0';
    return 0;
  }

  /* Copy string, handling escapes */
  size_t out = 0;
  size_t i = 0;
  bool truncated = false;

  while ((i < len) && (out < buf_len - 1)) {
    if ((value[i] == '\\') && ((i + 1) < len)) {
      i++;
      switch (value[i]) {
      case 'n':
        buf[out++] = '\n';
        break;
      case 'r':
        buf[out++] = '\r';
        break;
      case 't':
        buf[out++] = '\t';
        break;
      case 'b':
        buf[out++] = '\b'; /* Backspace */
        break;
      case 'f':
        buf[out++] = '\f'; /* Form feed */
        break;
      case '/':
        buf[out++] = '/'; /* Forward slash (optional escape in JSON) */
        break;
      case '"':
        buf[out++] = '"';
        break;
      case '\\':
        buf[out++] = '\\';
        break;
      case 'u':
        /* Unicode escape: \uXXXX
         * For embedded systems, we emit a replacement character
         * Full UTF-8 encoding would require more complex handling
         */
        if ((i + 4) < len) {
          buf[out++] = '?'; /* Replacement for unicode */
          i += 4;           /* Skip the 4 hex digits */
        } else {
          buf[out++] = value[i]; /* Incomplete escape */
        }
        break;
      default:
        buf[out++] = value[i];
        break;
      }
    } else {
      buf[out++] = value[i];
    }
    i++;
  }

  buf[out] = '\0';

  if (i < len) {
    truncated = true;
  }

  /* Bounds check before narrowing cast */
  if (out > (size_t)INT32_MAX) {
    return -1; /* Output too large */
  }

  return truncated ? -1 : (int32_t)out;
}

/*============================================================================
 * JSON String Escaping Helpers (RFC 8259 Section 7)
 *============================================================================*/

/** Hex digits for \u00XX escapes */
static const char json_hex_digits[] = "0123456789abcdef";

/**
 * @brief Calculate escaped length of a string (including surrounding quotes)
 * @param str  Source string
 * @param len  Source length
 * @return Total bytes needed for quoted+escaped string
 */
static size_t json_escaped_len(const char *str, size_t len) {
  size_t escaped = 2U; /* Opening and closing quotes */
  for (size_t i = 0U; i < len; i++) {
    unsigned char c = (unsigned char)str[i];
    if ((c == (unsigned char)'"') || (c == (unsigned char)'\\')) {
      escaped += 2U;
    } else if (c < 0x20U) {
      /* Control characters: named escapes are 2 bytes, others are 6 (\u00XX) */
      if ((c == (unsigned char)'\n') || (c == (unsigned char)'\r') ||
          (c == (unsigned char)'\t') || (c == (unsigned char)'\b') ||
          (c == (unsigned char)'\f')) {
        escaped += 2U;
      } else {
        escaped += 6U; /* \u00XX */
      }
    } else {
      escaped += 1U;
    }
  }
  return escaped;
}

/**
 * @brief Write escaped JSON string with surrounding quotes
 * @param dst  Destination buffer (caller must ensure sufficient space)
 * @param src  Source string
 * @param len  Source length
 * @return Number of bytes written to dst
 *
 * @note Caller MUST verify buffer has json_escaped_len() bytes available
 */
static size_t json_escape_write(char *dst, const char *src, size_t len) {
  size_t pos = 0U;
  dst[pos] = '"';
  pos++;

  for (size_t i = 0U; i < len; i++) {
    unsigned char c = (unsigned char)src[i];

    if (c == (unsigned char)'"') {
      dst[pos] = '\\';
      pos++;
      dst[pos] = '"';
      pos++;
    } else if (c == (unsigned char)'\\') {
      dst[pos] = '\\';
      pos++;
      dst[pos] = '\\';
      pos++;
    } else if (c < 0x20U) {
      /* Control character escaping per RFC 8259 */
      dst[pos] = '\\';
      pos++;
      switch (c) {
      case (unsigned char)'\n':
        dst[pos] = 'n';
        pos++;
        break;
      case (unsigned char)'\r':
        dst[pos] = 'r';
        pos++;
        break;
      case (unsigned char)'\t':
        dst[pos] = 't';
        pos++;
        break;
      case (unsigned char)'\b':
        dst[pos] = 'b';
        pos++;
        break;
      case (unsigned char)'\f':
        dst[pos] = 'f';
        pos++;
        break;
      default:
        /* \u00XX for remaining control chars (0x00-0x07, 0x0E-0x1F, etc.) */
        dst[pos] = 'u';
        pos++;
        dst[pos] = '0';
        pos++;
        dst[pos] = '0';
        pos++;
        dst[pos] = json_hex_digits[(c >> 4U) & 0x0FU];
        pos++;
        dst[pos] = json_hex_digits[c & 0x0FU];
        pos++;
        break;
      }
    } else {
      dst[pos] = (char)c;
      pos++;
    }
  }

  dst[pos] = '"';
  pos++;
  return pos;
}

/*============================================================================
 * JSON Building - Quick API Implementation
 *============================================================================*/

int32_t nats_json_sprintf(char *buf, size_t buf_len, ...) {
  if ((buf == NULL) || (buf_len < 2)) {
    return -1;
  }

  va_list args;
  va_start(args, buf_len);

  size_t pos = 0;
  bool first = true;

  /* Opening brace */
  buf[pos++] = '{';

  /* Process key-value pairs */
  while (1) {
    const char *key_spec = va_arg(args, const char *);
    if (key_spec == NULL) {
      break;
    }

    /* Find type suffix */
    const char *colon = strchr(key_spec, ':');
    if (colon == NULL) {
      va_end(args);
      return -1;
    }

    size_t key_len = (size_t)(colon - key_spec);
    const char *type_str = colon + 1;

    /* Add comma if not first */
    if (!first) {
      if (pos >= buf_len - 1) {
        va_end(args);
        return -1;
      }
      buf[pos++] = ',';
    }
    first = false;

    /* Add key */
    if (pos + key_len + 3 >= buf_len) {
      va_end(args);
      return -1;
    }
    buf[pos++] = '"';
    memcpy(&buf[pos], key_spec, key_len);
    pos += key_len;
    buf[pos++] = '"';
    buf[pos++] = ':';

    /* Add value based on type */
    int written = 0;

    if (type_str[0] == 'i') {
      /* Integer */
      int32_t val = va_arg(args, int32_t);
      written = snprintf(&buf[pos], buf_len - pos, "%ld", (long)val);
    } else if (type_str[0] == 'u') {
      /* Unsigned integer */
      uint32_t val = va_arg(args, uint32_t);
      written = snprintf(&buf[pos], buf_len - pos, "%lu", (unsigned long)val);
    } else if (type_str[0] == 'f') {
      /* Float with precision */
      double val =
          va_arg(args, double); /* floats promoted to double in va_arg */
      int decimals = 1;         /* Default precision */

      if (type_str[1] >= '0' && type_str[1] <= '9') {
        decimals = type_str[1] - '0';
      }

      written = snprintf(&buf[pos], buf_len - pos, "%.*f", decimals, val);
    } else if (type_str[0] == 's') {
      /* String */
      const char *val = va_arg(args, const char *);
      if (val == NULL) {
        written = snprintf(&buf[pos], buf_len - pos, "null");
      } else {
        /* Pre-calculate escaped length to fail fast (RFC 8259) */
        size_t val_len = strlen(val);
        size_t escaped_len = json_escaped_len(val, val_len);
        if ((pos + escaped_len) >= buf_len) {
          va_end(args);
          return -1; /* Insufficient buffer for escaped string */
        }

        /* Write quoted+escaped string - bounds verified above */
        pos += json_escape_write(&buf[pos], val, val_len);
        written = 0; /* Already handled */
      }
    } else if (type_str[0] == 'b') {
      /* Boolean */
      int val = va_arg(args, int); /* bool promoted to int */
      written =
          snprintf(&buf[pos], buf_len - pos, "%s", val ? "true" : "false");
    } else if (type_str[0] == 'n') {
      /* Null (no value argument) */
      written = snprintf(&buf[pos], buf_len - pos, "null");
    } else {
      va_end(args);
      return -1;
    }

    /* Check snprintf error first (negative), then check truncation */
    if (written < 0) {
      va_end(args);
      return -1;
    }
    if ((size_t)written >= (buf_len - pos)) {
      va_end(args);
      return -1; /* Output truncated */
    }
    pos += (size_t)written;
  }

  va_end(args);

  /* Closing brace */
  if (pos >= buf_len - 1) {
    return -1;
  }
  buf[pos++] = '}';
  buf[pos] = '\0';

  /* Bounds check before narrowing cast */
  if (pos > (size_t)INT32_MAX) {
    return -1; /* Output too large */
  }

  return (int32_t)pos;
}

/*============================================================================
 * JSON Building - Builder API Implementation
 *============================================================================*/

static void builder_append(nats_json_builder_t *b, const char *str,
                           size_t len) {
  if (b->error) {
    return;
  }

  /* Reordered to avoid potential overflow: len >= (capacity - len) */
  if (len >= (b->capacity - b->len)) {
    b->error = true;
    return;
  }

  memcpy(&b->buf[b->len], str, len);
  b->len += len;
}

static void builder_append_char(nats_json_builder_t *b, char c) {
  if (b->error) {
    return;
  }

  if (b->len + 1 >= b->capacity) {
    b->error = true;
    return;
  }

  b->buf[b->len++] = c;
}

static void builder_add_key(nats_json_builder_t *b, const char *key) {
  if (b->need_comma) {
    builder_append_char(b, ',');
  }
  b->need_comma = true;

  if (key != NULL) {
    builder_append_char(b, '"');
    builder_append(b, key, strlen(key));
    builder_append_char(b, '"');
    builder_append_char(b, ':');
  }
}

void nats_json_build_init(nats_json_builder_t *b, char *buf, size_t capacity) {
  if ((b == NULL) || (buf == NULL)) {
    return;
  }

  b->buf = buf;
  b->capacity = capacity;
  b->len = 0;
  b->error = false;
  b->need_comma = false;
  b->depth = 0;
}

void nats_json_build_object_start(nats_json_builder_t *b) {
  if (b == NULL) {
    return;
  }

  builder_append_char(b, '{');
  b->need_comma = false;
  b->depth++;
}

void nats_json_build_object_end(nats_json_builder_t *b) {
  if (b == NULL) {
    return;
  }

  builder_append_char(b, '}');
  b->need_comma = true;
  if (b->depth > 0) {
    b->depth--;
  }
}

void nats_json_build_array_start(nats_json_builder_t *b, const char *key) {
  if (b == NULL) {
    return;
  }

  builder_add_key(b, key);
  builder_append_char(b, '[');
  b->need_comma = false;
  b->depth++;
}

void nats_json_build_array_end(nats_json_builder_t *b) {
  if (b == NULL) {
    return;
  }

  builder_append_char(b, ']');
  b->need_comma = true;
  if (b->depth > 0) {
    b->depth--;
  }
}

void nats_json_build_int(nats_json_builder_t *b, const char *key,
                         int32_t value) {
  if (b == NULL) {
    return;
  }

  builder_add_key(b, key);

  char num[16];
  int len = snprintf(num, sizeof(num), "%ld", (long)value);
  if ((len < 0) || ((size_t)len >= sizeof(num))) {
    b->error = true;
  } else {
    builder_append(b, num, (size_t)len);
  }
}

void nats_json_build_uint(nats_json_builder_t *b, const char *key,
                          uint32_t value) {
  if (b == NULL) {
    return;
  }

  builder_add_key(b, key);

  char num[16];
  int len = snprintf(num, sizeof(num), "%lu", (unsigned long)value);
  if ((len < 0) || ((size_t)len >= sizeof(num))) {
    b->error = true;
  } else {
    builder_append(b, num, (size_t)len);
  }
}

void nats_json_build_float(nats_json_builder_t *b, const char *key, float value,
                           uint8_t decimals) {
  if (b == NULL) {
    return;
  }

  builder_add_key(b, key);

  if (decimals > 6) {
    decimals = 6;
  }

  char num[32];
  int len = snprintf(num, sizeof(num), "%.*f", decimals, (double)value);
  if ((len < 0) || ((size_t)len >= sizeof(num))) {
    b->error = true;
  } else {
    builder_append(b, num, (size_t)len);
  }
}

void nats_json_build_bool(nats_json_builder_t *b, const char *key, bool value) {
  if (b == NULL) {
    return;
  }

  builder_add_key(b, key);

  if (value) {
    builder_append(b, "true", 4);
  } else {
    builder_append(b, "false", 5);
  }
}

void nats_json_build_string(nats_json_builder_t *b, const char *key,
                            const char *value) {
  if (b == NULL) {
    return;
  }

  builder_add_key(b, key);

  if (value == NULL) {
    builder_append(b, "null", 4);
    return;
  }

  /* Pre-calculate escaped length to fail fast (RFC 8259) */
  size_t val_len = strlen(value);
  size_t escaped_len = json_escaped_len(value, val_len);
  if ((b->len + escaped_len) >= b->capacity) {
    b->error = true;
    return; /* Insufficient buffer for escaped string */
  }

  /* Write quoted+escaped string - bounds verified above */
  b->len += json_escape_write(&b->buf[b->len], value, val_len);
}

void nats_json_build_null(nats_json_builder_t *b, const char *key) {
  if (b == NULL) {
    return;
  }

  builder_add_key(b, key);
  builder_append(b, "null", 4);
}

void nats_json_build_raw(nats_json_builder_t *b, const char *key,
                         const char *raw_json) {
  if (b == NULL) {
    return;
  }

  builder_add_key(b, key);

  if (raw_json != NULL) {
    builder_append(b, raw_json, strlen(raw_json));
  } else {
    builder_append(b, "null", 4);
  }
}

const char *nats_json_build_finish(nats_json_builder_t *b) {
  if ((b == NULL) || b->error) {
    return NULL;
  }

  if (b->len >= b->capacity) {
    b->error = true;
    return NULL;
  }

  b->buf[b->len] = '\0';
  return b->buf;
}

bool nats_json_build_error(const nats_json_builder_t *b) {
  if (b == NULL) {
    return true;
  }
  return b->error;
}
