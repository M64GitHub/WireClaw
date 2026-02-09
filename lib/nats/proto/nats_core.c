/**
 * @file nats_core.c
 * @brief NATS Embedded Client - Core Implementation
 *
 * Platform-independent NATS protocol implementation.
 *
 * @author mario@synadia.com
 * @copyright Copyright (c) 2026 Synadia Communications Inc.
 * @license Apache-2.0
 */

#include "nats_core.h"
#include "../parse/nats_parse.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*============================================================================
 * Version String
 *============================================================================*/

static const char NATS_VERSION_STR[] = "0.1.0";

/*============================================================================
 * Error Strings
 *============================================================================*/

static const char *const NATS_ERR_STRINGS[] = {
    [NATS_OK] = "OK",
    [NATS_ERR_WOULD_BLOCK] = "Would block",
    [NATS_ERR_TIMEOUT] = "Timeout",
    [NATS_ERR_IO] = "I/O error",
    [NATS_ERR_NOT_CONNECTED] = "Not connected",
    [NATS_ERR_CONNECTION_LOST] = "Connection lost",
    [NATS_ERR_PROTOCOL] = "Protocol error",
    [NATS_ERR_SERVER] = "Server error",
    [NATS_ERR_STALE_CONNECTION] = "Stale connection",
    [NATS_ERR_INVALID_ARG] = "Invalid argument",
    [NATS_ERR_BUFFER_FULL] = "Buffer full",
    [NATS_ERR_BUFFER_OVERFLOW] = "Buffer overflow",
    [NATS_ERR_NO_MEMORY] = "No memory",
    [NATS_ERR_INVALID_STATE] = "Invalid state",
    [NATS_ERR_AUTH_FAILED] = "Auth failed",
    [NATS_ERR_NOT_FOUND] = "Not found",
};

static const char *const NATS_STATE_STRINGS[] = {
    [NATS_STATE_DISCONNECTED] = "DISCONNECTED",
    [NATS_STATE_CONNECTING] = "CONNECTING",
    [NATS_STATE_WAIT_INFO] = "WAIT_INFO",
    [NATS_STATE_SEND_CONNECT] = "SEND_CONNECT",
    [NATS_STATE_CONNECTED] = "CONNECTED",
    [NATS_STATE_RECONNECTING] = "RECONNECTING",
    [NATS_STATE_DRAINING] = "DRAINING",
    [NATS_STATE_CLOSED] = "CLOSED",
};

/*============================================================================
 * Internal Helpers (wrappers around nats_parse module)
 *============================================================================*/

/**
 * @brief Safe string copy wrapper for internal use
 * @note Wraps nats_safe_strcpy, returning size_t for backward compatibility
 */
static size_t safe_strcpy(char *dst, const char *src, size_t dst_size) {
  int32_t ret = nats_safe_strcpy(dst, src, dst_size);
  /* Return absolute value - truncation detection handled at call sites if
   * needed */
  return (ret >= 0) ? (size_t)ret : (size_t)(-ret);
}

/**
 * @brief Parse unsigned integer from string with overflow detection
 * @return Parsed value, or SIZE_MAX on overflow
 * @note Wraps nats_parse_size, returning SIZE_MAX on error for backward compat
 */
static size_t parse_uint(const char *str, size_t len) {
  if ((str == NULL) || (len == 0U)) {
    return 0U;
  }

  size_t result = 0U;
  nats_parse_err_t err = nats_parse_size(str, len, &result);
  if (err != NATS_PARSE_OK) {
    return SIZE_MAX; /* Saturate on overflow/error */
  }
  return result;
}

/**
 * @brief Compact receive buffer by removing consumed bytes
 */
static void compact_rx_buf(nats_client_t *client, size_t consumed) {
  NATS_ASSERT(client != NULL);
  NATS_ASSERT(consumed <= client->rx_len);

  size_t remaining = client->rx_len - consumed;
  if ((remaining > 0U) && (consumed > 0U)) {
    memmove(client->rx_buf, &client->rx_buf[consumed], remaining);
  }
  client->rx_len = remaining;
}

/**
 * @brief Send data through transport
 */
static nats_err_t send_data(nats_client_t *client, const uint8_t *data,
                            size_t len) {
  NATS_ASSERT(client != NULL);
  NATS_ASSERT(client->transport.send != NULL);

  if ((data == NULL) || (len == 0U)) {
    return NATS_OK;
  }

  size_t sent = 0U;
  while (sent < len) {
    int32_t n =
        client->transport.send(client->transport.ctx, &data[sent], len - sent);

    if (n < 0) {
      return NATS_ERR_IO;
    }
    if (n == 0) {
      return NATS_ERR_WOULD_BLOCK;
    }

    sent += (size_t)n;
  }

  client->stats.bytes_out += (uint32_t)len;
  client->last_activity = client->time_fn();

  return NATS_OK;
}

/**
 * @brief Send a protocol line (adds \r\n)
 */
static nats_err_t send_line(nats_client_t *client, const char *line) {
  nats_err_t err;

  err = send_data(client, (const uint8_t *)line, strlen(line));
  if (err != NATS_OK) {
    return err;
  }

  err = send_data(client, (const uint8_t *)"\r\n", 2U);
  return err;
}

/**
 * @brief Send formatted line using tx_buf
 */
