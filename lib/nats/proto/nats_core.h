/**
 * @file nats_core.h
 * @brief NATS Embedded Client - Platform-Independent Core
 *
 * A zero-allocation, MISRA-C compliant NATS client core.
 * This file contains NO platform dependencies - all I/O is done
 * through user-provided function pointers.
 *
 * @author mario@synadia.com
 * @copyright Copyright (c) 2026 Synadia Communications Inc.
 * @license Apache-2.0
 *
 * MISRA-C:2012 Compliance Notes:
 * - All functions return explicit error codes (no silent failures)
 * - Fixed-width integer types used throughout
 * - No dynamic memory allocation
 * - No recursion
 * - All switch statements have default cases
 */

#ifndef NATS_CORE_H
#define NATS_CORE_H

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
 * Version
 *============================================================================*/

#define NATS_CORE_VERSION_MAJOR 0U
#define NATS_CORE_VERSION_MINOR 1U
#define NATS_CORE_VERSION_PATCH 0U

/*============================================================================
 * Configuration - Override these before including if needed
 *============================================================================*/

/** Maximum length of a NATS subject */
#ifndef NATS_MAX_SUBJECT_LEN
#define NATS_MAX_SUBJECT_LEN 128U
#endif

/** Maximum length of a single protocol line (INFO, MSG header, etc.) */
#ifndef NATS_MAX_LINE_LEN
#define NATS_MAX_LINE_LEN 512U
#endif

/** Maximum message payload size */
#ifndef NATS_MAX_PAYLOAD_LEN
#define NATS_MAX_PAYLOAD_LEN 4096U
#endif

/** Maximum number of concurrent subscriptions */
#ifndef NATS_MAX_SUBSCRIPTIONS
#define NATS_MAX_SUBSCRIPTIONS 16U
#endif

/** Receive buffer size (must hold line + payload + \r\n) */
#ifndef NATS_RX_BUFFER_SIZE
#define NATS_RX_BUFFER_SIZE (NATS_MAX_LINE_LEN + NATS_MAX_PAYLOAD_LEN + 4U)
#endif

/** Transmit buffer size */
#ifndef NATS_TX_BUFFER_SIZE
#define NATS_TX_BUFFER_SIZE 512U
#endif

/** Maximum client name length */
#ifndef NATS_MAX_NAME_LEN
#define NATS_MAX_NAME_LEN 32U
#endif

/*============================================================================
 * Compile-Time Safety Checks
 *
 * These static asserts verify buffer sizes fit within return types,
 * eliminating overflow risks that might otherwise be flagged.
 *============================================================================*/

/**
 * Static assert macro for compile-time checks (C99/C11 compatible)
 */
#ifndef NATS_STATIC_ASSERT
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define NATS_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
/* C99 fallback using negative array size */
#define NATS_STATIC_ASSERT(cond, msg)                                          \
  typedef char nats_static_assert_##__LINE__[(cond) ? 1 : -1]
#endif
#endif

/* Verify buffer sizes fit in int32_t (transport send/recv return type) */
NATS_STATIC_ASSERT(NATS_RX_BUFFER_SIZE <= 2147483647UL,
                   "RX buffer exceeds int32_t range");
NATS_STATIC_ASSERT(NATS_TX_BUFFER_SIZE <= 2147483647UL,
                   "TX buffer exceeds int32_t range");

/* Verify payload fits in unsigned int for %u format specifier portability */
NATS_STATIC_ASSERT(NATS_MAX_PAYLOAD_LEN <= 4294967295UL,
                   "Payload size exceeds uint32_t range");

/** Default NATS port */
#define NATS_DEFAULT_PORT 4222U

/*============================================================================
 * Error Codes
 *============================================================================*/

/**
 * @brief NATS client error codes
 *
 * Categorized as:
 * - 0: Success
 * - 1-99: Recoverable errors (retry possible)
 * - 100-199: Protocol/server errors (reconnect needed)
 * - 200+: Permanent errors (configuration/usage issues)
 */