static nats_err_t send_linef(nats_client_t *client, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  int len =
      vsnprintf((char *)client->tx_buf, sizeof(client->tx_buf), fmt, args);
  va_end(args);

  if ((len < 0) || ((size_t)len >= sizeof(client->tx_buf))) {
    return NATS_ERR_BUFFER_OVERFLOW;
  }

  return send_line(client, (const char *)client->tx_buf);
}

/*============================================================================
 * Protocol Handlers
 *============================================================================*/

/**
 * @brief Handle INFO message from server
 */
static nats_err_t handle_info(nats_client_t *client, const char *json) {
  (void)json; /* TODO: Parse JSON for server capabilities */

  /* For now, just note that we received INFO */
  client->server_info.proto = 1U;
  client->server_info.headers = false;
  client->server_info.jetstream = false;

  /* Transition to send CONNECT */
  if (client->state == NATS_STATE_WAIT_INFO) {
    client->state = NATS_STATE_SEND_CONNECT;
  }

  return NATS_OK;
}

/**
 * @brief Send CONNECT command
 */
static nats_err_t send_connect(nats_client_t *client) {
  nats_err_t err;

  /* Build minimal CONNECT JSON */
  /* TODO: Add auth fields when configured */
  err = send_linef(
      client,
      "CONNECT {\"verbose\":%s,\"pedantic\":%s,\"name\":\"%s\","
      "\"lang\":\"c\",\"version\":\"%s\",\"protocol\":1,\"echo\":%s}",
      client->opts.verbose ? "true" : "false",
      client->opts.pedantic ? "true" : "false", client->name, NATS_VERSION_STR,
      client->opts.echo ? "true" : "false");

  if (err != NATS_OK) {
    return err;
  }

  /* Send initial PING */
  err = send_line(client, "PING");
  if (err != NATS_OK) {
    return err;
  }

  client->state = NATS_STATE_CONNECTED;
  client->pings_out = 1U;
  client->last_ping_sent = client->time_fn();

  /* Emit connected event */
  if (client->event_cb != NULL) {
    client->event_cb(client, NATS_EVENT_CONNECTED, client->event_userdata);
  }

  /* Re-subscribe existing subscriptions (for reconnect) */
  nats_err_t resub_err = NATS_OK;
  for (size_t i = 0U; i < NATS_MAX_SUBSCRIPTIONS; i++) {
    if (client->subs[i].active) {
      err = send_linef(client, "SUB %s %u", client->subs[i].subject,
                       client->subs[i].sid);
      /* Track first error - subscriptions may be silently lost */
      if ((err != NATS_OK) && (resub_err == NATS_OK)) {
        resub_err = err;
      }
    }
  }

  /* Return first resub error if any occurred */
  return resub_err;
}

/**
 * @brief Handle PING from server
 */
static nats_err_t handle_ping(nats_client_t *client) {
  return send_line(client, "PONG");
}

/**
 * @brief Handle PONG from server
 */
static nats_err_t handle_pong(nats_client_t *client) {
  if (client->pings_out > 0U) {
    client->pings_out--;
  }
  /* Unsolicited PONGs (pings_out was already 0) are silently ignored.
   * This prevents a malicious server from suppressing stale connection
   * detection by flooding extra PONGs. */
  client->stats.pongs_recv++;
  return NATS_OK;
}

/**
 * @brief Handle +OK from server
 */
static nats_err_t handle_ok(nats_client_t *client) {
  (void)client; /* Nothing to do in non-verbose mode */
  return NATS_OK;
}

/**
 * @brief Handle -ERR from server
 */
static nats_err_t handle_err(nats_client_t *client, const char *msg) {
  (void)msg; /* TODO: Parse specific error */

  client->last_error = NATS_ERR_SERVER;

  if (client->event_cb != NULL) {
    client->event_cb(client, NATS_EVENT_ERROR, client->event_userdata);
  }

  return NATS_ERR_SERVER;
}

/**
 * @brief Parse MSG header: MSG <subject> <sid> [reply] <size>
 */
static bool parse_msg_header(nats_client_t *client, const char *header,
                             size_t header_len) {
  if ((header == NULL) || (header_len == 0U)) {
    return false;
  }

  const char *p = header;
  const char *tok_end;
  const char *buf_end = header + header_len;
  size_t len;

  /* Subject - check len < MAX so [len] index for null terminator is valid */
  p = nats_skip_space(p, buf_end);
  tok_end = nats_find_token_end(p, buf_end);
  len = (size_t)(tok_end - p);
  if ((len == 0U) || (len >= NATS_MAX_SUBJECT_LEN)) {
    return false;
  }
  memcpy(client->parser.msg_subject, p, len);
  client->parser.msg_subject[len] =
      '\0'; /* Safe: len < MAX guarantees valid index */

  /* SID - must have at least one digit */
  p = nats_skip_space(tok_end, buf_end);
  tok_end = nats_find_token_end(p, buf_end);
  size_t sid_len = (size_t)(tok_end - p);
  if (sid_len == 0U) {
    return false; /* Missing SID */
  }
  /* Verify first char is a digit (not empty or garbage) */
  if ((p[0] < '0') || (p[0] > '9')) {
    return false; /* Invalid SID */
  }
  size_t sid_val = parse_uint(p, sid_len);
  /* SID is uint16_t, reject invalid SID from server */
  if (sid_val > UINT16_MAX) {
    return false; /* Invalid SID */
  }
  client->parser.msg_sid = (uint16_t)sid_val;

  /* Check for reply-to or size */
  p = nats_skip_space(tok_end, buf_end);
  tok_end = nats_find_token_end(p, buf_end);
  len = (size_t)(tok_end - p);

  if (len == 0U) {
    return false; /* Missing size */
  }

  /* Peek ahead to see if there's another token */
  const char *next = nats_skip_space(tok_end, buf_end);
  if ((next < buf_end) && (*next != '\0')) {
    /* This token is reply-to, next is size */
    /* Check len < MAX so [len] index for null terminator is valid */
    if (len >= NATS_MAX_SUBJECT_LEN) {
      return false;
    }
    memcpy(client->parser.msg_reply, p, len);
    client->parser.msg_reply[len] = '\0';

    p = next;
    tok_end = nats_find_token_end(p, buf_end);
    len = (size_t)(tok_end - p);
    if (len == 0U) {
      return false; /* Missing size */
    }
    client->parser.expected_bytes = parse_uint(p, len);
  } else {
    /* This token is size, no reply-to */
    client->parser.msg_reply[0] = '\0';
    client->parser.expected_bytes = parse_uint(p, len);
  }

  /* Validate payload size */
  if (client->parser.expected_bytes > NATS_MAX_PAYLOAD_LEN) {
    return false; /* Payload too large */
  }

  return true;
}

/**
 * @brief Deliver message to subscription callback
 */
static void deliver_msg(nats_client_t *client, const uint8_t *payload,
                        size_t len) {
  /* Find subscription */
  nats_sub_t *sub = NULL;
  for (size_t i = 0U; i < NATS_MAX_SUBSCRIPTIONS; i++) {
    if (client->subs[i].active &&
        (client->subs[i].sid == client->parser.msg_sid)) {
      sub = &client->subs[i];
      break;
    }
  }

  if (sub == NULL) {
    /* No subscription found, ignore message */
    return;
  }

  /* Build message struct */
  bool has_reply = (client->parser.msg_reply[0] != '\0');
  nats_msg_t msg = {.subject = client->parser.msg_subject,
                    .subject_len = strlen(client->parser.msg_subject),
                    .reply = has_reply ? client->parser.msg_reply : NULL,
                    .reply_len =
                        has_reply ? strlen(client->parser.msg_reply) : 0U,
                    .data = payload,
                    .data_len = len,
                    .sid = client->parser.msg_sid};

  /* Update stats */
  client->stats.msgs_in++;
  client->stats.bytes_in += (uint32_t)len;
  sub->recv_msgs++;

  /* Invoke callback */
  if (sub->callback != NULL) {
    sub->callback(client, &msg, sub->userdata);
  }

  /* Auto-unsubscribe if max reached */
  if ((sub->max_msgs > 0U) && (sub->recv_msgs >= sub->max_msgs)) {
    sub->active = false;
  }
}

/**
 * @brief Detect command type from line
 */
typedef enum {
  CMD_UNKNOWN = 0,
  CMD_INFO,
  CMD_MSG,
  CMD_HMSG,
  CMD_PING,
  CMD_PONG,
  CMD_OK,
  CMD_ERR
} cmd_type_t;

static cmd_type_t detect_cmd(const char *line, size_t len) {
  if (len < 2U) {
    return CMD_UNKNOWN;
  }

  switch (line[0]) {
  case 'I':
    if ((len >= 4U) && (memcmp(line, "INFO", 4U) == 0)) {
      return CMD_INFO;
    }
    break;
  case 'M':
    if ((len >= 3U) && (memcmp(line, "MSG", 3U) == 0)) {
      return CMD_MSG;
    }
    break;
  case 'H':
    if ((len >= 4U) && (memcmp(line, "HMSG", 4U) == 0)) {
      return CMD_HMSG;
    }
    break;
  case 'P':
    if ((len >= 4U) && (memcmp(line, "PING", 4U) == 0)) {
      return CMD_PING;
    }
    if ((len >= 4U) && (memcmp(line, "PONG", 4U) == 0)) {
      return CMD_PONG;
    }
    break;
  case '+':
    if ((len >= 3U) && (memcmp(line, "+OK", 3U) == 0)) {
      return CMD_OK;
    }
    break;
  case '-':
    if ((len >= 4U) && (memcmp(line, "-ERR", 4U) == 0)) {
      return CMD_ERR;
    }
    break;
  default:
    break;
  }

  return CMD_UNKNOWN;
}

/**
 * @brief Parse incoming data
 */