typedef enum {
  /* Success */
  NATS_OK = 0,

  /* Recoverable - retry */
  NATS_ERR_WOULD_BLOCK = 1, /**< Operation would block, try later */
  NATS_ERR_TIMEOUT = 2,     /**< Operation timed out */
  NATS_ERR_IO = 3,          /**< I/O error from transport */

  /* Recoverable - reconnect needed */
  NATS_ERR_NOT_CONNECTED = 100,    /**< Not connected to server */
  NATS_ERR_CONNECTION_LOST = 101,  /**< Connection dropped */
  NATS_ERR_PROTOCOL = 102,         /**< Protocol parse error */
  NATS_ERR_SERVER = 103,           /**< Server sent -ERR */
  NATS_ERR_STALE_CONNECTION = 104, /**< Too many outstanding PINGs */

  /* Permanent - configuration error */
  NATS_ERR_INVALID_ARG = 200,     /**< Invalid argument passed */
  NATS_ERR_BUFFER_FULL = 201,     /**< Buffer is full */
  NATS_ERR_BUFFER_OVERFLOW = 202, /**< Data exceeds buffer capacity */
  NATS_ERR_NO_MEMORY = 203,       /**< Static allocation exhausted */
  NATS_ERR_INVALID_STATE = 204,   /**< Operation invalid in current state */
  NATS_ERR_AUTH_FAILED = 205,     /**< Authentication failed */
  NATS_ERR_NOT_FOUND = 206,       /**< Subscription not found */

  NATS_ERR_COUNT /**< Number of error codes (for bounds) */
} nats_err_t;

/*============================================================================
 * Connection States
 *============================================================================*/

/**
 * @brief Connection state machine states
 */
typedef enum {
  NATS_STATE_DISCONNECTED = 0, /**< Not connected */
  NATS_STATE_CONNECTING = 1,   /**< TCP connecting (reserved, not yet used) */
  NATS_STATE_WAIT_INFO = 2,    /**< Waiting for INFO from server */
  NATS_STATE_SEND_CONNECT = 3, /**< Sending CONNECT command */
  NATS_STATE_CONNECTED = 4,    /**< Fully connected, ready for pub/sub */
  NATS_STATE_RECONNECTING = 5, /**< Attempting reconnection (reserved, not yet used) */
  NATS_STATE_DRAINING = 6,     /**< Draining before disconnect */
  NATS_STATE_CLOSED = 7,       /**< Permanently closed */

  NATS_STATE_COUNT /**< Number of states (for bounds) */
} nats_state_t;

/*============================================================================
 * Parser States (internal)
 *============================================================================*/

typedef enum {
  NATS_PARSE_LINE = 0,         /**< Reading command line */
  NATS_PARSE_MSG_PAYLOAD = 1,  /**< Reading MSG payload */
  NATS_PARSE_HMSG_HEADERS = 2, /**< Reading HMSG headers */
  NATS_PARSE_HMSG_PAYLOAD = 3, /**< Reading HMSG payload */

  NATS_PARSE_COUNT
} nats_parse_state_t;

/*============================================================================
 * Event Types
 *============================================================================*/

/**
 * @brief Events emitted by the client
 */
typedef enum {
  NATS_EVENT_CONNECTED = 0,    /**< Successfully connected */
  NATS_EVENT_DISCONNECTED = 1, /**< Disconnected from server */
  NATS_EVENT_RECONNECTING = 2, /**< Attempting to reconnect */
  NATS_EVENT_ERROR = 3,        /**< Error occurred */
  NATS_EVENT_CLOSED = 4,       /**< Client permanently closed */

  NATS_EVENT_COUNT
} nats_event_t;

/*============================================================================
 * Data Structures
 *============================================================================*/

/* Forward declaration */
struct nats_client;

/**
 * @brief Received message structure
 *
 * All pointers point into the receive buffer and are only valid
 * during the callback invocation. Do NOT store these pointers.
 */
typedef struct {
  const char *subject; /**< Message subject (null-terminated) */
  size_t subject_len;  /**< Subject length (excluding null) */
  const char *reply;   /**< Reply-to subject or NULL */
  size_t reply_len;    /**< Reply length (0 if no reply) */
  const uint8_t *data; /**< Payload data */
  size_t data_len;     /**< Payload length */
  uint16_t sid;        /**< Subscription ID */
} nats_msg_t;

/**
 * @brief Message callback function type
 *
 * @param client    Pointer to the NATS client
 * @param msg       Pointer to received message (valid only during callback)
 * @param userdata  User-provided context pointer
 *
 * @note The msg pointer and all its members are only valid during
 *       the callback. Copy any data you need to retain.
 */
typedef void (*nats_msg_cb_t)(struct nats_client *client, const nats_msg_t *msg,
                              void *userdata);

/**
 * @brief Event callback function type
 *
 * @param client    Pointer to the NATS client
 * @param event     Event type
 * @param userdata  User-provided context pointer
 */
typedef void (*nats_event_cb_t)(struct nats_client *client, nats_event_t event,
                                void *userdata);

/*============================================================================
 * Transport Abstraction (User Implements)
 *============================================================================*/

/**
 * @brief Transport send function type
 *
 * @param ctx       User transport context
 * @param data      Data to send
 * @param len       Length of data
 * @return          Bytes written, 0 if would block, -1 on error
 */
typedef int32_t (*nats_transport_send_t)(void *ctx, const uint8_t *data,
                                         size_t len);

/**
 * @brief Transport receive function type
 *
 * @param ctx       User transport context
 * @param data      Buffer to receive into
 * @param len       Maximum bytes to read
 * @return          Bytes read, 0 if no data, -1 on error
 */
typedef int32_t (*nats_transport_recv_t)(void *ctx, uint8_t *data, size_t len);

/**
 * @brief Transport status function type
 *
 * @param ctx       User transport context
 * @return          true if connected, false otherwise
 */
typedef bool (*nats_transport_connected_t)(void *ctx);

/**
 * @brief Transport close function type
 *
 * @param ctx       User transport context
 */
typedef void (*nats_transport_close_t)(void *ctx);

/**
 * @brief Time function type - returns milliseconds since boot
 *
 * @return          Current time in milliseconds
 */
typedef uint32_t (*nats_time_ms_t)(void);

/**
 * @brief Transport interface structure
 */
typedef struct {
  nats_transport_send_t send;           /**< Send function (required) */
  nats_transport_recv_t recv;           /**< Receive function (required) */
  nats_transport_connected_t connected; /**< Connection check (required) */
  nats_transport_close_t close;         /**< Close function (optional) */
  void *ctx;                            /**< User context for callbacks */
} nats_transport_t;

/*============================================================================
 * Subscription Entry
 *============================================================================*/

typedef struct {
  char subject[NATS_MAX_SUBJECT_LEN]; /**< Subject pattern */
  nats_msg_cb_t callback;             /**< Message callback */
  void *userdata;                     /**< User context */
  uint16_t sid;                       /**< Subscription ID */
  uint16_t max_msgs;                  /**< Max messages (0=unlimited) */
  uint16_t recv_msgs;                 /**< Messages received */
  bool active;                        /**< Subscription active flag */
} nats_sub_t;

/*============================================================================
 * Parser Context (internal)
 *============================================================================*/

typedef struct {
  nats_parse_state_t state;
  size_t expected_bytes; /**< Payload bytes expected */
  size_t header_bytes;   /**< Header bytes (HMSG only) */
  uint16_t msg_sid;      /**< Current message SID */
  char msg_subject[NATS_MAX_SUBJECT_LEN];
  char msg_reply[NATS_MAX_SUBJECT_LEN];
} nats_parser_t;

/*============================================================================
 * Client Options
 *============================================================================*/

typedef struct {
  const char *name;            /**< Client name (NULL for default) */
  const char *user;            /**< Username (NULL if no auth) */
  const char *pass;            /**< Password (NULL if no auth) */
  const char *token;           /**< Auth token (NULL if no auth) */
  uint32_t ping_interval_ms;   /**< PING interval (default 30000) */
  uint32_t connect_timeout_ms; /**< Connect timeout (default 5000) */
  uint8_t max_pings_out;       /**< Max outstanding PINGs (default 2) */
  bool verbose;                /**< Verbose mode (server sends +OK) */
  bool pedantic;               /**< Pedantic mode */
  bool echo;                   /**< Echo own messages (default true) */
} nats_options_t;

/*============================================================================
 * Client Statistics
 *============================================================================*/