static nats_err_t parse_data(nats_client_t *client) {
  nats_err_t err = NATS_OK;

  while (client->rx_len > 0U) {
    if (client->parser.state == NATS_PARSE_LINE) {
      /* Look for complete line */
      int32_t line_end = nats_find_crlf(client->rx_buf, client->rx_len);
      if (line_end < 0) {
        /* Incomplete line, wait for more data */
        break;
      }

      if (line_end < 2) {
        err = NATS_ERR_PROTOCOL;
        client->last_error = err;
        break;
      }

      /* Null-terminate for parsing (overwrite \r) */
      client->rx_buf[(size_t)(line_end - 2)] = '\0';
      size_t line_len = (size_t)(line_end - 2);

      /* Validate line length against protocol maximum */
      if (line_len > NATS_MAX_LINE_LEN) {
        err = NATS_ERR_PROTOCOL;
        client->last_error = err;
        break;
      }

      /* Detect and handle command */
      cmd_type_t cmd = detect_cmd((const char *)client->rx_buf, line_len);

      switch (cmd) {
      case CMD_INFO:
        if (line_len > 5U) {
          err = handle_info(client, (const char *)&client->rx_buf[5]);
        } else {
          err = handle_info(client, "");
        }
        break;

      case CMD_MSG:
        /* line_len >= 4 guaranteed by detect_cmd returning CMD_MSG */
        if (!parse_msg_header(client, (const char *)&client->rx_buf[4],
                              line_len - 4U)) {
          err = NATS_ERR_PROTOCOL;
        } else {
          client->parser.state = NATS_PARSE_MSG_PAYLOAD;
        }
        break;

      case CMD_HMSG:
        /* TODO: Implement HMSG (headers) */
        err = NATS_ERR_PROTOCOL;
        break;

      case CMD_PING:
        err = handle_ping(client);
        break;

      case CMD_PONG:
        err = handle_pong(client);
        break;

      case CMD_OK:
        err = handle_ok(client);
        break;

      case CMD_ERR:
        if (line_len > 5U) {
          err = handle_err(client, (const char *)&client->rx_buf[5]);
        } else {
          err = handle_err(client, "");
        }
        break;

      default:
        err = NATS_ERR_PROTOCOL;
        break;
      }

      /* Consume the line */
      compact_rx_buf(client, (size_t)line_end);

      if (err != NATS_OK) {
        client->last_error = err;
        break;
      }

    } else if (client->parser.state == NATS_PARSE_MSG_PAYLOAD) {
      /* Check for integer overflow before addition */
      if (client->parser.expected_bytes > (SIZE_MAX - 2U)) {
        err = NATS_ERR_PROTOCOL;
        client->last_error = err;
        break;
      }
      /* Need payload + \r\n */
      size_t needed = client->parser.expected_bytes + 2U;
      if (client->rx_len < needed) {
        /* Wait for more data */
        break;
      }

      /* Verify \r\n after payload */
      if ((client->rx_buf[client->parser.expected_bytes] != '\r') ||
          (client->rx_buf[client->parser.expected_bytes + 1U] != '\n')) {
        err = NATS_ERR_PROTOCOL;
        client->last_error = err;
        break;
      }

      /* Deliver message */
      deliver_msg(client, client->rx_buf, client->parser.expected_bytes);

      /* Consume payload + \r\n */
      compact_rx_buf(client, needed);

      /* Back to line mode */
      client->parser.state = NATS_PARSE_LINE;
    } else {
      /* Unknown parser state */
      err = NATS_ERR_INVALID_STATE;
      break;
    }
  }

  return err;
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

nats_err_t nats_init(nats_client_t *client) {
  nats_options_t opts = NATS_OPTIONS_DEFAULT;
  return nats_init_opts(client, &opts);
}

nats_err_t nats_init_opts(nats_client_t *client, const nats_options_t *opts) {
  if (client == NULL) {
    return NATS_ERR_INVALID_ARG;
  }

  /* Zero everything */
  memset(client, 0, sizeof(nats_client_t));

  /* Set defaults */
  client->state = NATS_STATE_DISCONNECTED;
  client->next_sid = 1U;
  client->parser.state = NATS_PARSE_LINE;

  /* Copy options */
  if (opts != NULL) {
    client->opts = *opts;
    if (opts->name != NULL) {
      safe_strcpy(client->name, opts->name, sizeof(client->name));
    } else {
      safe_strcpy(client->name, "nats-embedded", sizeof(client->name));
    }
  } else {
    client->opts.ping_interval_ms = 30000U;
    client->opts.connect_timeout_ms = 5000U;
    client->opts.max_pings_out = 2U;
    client->opts.echo = true;
    safe_strcpy(client->name, "nats-embedded", sizeof(client->name));
  }

  return NATS_OK;
}

nats_err_t nats_set_transport(nats_client_t *client,
                              const nats_transport_t *transport) {
  if ((client == NULL) || (transport == NULL)) {
    return NATS_ERR_INVALID_ARG;
  }
  if ((transport->send == NULL) || (transport->recv == NULL) ||
      (transport->connected == NULL)) {
    return NATS_ERR_INVALID_ARG;
  }

  client->transport = *transport;
  return NATS_OK;
}

nats_err_t nats_set_time_fn(nats_client_t *client, nats_time_ms_t time_fn) {
  if ((client == NULL) || (time_fn == NULL)) {
    return NATS_ERR_INVALID_ARG;
  }

  client->time_fn = time_fn;
  return NATS_OK;
}

nats_err_t nats_set_event_callback(nats_client_t *client, nats_event_cb_t cb,
                                   void *userdata) {
  if (client == NULL) {
    return NATS_ERR_INVALID_ARG;
  }

  client->event_cb = cb;
  client->event_userdata = userdata;
  return NATS_OK;
}

nats_err_t nats_handshake(nats_client_t *client) {
  if (client == NULL) {
    return NATS_ERR_INVALID_ARG;
  }
  if (client->transport.send == NULL) {
    return NATS_ERR_INVALID_ARG;
  }
  if (client->time_fn == NULL) {
    return NATS_ERR_INVALID_ARG;
  }

  /* Reset state */
  client->rx_len = 0U;
  client->tx_len = 0U;
  client->parser.state = NATS_PARSE_LINE;
  client->pings_out = 0U;
  client->last_error = NATS_OK;

  /* Wait for INFO */
  client->state = NATS_STATE_WAIT_INFO;
  client->last_activity = client->time_fn();

  return NATS_OK;
}

nats_err_t nats_close(nats_client_t *client) {
  if (client == NULL) {
    return NATS_ERR_INVALID_ARG;
  }

  if (client->transport.close != NULL) {
    client->transport.close(client->transport.ctx);
  }

  client->state = NATS_STATE_CLOSED;

  if (client->event_cb != NULL) {
    client->event_cb(client, NATS_EVENT_CLOSED, client->event_userdata);
  }

  return NATS_OK;
}

nats_err_t nats_process(nats_client_t *client) {
  if (client == NULL) {
    return NATS_ERR_INVALID_ARG;
  }

  /* Check transport */
  if ((client->transport.connected == NULL) ||
      !client->transport.connected(client->transport.ctx)) {
    if (client->state == NATS_STATE_CONNECTED) {
      client->state = NATS_STATE_DISCONNECTED;
      client->last_error = NATS_ERR_CONNECTION_LOST;

      /* Reset parser state to prevent desync on reconnect */
      client->parser.state = NATS_PARSE_LINE;
      client->parser.expected_bytes = 0U;
      client->parser.msg_sid = 0U;
      client->parser.msg_subject[0] = '\0';
      client->parser.msg_reply[0] = '\0';

      if (client->event_cb != NULL) {
        client->event_cb(client, NATS_EVENT_DISCONNECTED,
                         client->event_userdata);
      }
    }
    return NATS_ERR_NOT_CONNECTED;
  }

  /* Read available data */
  size_t space = sizeof(client->rx_buf) - client->rx_len;
  if (space > 0U) {
    int32_t n = client->transport.recv(client->transport.ctx,
                                       &client->rx_buf[client->rx_len], space);

    if (n < 0) {
      client->last_error = NATS_ERR_IO;
      return NATS_ERR_IO;
    }
    if (n > 0) {
      client->rx_len += (size_t)n;
      client->stats.bytes_in += (uint32_t)n;
      client->last_activity = client->time_fn();
    }
  }

  /* Parse data */
  nats_err_t err = parse_data(client);

  /* Handle state transitions */
  if (client->state == NATS_STATE_SEND_CONNECT) {
    err = send_connect(client);
  }

  return err;
}

/**
 * @brief Wrap-safe timer elapsed check
 *
 * Uses signed comparison to handle uint32_t timer wraparound correctly.
 * Safe: max elapsed = ping_interval_ms (30000) << INT32_MAX
 */
static bool timer_elapsed(uint32_t now, uint32_t start, uint32_t interval_ms) {
  return ((int32_t)(now - start) >= (int32_t)interval_ms);
}

nats_err_t nats_check_ping(nats_client_t *client) {
  if (client == NULL) {
    return NATS_ERR_INVALID_ARG;
  }

  if (client->state != NATS_STATE_CONNECTED) {
    return NATS_OK; /* Not connected, nothing to do */
  }

  uint32_t now = client->time_fn();

  /* Check for stale connection */
  if (client->pings_out >= client->opts.max_pings_out) {
    client->last_error = NATS_ERR_STALE_CONNECTION;
    client->state = NATS_STATE_DISCONNECTED;
    if (client->event_cb != NULL) {
      client->event_cb(client, NATS_EVENT_DISCONNECTED, client->event_userdata);
    }
    return NATS_ERR_STALE_CONNECTION;
  }

  /* Send PING if interval elapsed - uses wrap-safe timer comparison */
  if (timer_elapsed(now, client->last_ping_sent,
                    client->opts.ping_interval_ms)) {
    nats_err_t err = send_line(client, "PING");
    if (err == NATS_OK) {
      client->pings_out++;
      client->last_ping_sent = now;
      client->stats.pings_sent++;
    }
    return err;
  }

  return NATS_OK;
}

nats_err_t nats_publish(nats_client_t *client, const char *subject,
                        const uint8_t *data, size_t len) {
  return nats_publish_reply(client, subject, NULL, data, len);
}

nats_err_t nats_publish_reply(nats_client_t *client, const char *subject,
                              const char *reply, const uint8_t *data,
                              size_t len) {
  if ((client == NULL) || (subject == NULL)) {
    return NATS_ERR_INVALID_ARG;
  }
  if ((data == NULL) && (len > 0U)) {
    return NATS_ERR_INVALID_ARG;
  }
  if (client->state != NATS_STATE_CONNECTED) {
    return NATS_ERR_NOT_CONNECTED;
  }
  if (len > NATS_MAX_PAYLOAD_LEN) {
    return NATS_ERR_BUFFER_OVERFLOW;
  }

  nats_err_t err;

  /* Send PUB line */
  if (reply != NULL) {
    err = send_linef(client, "PUB %s %s %u", subject, reply, (unsigned)len);
  } else {
    err = send_linef(client, "PUB %s %u", subject, (unsigned)len);
  }
  if (err != NATS_OK) {
    return err;
  }

  /* Send payload */
  if (len > 0U) {
    err = send_data(client, data, len);
    if (err != NATS_OK) {
      return err;
    }
  }

  /* Send trailing CRLF */
  err = send_data(client, (const uint8_t *)"\r\n", 2U);
  if (err != NATS_OK) {
    return err;
  }

  client->stats.msgs_out++;
  return NATS_OK;
}

nats_err_t nats_publish_str(nats_client_t *client, const char *subject,
                            const char *str) {
  if (str == NULL) {
    return nats_publish(client, subject, NULL, 0U);
  }
  return nats_publish(client, subject, (const uint8_t *)str, strlen(str));
}

nats_err_t nats_subscribe(nats_client_t *client, const char *subject,
                          nats_msg_cb_t cb, void *userdata, uint16_t *sid) {
  return nats_subscribe_queue(client, subject, NULL, cb, userdata, sid);
}

nats_err_t nats_subscribe_queue(nats_client_t *client, const char *subject,
                                const char *queue, nats_msg_cb_t cb,
                                void *userdata, uint16_t *sid) {
  if ((client == NULL) || (subject == NULL) || (cb == NULL)) {
    return NATS_ERR_INVALID_ARG;
  }
  if (!nats_subject_valid(subject, NATS_MAX_SUBJECT_LEN)) {
    return NATS_ERR_INVALID_ARG;
  }
  if (client->state != NATS_STATE_CONNECTED) {
    return NATS_ERR_NOT_CONNECTED;
  }

  /* Find free slot */
  nats_sub_t *sub = NULL;
  for (size_t i = 0U; i < NATS_MAX_SUBSCRIPTIONS; i++) {
    if (!client->subs[i].active) {
      sub = &client->subs[i];
      break;
    }
  }

  if (sub == NULL) {
    return NATS_ERR_NO_MEMORY;
  }

  /* Fill subscription */
  safe_strcpy(sub->subject, subject, sizeof(sub->subject));
  sub->callback = cb;
  sub->userdata = userdata;

  /* Check for SID exhaustion before incrementing */
  if (client->next_sid == UINT16_MAX) {
    return NATS_ERR_NO_MEMORY; /* SID space exhausted */
  }
  sub->sid = client->next_sid++;
  sub->max_msgs = 0U;
  sub->recv_msgs = 0U;
  sub->active = true;

  /* Send SUB command */
  nats_err_t err;
  if (queue != NULL) {
    err = send_linef(client, "SUB %s %s %u", subject, queue, sub->sid);
  } else {
    err = send_linef(client, "SUB %s %u", subject, sub->sid);
  }

  if (err != NATS_OK) {
    sub->active = false;
    return err;
  }

  if (sid != NULL) {
    *sid = sub->sid;
  }

  return NATS_OK;
}

nats_err_t nats_unsubscribe(nats_client_t *client, uint16_t sid) {
  return nats_unsubscribe_after(client, sid, 0U);
}

nats_err_t nats_unsubscribe_after(nats_client_t *client, uint16_t sid,
                                  uint16_t max_msgs) {
  if (client == NULL) {
    return NATS_ERR_INVALID_ARG;
  }

  /* Find subscription */
  nats_sub_t *sub = NULL;
  for (size_t i = 0U; i < NATS_MAX_SUBSCRIPTIONS; i++) {
    if (client->subs[i].active && (client->subs[i].sid == sid)) {
      sub = &client->subs[i];
      break;
    }
  }

  if (sub == NULL) {
    return NATS_ERR_NOT_FOUND;
  }

  nats_err_t err;
  if (max_msgs > 0U) {
    sub->max_msgs = max_msgs;
    err = send_linef(client, "UNSUB %u %u", sid, max_msgs);
  } else {
    sub->active = false;
    err = send_linef(client, "UNSUB %u", sid);
  }

  return err;
}

nats_err_t nats_new_inbox(nats_client_t *client, char *inbox,
                          size_t inbox_len) {
  if ((client == NULL) || (inbox == NULL) || (inbox_len < 24U)) {
    return NATS_ERR_INVALID_ARG;
  }

  /* Simple inbox: _INBOX.<random> */
  /* TODO: Better random generation */
  uint32_t r = client->time_fn() ^ (uint32_t)(uintptr_t)client;
  int ret = snprintf(inbox, inbox_len, "_INBOX.%08X%04X", (unsigned int)r,
                     (unsigned int)client->next_sid);
  if ((ret < 0) || ((size_t)ret >= inbox_len)) {
    return NATS_ERR_BUFFER_OVERFLOW;
  }

  return NATS_OK;
}

nats_state_t nats_get_state(const nats_client_t *client) {
  if (client == NULL) {
    return NATS_STATE_DISCONNECTED;
  }
  return client->state;
}

nats_err_t nats_get_last_error(const nats_client_t *client) {
  if (client == NULL) {
    return NATS_ERR_INVALID_ARG;
  }
  return client->last_error;
}

nats_err_t nats_get_stats(const nats_client_t *client, nats_stats_t *stats) {
  if ((client == NULL) || (stats == NULL)) {
    return NATS_ERR_INVALID_ARG;
  }

  *stats = client->stats;
  return NATS_OK;
}

bool nats_is_connected(const nats_client_t *client) {
  if (client == NULL) {
    return false;
  }
  return (client->state == NATS_STATE_CONNECTED);
}

const char *nats_err_str(nats_err_t err) {
  if ((size_t)err < NATS_ERR_COUNT) {
    return NATS_ERR_STRINGS[err];
  }
  return "Unknown error";
}

const char *nats_state_str(nats_state_t state) {
  if ((size_t)state < NATS_STATE_COUNT) {
    return NATS_STATE_STRINGS[state];
  }
  return "UNKNOWN";
}

/*============================================================================
 * Message Response Implementation
 *============================================================================*/

nats_err_t nats_msg_respond(nats_client_t *client, const nats_msg_t *msg,
                            const uint8_t *data, size_t len) {
  if ((client == NULL) || (msg == NULL)) {
    return NATS_ERR_INVALID_ARG;
  }

  /* Must have a reply-to subject */
  if ((msg->reply == NULL) || (msg->reply_len == 0U)) {
    return NATS_ERR_INVALID_ARG;
  }

  return nats_publish(client, msg->reply, data, len);
}

nats_err_t nats_msg_respond_str(nats_client_t *client, const nats_msg_t *msg,
                                const char *str) {
  if (str == NULL) {
    return nats_msg_respond(client, msg, NULL, 0U);
  }
  return nats_msg_respond(client, msg, (const uint8_t *)str, strlen(str));
}

/*============================================================================
 * Async Request/Reply Implementation
 *============================================================================*/

/* Internal callback for request responses */
static void request_response_cb(nats_client_t *client, const nats_msg_t *msg,
                                void *userdata) {
  (void)client;
  nats_request_t *req = (nats_request_t *)userdata;

  if ((req == NULL) || (!req->active)) {
    return;
  }

  /* Copy response data */
  size_t copy_len = msg->data_len;
  if (copy_len > NATS_MAX_PAYLOAD_LEN) {
    copy_len = NATS_MAX_PAYLOAD_LEN;
  }

  if ((msg->data != NULL) && (copy_len > 0U)) {
    memcpy(req->response_data, msg->data, copy_len);
  }
  req->response_len = copy_len;
  req->completed = true;
}

nats_err_t nats_request_start(nats_client_t *client, nats_request_t *req,
                              const char *subject, const uint8_t *data,
                              size_t len, uint32_t timeout_ms) {
  if ((client == NULL) || (req == NULL) || (subject == NULL)) {
    return NATS_ERR_INVALID_ARG;
  }
  if (client->state != NATS_STATE_CONNECTED) {
    return NATS_ERR_NOT_CONNECTED;
  }

  /* Initialize request state */
  memset(req, 0, sizeof(nats_request_t));

  /* Generate inbox */
  nats_err_t err = nats_new_inbox(client, req->inbox, sizeof(req->inbox));
  if (err != NATS_OK) {
    return err;
  }

  /* Subscribe to inbox (auto-unsub after 1 message) */
  err = nats_subscribe(client, req->inbox, request_response_cb, req, &req->sid);
  if (err != NATS_OK) {
    return err;
  }

  /* Auto-unsubscribe after 1 message */
  err = nats_unsubscribe_after(client, req->sid, 1U);
  if (err != NATS_OK) {
    nats_unsubscribe(client, req->sid);
    return err;
  }

  /* Publish request with reply-to */
  err = nats_publish_reply(client, subject, req->inbox, data, len);
  if (err != NATS_OK) {
    nats_unsubscribe(client, req->sid);
    return err;
  }

  /* Mark request as active */
  req->start_time = client->time_fn();
  req->timeout_ms = timeout_ms;
  req->active = true;
  req->completed = false;
  req->timed_out = false;

  return NATS_OK;
}

nats_err_t nats_request_check(nats_client_t *client, nats_request_t *req) {
  if ((client == NULL) || (req == NULL)) {
    return NATS_ERR_INVALID_ARG;
  }

  if (!req->active) {
    return NATS_ERR_INVALID_STATE;
  }

  /* Check if response received */
  if (req->completed) {
    req->active = false;
    return NATS_OK;
  }

  /* Check timeout - uses wrap-safe timer comparison */
  if (timer_elapsed(client->time_fn(), req->start_time, req->timeout_ms)) {
    req->timed_out = true;
    req->active = false;
    /* Clean up subscription */
    nats_unsubscribe(client, req->sid);
    return NATS_ERR_TIMEOUT;
  }

  return NATS_ERR_WOULD_BLOCK;
}

nats_err_t nats_request_cancel(nats_client_t *client, nats_request_t *req) {
  if ((client == NULL) || (req == NULL)) {
    return NATS_ERR_INVALID_ARG;
  }

  if (req->active) {
    nats_unsubscribe(client, req->sid);
    req->active = false;
  }

  return NATS_OK;
}

/*============================================================================
 * Flush & Drain Implementation
 *============================================================================*/

nats_err_t nats_flush(nats_client_t *client) {
  if (client == NULL) {
    return NATS_ERR_INVALID_ARG;
  }
  if (client->state != NATS_STATE_CONNECTED) {
    return NATS_ERR_NOT_CONNECTED;
  }

  /* Send PING and wait for PONG to confirm server received all prior data */
  nats_err_t err = send_line(client, "PING");
  if (err != NATS_OK) {
    return err;
  }

  client->pings_out++;
  client->last_ping_sent = client->time_fn();
  client->stats.pings_sent++;

  return NATS_OK;
}

nats_err_t nats_drain(nats_client_t *client) {
  if (client == NULL) {
    return NATS_ERR_INVALID_ARG;
  }
  if (client->state != NATS_STATE_CONNECTED) {
    return NATS_ERR_NOT_CONNECTED;
  }

  /* Transition to draining state */
  client->state = NATS_STATE_DRAINING;

  /* Unsubscribe all subscriptions */
  for (size_t i = 0U; i < NATS_MAX_SUBSCRIPTIONS; i++) {
    if (client->subs[i].active) {
      send_linef(client, "UNSUB %u", client->subs[i].sid);
      /* Keep active=true to receive in-flight messages */
    }
  }

  /* Send PING to flush */
  nats_flush(client);

  return NATS_OK;
}

/*============================================================================
 * Subject Utilities Implementation
 *============================================================================*/

bool nats_subject_valid(const char *subject, size_t max_len) {
  if ((subject == NULL) || (max_len == 0U) || (subject[0] == '\0')) {
    return false;
  }

  bool last_was_dot = true; /* Treat start as after dot to catch leading dot */
  size_t i = 0U;

  for (i = 0U; (i < max_len) && (subject[i] != '\0'); i++) {
    char c = subject[i];

    /* Check for invalid characters:
     * - Must be printable ASCII (0x21-0x7E) or dot (0x2E)
     * - Space (0x20) is explicitly invalid
     * - Control chars (0x00-0x1F) are invalid
     * - DEL (0x7F) is invalid
     * - Extended ASCII (0x80+) is invalid
     * Note: We allow wildcards (* >) for pattern validation
     */
    if ((c == ' ') || (c < 0x20) || (c == 0x7F)) {
      return false;
    }
    /* Reject extended ASCII (chars with high bit set) */
    if ((unsigned char)c > 0x7E) {
      return false;
    }

    if (c == '.') {
      /* Empty token (double dot or leading dot) */
      if (last_was_dot) {
        return false;
      }
      last_was_dot = true;
    } else {
      last_was_dot = false;
    }
  }

  /* Subject too long (reached max_len without finding null terminator) */
  if (i == max_len) {
    return false;
  }

  /* Trailing dot is invalid */
  if (last_was_dot) {
    return false;
  }

  return true;
}

bool nats_subject_matches(const char *pattern, size_t pattern_len,
                          const char *subject, size_t subject_len) {
  if ((pattern == NULL) || (subject == NULL)) {
    return false;
  }
  if ((pattern_len == 0U) || (subject_len == 0U)) {
    return false;
  }

  const char *p = pattern;
  const char *s = subject;
  const char *p_end = pattern + pattern_len;
  const char *s_end = subject + subject_len;

  while ((p < p_end) && (*p != '\0')) {
    if (*p == '>') {
      /* '>' matches one or more remaining tokens
       * MUST be the last token in the pattern (nothing after it)
       */
      if (((p + 1) < p_end) && (p[1] != '\0')) {
        /* Invalid pattern: > is not at the end */
        return false;
      }
      /* '>' requires at least one token to match */
      if ((s >= s_end) || (*s == '\0')) {
        return false;
      }
      /* Valid: > matches the rest of the subject */
      return true;
    }

    if (*p == '*') {
      /* '*' matches exactly one token
       * Must be a complete token (followed by '.' or end of pattern)
       */
      if (((p + 1) < p_end) && (p[1] != '\0') && (p[1] != '.')) {
        /* Invalid pattern: * not followed by dot or end (e.g., "*foo" or "*>")
         */
        return false;
      }

      /* Subject must have a non-empty token to match */
      if ((s >= s_end) || (*s == '\0') || (*s == '.')) {
        /* No token or empty token - no match */
        return false;
      }

      /* Skip the subject token (consume all chars until dot or end) */
      while ((s < s_end) && (*s != '\0') && (*s != '.')) {
        s++;
      }

      /* Advance pattern past '*' */
      p++;

      /* Handle the separator after '*' */
      if ((p < p_end) && (*p == '.')) {
        p++;
        if ((s < s_end) && (*s == '.')) {
          s++;
        } else if ((s < s_end) && (*s != '\0')) {
          /* Pattern has more tokens but subject doesn't have separator */
          return false;
        }
      }
    } else if (*p == '.') {
      /* Dot in pattern must match dot in subject */
      if ((s >= s_end) || (*s != '.')) {
        return false;
      }
      p++;
      s++;
    } else {
      /* Literal character match */
      if ((s >= s_end) || (*p != *s)) {
        return false;
      }
      p++;
      s++;
    }
  }

  /* Pattern exhausted - subject must also be exhausted */
  return ((s >= s_end) || (*s == '\0'));
}

const char *nats_version(void) { return NATS_VERSION_STR; }

/*============================================================================
 * Test Wrappers (compile with -DNATS_TESTING to enable)
 *============================================================================*/

#ifdef NATS_TESTING

bool nats_test_parse_msg_header(nats_client_t *client, const char *header,
                                size_t header_len) {
  return parse_msg_header(client, header, header_len);
}

nats_test_cmd_t nats_test_detect_cmd(const char *line, size_t len) {
  /* Map internal cmd_type_t to public nats_test_cmd_t */
  cmd_type_t cmd = detect_cmd(line, len);
  switch (cmd) {
  case CMD_INFO:
    return NATS_TEST_CMD_INFO;
  case CMD_MSG:
    return NATS_TEST_CMD_MSG;
  case CMD_HMSG:
    return NATS_TEST_CMD_HMSG;
  case CMD_PING:
    return NATS_TEST_CMD_PING;
  case CMD_PONG:
    return NATS_TEST_CMD_PONG;
  case CMD_OK:
    return NATS_TEST_CMD_OK;
  case CMD_ERR:
    return NATS_TEST_CMD_ERR;
  default:
    return NATS_TEST_CMD_UNKNOWN;
  }
}

#endif /* NATS_TESTING */