/**
 * Statistics counters use uint32_t for embedded memory efficiency.
 * Counters wrap at ~4GB (bytes) or ~4B (messages). Applications
 * needing long-term accuracy should track deltas periodically.
 */
typedef struct {
  uint32_t msgs_in;    /**< Messages received */
  uint32_t msgs_out;   /**< Messages published */
  uint32_t bytes_in;   /**< Bytes received */
  uint32_t bytes_out;  /**< Bytes sent */
  uint32_t reconnects; /**< Number of reconnections */
  uint32_t pings_sent; /**< PING commands sent */
  uint32_t pongs_recv; /**< PONG responses received */
} nats_stats_t;

/*============================================================================
 * Main Client Structure
 *============================================================================*/

typedef struct nats_client {
  /* Transport */
  nats_transport_t transport;
  nats_time_ms_t time_fn; /**< Millisecond time function */

  /* Buffers (no heap allocation!) */
  uint8_t rx_buf[NATS_RX_BUFFER_SIZE];
  uint8_t tx_buf[NATS_TX_BUFFER_SIZE];
  size_t rx_len; /**< Bytes in receive buffer */
  size_t tx_len; /**< Bytes in transmit buffer */

  /* State */
  nats_state_t state;
  nats_err_t last_error;
  nats_parser_t parser;

  /* Subscriptions */
  nats_sub_t subs[NATS_MAX_SUBSCRIPTIONS];
  uint16_t next_sid; /**< Next subscription ID */

  /* Timing */
  uint32_t last_activity;  /**< Last rx/tx timestamp */
  uint32_t last_ping_sent; /**< Last PING sent timestamp */
  uint8_t pings_out;       /**< Outstanding PING count */

  /* Callbacks */
  nats_event_cb_t event_cb; /**< Event callback */
  void *event_userdata;     /**< Event callback context */

  /* Configuration */
  nats_options_t opts;
  char name[NATS_MAX_NAME_LEN];

  /* Statistics */
  nats_stats_t stats;

  /* Server info (from INFO message) */
  struct {
    char server_id[64];
    char server_name[64];
    uint16_t proto; /**< Protocol version */
    bool headers;   /**< Headers supported */
    bool jetstream; /**< JetStream available */
  } server_info;

} nats_client_t;

/*============================================================================
 * API Functions
 *============================================================================*/

/*--- Initialization ---*/

/**
 * @brief Initialize a NATS client with default options
 *
 * @param client    Pointer to client structure to initialize
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_init(nats_client_t *client);

/**
 * @brief Initialize a NATS client with custom options
 *
 * @param client    Pointer to client structure to initialize
 * @param opts      Pointer to options (NULL for defaults)
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_init_opts(nats_client_t *client, const nats_options_t *opts);

/**
 * @brief Set the transport interface
 *
 * @param client    Initialized client
 * @param transport Transport interface (functions + context)
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_set_transport(nats_client_t *client,
                              const nats_transport_t *transport);

/**
 * @brief Set the time function
 *
 * @param client    Initialized client
 * @param time_fn   Function returning milliseconds since boot
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_set_time_fn(nats_client_t *client, nats_time_ms_t time_fn);

/**
 * @brief Set event callback
 *
 * @param client    Initialized client
 * @param cb        Event callback function
 * @param userdata  User context for callback
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_set_event_callback(nats_client_t *client, nats_event_cb_t cb,
                                   void *userdata);

/*--- Connection ---*/

/**
 * @brief Begin NATS protocol handshake
 *
 * Requires transport and time function to be set.
 * After calling, use nats_process() until state becomes NATS_STATE_CONNECTED.
 *
 * @param client    Initialized client with transport set
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_handshake(nats_client_t *client);

/**
 * @brief Close the connection
 *
 * @param client    Connected client
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_close(nats_client_t *client);

/*--- Main Loop ---*/

/**
 * @brief Process incoming data and handle protocol
 *
 * This is the main work function. Call this regularly (e.g., in loop()).
 * It will:
 * - Read data from transport
 * - Parse NATS protocol messages
 * - Invoke message callbacks
 * - Handle PING/PONG
 * - Update connection state
 *
 * @param client    Client to process
 * @return          NATS_OK on success, error code otherwise
 *
 * @note This function is non-blocking if transport recv is non-blocking
 */
nats_err_t nats_process(nats_client_t *client);

/**
 * @brief Check and send periodic PING if needed
 *
 * Call this periodically to maintain connection health.
 *
 * @param client    Client to check
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_check_ping(nats_client_t *client);

/*--- Publish ---*/

/**
 * @brief Publish a message
 *
 * @param client    Connected client
 * @param subject   Subject to publish to
 * @param data      Payload data (can be NULL if len is 0)
 * @param len       Payload length
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_publish(nats_client_t *client, const char *subject,
                        const uint8_t *data, size_t len);

/**
 * @brief Publish a message with reply-to subject
 *
 * @param client    Connected client
 * @param subject   Subject to publish to
 * @param reply     Reply-to subject
 * @param data      Payload data (can be NULL if len is 0)
 * @param len       Payload length
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_publish_reply(nats_client_t *client, const char *subject,
                              const char *reply, const uint8_t *data,
                              size_t len);

/**
 * @brief Publish a null-terminated string
 *
 * Convenience function for publishing string data.
 *
 * @param client    Connected client
 * @param subject   Subject to publish to
 * @param str       Null-terminated string payload
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_publish_str(nats_client_t *client, const char *subject,
                            const char *str);

/*--- Subscribe ---*/

/**
 * @brief Subscribe to a subject
 *
 * @param client    Connected client
 * @param subject   Subject pattern (may include wildcards)
 * @param cb        Message callback
 * @param userdata  User context for callback
 * @param[out] sid  Subscription ID (output, can be NULL)
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_subscribe(nats_client_t *client, const char *subject,
                          nats_msg_cb_t cb, void *userdata, uint16_t *sid);

/**
 * @brief Subscribe with queue group
 *
 * @param client    Connected client
 * @param subject   Subject pattern (may include wildcards)
 * @param queue     Queue group name
 * @param cb        Message callback
 * @param userdata  User context for callback
 * @param[out] sid  Subscription ID (output, can be NULL)
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_subscribe_queue(nats_client_t *client, const char *subject,
                                const char *queue, nats_msg_cb_t cb,
                                void *userdata, uint16_t *sid);

/**
 * @brief Unsubscribe
 *
 * @param client    Connected client
 * @param sid       Subscription ID to unsubscribe
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_unsubscribe(nats_client_t *client, uint16_t sid);

/**
 * @brief Unsubscribe after N messages
 *
 * @param client    Connected client
 * @param sid       Subscription ID
 * @param max_msgs  Unsubscribe after this many messages
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_unsubscribe_after(nats_client_t *client, uint16_t sid,
                                  uint16_t max_msgs);

/*--- Request/Reply ---*/

/**
 * @brief Generate a unique inbox subject
 *
 * @param client    Client (used for unique ID generation)
 * @param inbox     Buffer to write inbox subject
 * @param inbox_len Size of inbox buffer
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_new_inbox(nats_client_t *client, char *inbox, size_t inbox_len);

/**
 * @brief Respond to a received message
 *
 * Publishes a response to the message's reply-to subject.
 * Returns error if message has no reply-to subject.
 *
 * @param client    Connected client
 * @param msg       Message to respond to (must have reply-to)
 * @param data      Response payload data
 * @param len       Response payload length
 * @return          NATS_OK on success, NATS_ERR_INVALID_ARG if no reply-to
 */
nats_err_t nats_msg_respond(nats_client_t *client, const nats_msg_t *msg,
                            const uint8_t *data, size_t len);

/**
 * @brief Respond to a received message with a string
 *
 * Convenience function for responding with null-terminated string.
 *
 * @param client    Connected client
 * @param msg       Message to respond to (must have reply-to)
 * @param str       Response string (null-terminated)
 * @return          NATS_OK on success, NATS_ERR_INVALID_ARG if no reply-to
 */
nats_err_t nats_msg_respond_str(nats_client_t *client, const nats_msg_t *msg,
                                const char *str);

/*--- Async Request/Reply ---*/

/**
 * @brief Async request state structure
 *
 * Holds state for a pending request. User allocates this on stack or heap.
 * All fields are managed by the nats_request_* functions.
 */
typedef struct {
  char inbox[NATS_MAX_SUBJECT_LEN]; /**< Auto-generated reply inbox */
  uint16_t sid;                     /**< Subscription ID for inbox */
  uint32_t start_time;              /**< Request start timestamp */
  uint32_t timeout_ms;              /**< Timeout in milliseconds */
  bool completed;                   /**< Response received flag */
  bool timed_out;                   /**< Timeout occurred flag */
  bool active;                      /**< Request is active */
  /* Response data - valid when completed=true */
  uint8_t response_data[NATS_MAX_PAYLOAD_LEN]; /**< Response payload copy */
  size_t response_len;                         /**< Response payload length */
} nats_request_t;

/**
 * @brief Start an async request
 *
 * Creates inbox, subscribes to it, and publishes request.
 * Call nats_request_check() in your loop to poll for response.
 *
 * @param client     Connected client
 * @param req        Request state (caller provides storage)
 * @param subject    Subject to send request to
 * @param data       Request payload data (can be NULL)
 * @param len        Request payload length
 * @param timeout_ms Timeout in milliseconds
 * @return           NATS_OK on success, error code otherwise
 */
nats_err_t nats_request_start(nats_client_t *client, nats_request_t *req,
                              const char *subject, const uint8_t *data,
                              size_t len, uint32_t timeout_ms);

/**
 * @brief Check for request response
 *
 * Call this in your main loop after nats_process().
 *
 * @param client    Client to check
 * @param req       Request state from nats_request_start()
 * @return          NATS_OK if response received (check req->response_data),
 *                  NATS_ERR_WOULD_BLOCK if still waiting,
 *                  NATS_ERR_TIMEOUT if timed out
 */
nats_err_t nats_request_check(nats_client_t *client, nats_request_t *req);

/**
 * @brief Cancel a pending request
 *
 * Unsubscribes from inbox and marks request as inactive.
 *
 * @param client    Client
 * @param req       Request state to cancel
 * @return          NATS_OK on success
 */
nats_err_t nats_request_cancel(nats_client_t *client, nats_request_t *req);

/*--- Connection Management ---*/

/**
 * @brief Flush pending data
 *
 * Ensures all buffered data is sent to the server.
 * In this zero-copy implementation, data is sent immediately,
 * so this function sends a PING and waits for PONG to confirm
 * the server has received all prior messages.
 *
 * @param client    Connected client
 * @return          NATS_OK if flushed, NATS_ERR_WOULD_BLOCK if waiting
 */
nats_err_t nats_flush(nats_client_t *client);

/**
 * @brief Begin graceful drain
 *
 * Starts draining: unsubscribes all subscriptions but continues
 * processing incoming messages until server confirms.
 * Keep calling nats_process() until state becomes CLOSED.
 *
 * @param client    Connected client
 * @return          NATS_OK on success
 */
nats_err_t nats_drain(nats_client_t *client);

/*--- Status & Info ---*/

/**
 * @brief Get current client state
 *
 * @param client    Client to query
 * @return          Current state
 */
nats_state_t nats_get_state(const nats_client_t *client);

/**
 * @brief Get last error code
 *
 * @param client    Client to query
 * @return          Last error code
 */
nats_err_t nats_get_last_error(const nats_client_t *client);

/**
 * @brief Get client statistics
 *
 * @param client    Client to query
 * @param[out] stats    Statistics output
 * @return          NATS_OK on success, error code otherwise
 */
nats_err_t nats_get_stats(const nats_client_t *client, nats_stats_t *stats);

/**
 * @brief Check if connected
 *
 * @param client    Client to query
 * @return          true if in CONNECTED state, false otherwise
 */
bool nats_is_connected(const nats_client_t *client);

/*--- Subject Utilities ---*/

/**
 * @brief Validate a NATS subject
 *
 * Checks that the subject follows NATS naming rules:
 * - Not empty
 * - No spaces or control characters
 * - Tokens separated by '.'
 * - No empty tokens (no ".." or leading/trailing ".")
 * - Length less than max_len
 *
 * @param subject   Subject to validate
 * @param max_len   Maximum allowed length (use NATS_MAX_SUBJECT_LEN)
 * @return          true if valid, false otherwise
 */
bool nats_subject_valid(const char *subject, size_t max_len);

/**
 * @brief Check if a subject matches a pattern
 *
 * Supports NATS wildcards:
 * - '*' matches a single token
 * - '>' matches one or more tokens (must be last)
 *
 * @param pattern      Pattern with wildcards
 * @param pattern_len  Length of pattern string
 * @param subject      Concrete subject to match
 * @param subject_len  Length of subject string
 * @return             true if matches, false otherwise
 */
bool nats_subject_matches(const char *pattern, size_t pattern_len,
                          const char *subject, size_t subject_len);

/*--- Utility ---*/

/**
 * @brief Get error message string
 *
 * @param err       Error code
 * @return          Human-readable error string
 */
const char *nats_err_str(nats_err_t err);

/**
 * @brief Get state name string
 *
 * @param state     State code
 * @return          Human-readable state string
 */
const char *nats_state_str(nats_state_t state);

/**
 * @brief Get library version string
 *
 * @return          Version string (e.g., "0.1.0")
 */
const char *nats_version(void);

/*============================================================================
 * Default Options Initializer
 *============================================================================*/

/**
 * @brief Default options initializer
 */
#define NATS_OPTIONS_DEFAULT                                                   \
  {.name = "nats-embedded",                                                    \
   .user = NULL,                                                               \
   .pass = NULL,                                                               \
   .token = NULL,                                                              \
   .ping_interval_ms = 30000U,                                                 \
   .connect_timeout_ms = 5000U,                                                \
   .max_pings_out = 2U,                                                        \
   .verbose = false,                                                           \
   .pedantic = false,                                                          \
   .echo = true}

/*============================================================================
 * Assertion Macro (for development)
 *============================================================================*/

/**
 * @brief Assert handler function type (user-configurable)
 *
 * @param cond_str  The stringified condition that failed
 * @param file      Source file name (__FILE__)
 * @param line      Line number (__LINE__)
 *
 * This function should not return. Default implementation
 * enters an infinite loop suitable for debugging.
 */
#ifndef NATS_ASSERT_HANDLER
#define NATS_ASSERT_HANDLER(cond_str, file, line)                              \
  do {                                                                         \
    volatile int _nats_halt = 1;                                               \
    while (_nats_halt) { /* halt */                                            \
    }                                                                          \
  } while (0)
#endif

#ifndef NATS_ASSERT
#ifdef NATS_DEBUG
#define NATS_ASSERT(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      NATS_ASSERT_HANDLER(#cond, __FILE__, __LINE__);                          \
    }                                                                          \
  } while (0)
#else
#define NATS_ASSERT(cond) ((void)0)
#endif
#endif

/*============================================================================
 * Test Wrappers (compile with -DNATS_TESTING to enable)
 *============================================================================*/

#ifdef NATS_TESTING

/**
 * @brief Command type enum for testing (mirrors internal cmd_type_t)
 */
typedef enum {
  NATS_TEST_CMD_UNKNOWN = 0,
  NATS_TEST_CMD_INFO,
  NATS_TEST_CMD_MSG,
  NATS_TEST_CMD_HMSG,
  NATS_TEST_CMD_PING,
  NATS_TEST_CMD_PONG,
  NATS_TEST_CMD_OK,
  NATS_TEST_CMD_ERR
} nats_test_cmd_t;

/**
 * @brief [TEST ONLY] Parse MSG header: " subject sid [reply] size"
 *
 * @param client      Client with parser state
 * @param header      Header string (after "MSG")
 * @param header_len  Length of header string
 * @return            true on success, false on parse error
 */
bool nats_test_parse_msg_header(nats_client_t *client, const char *header,
                                size_t header_len);

/**
 * @brief [TEST ONLY] Detect command type from line
 *
 * @param line      Protocol line (null-terminated)
 * @param len       Length of line
 * @return          Command type
 */
nats_test_cmd_t nats_test_detect_cmd(const char *line, size_t len);

#endif /* NATS_TESTING */

#ifdef __cplusplus
}
#endif

#endif /* NATS_CORE_H */
