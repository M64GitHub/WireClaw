/**
 * @file nats_transport_arduino.h
 * @brief NATS Embedded Client - Arduino Adapter
 *
 * Thin wrapper connecting nats_core to Arduino WiFiClient.
 * Include this in your .ino sketch instead of nats_core.h directly.
 *
 * MISRA C++:2008 Compliance Notes:
 * - Rule 6-6-5: Single exit point in functions
 * - Rule 9-3-1: const member functions where applicable
 * - Rule 12-8-2: Deleted copy/move operations
 * - Rule 17-0-1: No reserved identifiers (leading underscores)
 * - Exceptions disabled (project-wide policy)
 * - No RTTI usage
 * - No dynamic memory allocation
 *
 * @author Mario Schallner
 * @copyright Copyright (c) 2026 Mario Schallner
 */

#ifndef NATS_ARDUINO_H
#define NATS_ARDUINO_H

#include "nats_core.h"
#include <Arduino.h>
#include <WiFiClient.h>

/*============================================================================
 * Arduino Transport Wrapper
 *============================================================================*/

/**
 * @brief Arduino NATS client wrapper
 *
 * Combines the core client with Arduino-specific transport.
 */
class NatsClient {
public:
  NatsClient() : m_connected(false) {
    nats_init(&m_client);
    setupTransport();
  }

  // Prevent copying (Rule of Five)
  NatsClient(const NatsClient&) = delete;
  NatsClient& operator=(const NatsClient&) = delete;
  NatsClient(NatsClient&&) = delete;
  NatsClient& operator=(NatsClient&&) = delete;

  ~NatsClient() {
    if (m_connected) {
      disconnect();
    }
  }

  /**
   * @brief Connect to NATS server
   *
   * @param host      Server hostname or IP
   * @param port      Server port (default 4222)
   * @param timeout   Connection timeout in ms
   * @return          true on success
   */
  bool connect(const char *host, uint16_t port = NATS_DEFAULT_PORT,
               uint32_t timeout = 5000U) {
    bool result = false;

    // TCP connect
    if (m_tcp.connect(host, port, timeout)) {
      m_tcp.setNoDelay(true);  // Disable Nagle - must be AFTER connect!
      m_connected = true;

      // NATS handshake
      nats_err_t err = nats_handshake(&m_client);
      if (err == NATS_OK) {
        // Process until connected (with timeout)
        uint32_t start = millis();
        bool timedOut = false;

        while (nats_get_state(&m_client) != NATS_STATE_CONNECTED) {
          if ((millis() - start) > timeout) {
            timedOut = true;
            break;
          }
          nats_process(&m_client);
          yield();
        }

        if (!timedOut) {
          result = true;
        }
      }

      // Cleanup on failure
      if (!result) {
        m_tcp.stop();
        m_connected = false;
      }
    }

    return result;
  }

  /**
   * @brief Disconnect from server
   */
  void disconnect() {
    nats_close(&m_client);
    m_tcp.stop();
    m_connected = false;
  }

  /**
   * @brief Check if connected
   */
  bool connected() const { return m_connected && nats_is_connected(&m_client); }

  /**
   * @brief Process incoming messages (call in loop())
   *
   * @return NATS_OK on success
   */
  nats_err_t process() {
    nats_err_t err = nats_process(&m_client);
    nats_check_ping(&m_client);

    // Check transport health
    if (!m_tcp.connected()) {
      m_connected = false;
      return NATS_ERR_CONNECTION_LOST;
    }

    return err;
  }

  /**
   * @brief Publish a message
   */
  nats_err_t publish(const char *subject, const uint8_t *data, size_t len) {
    return nats_publish(&m_client, subject, data, len);
  }

  /**
   * @brief Publish a string message
   */
  nats_err_t publish(const char *subject, const char *str) {
    return nats_publish_str(&m_client, subject, str);
  }

  /**
   * @brief Subscribe to a subject
   */
  nats_err_t subscribe(const char *subject, nats_msg_cb_t cb,
                       void *userdata = nullptr, uint16_t *sid = nullptr) {
    return nats_subscribe(&m_client, subject, cb, userdata, sid);
  }

  /**
   * @brief Unsubscribe
   */
  nats_err_t unsubscribe(uint16_t sid) {
    return nats_unsubscribe(&m_client, sid);
  }

  /**
   * @brief Subscribe with queue group
   */
  nats_err_t subscribeQueue(const char *subject, const char *queue,
                            nats_msg_cb_t cb, void *userdata = nullptr,
                            uint16_t *sid = nullptr) {
    return nats_subscribe_queue(&m_client, subject, queue, cb, userdata, sid);
  }

  /**
   * @brief Respond to a message (for request/reply pattern)
   */
  nats_err_t respond(const nats_msg_t *msg, const uint8_t *data, size_t len) {
    return nats_msg_respond(&m_client, msg, data, len);
  }

  /**
   * @brief Respond to a message with a string
   */
  nats_err_t respond(const nats_msg_t *msg, const char *str) {
    return nats_msg_respond_str(&m_client, msg, str);
  }

  /**
   * @brief Start an async request
   *
   * @param req        Request state (caller provides storage)
   * @param subject    Subject to request from
   * @param data       Request payload (can be NULL)
   * @param len        Request payload length
   * @param timeout_ms Timeout in milliseconds
   * @return           NATS_OK on success
   */
  nats_err_t requestStart(nats_request_t *req, const char *subject,
                          const uint8_t *data, size_t len,
                          uint32_t timeout_ms) {
    return nats_request_start(&m_client, req, subject, data, len, timeout_ms);
  }

  /**
   * @brief Start an async request with string payload
   */
  nats_err_t requestStart(nats_request_t *req, const char *subject,
                          const char *str, uint32_t timeout_ms) {
    return nats_request_start(&m_client, req, subject,
                              str ? (const uint8_t *)str : NULL,
                              str ? strlen(str) : 0, timeout_ms);
  }

  /**
   * @brief Check if request has response
   *
   * @return NATS_OK if response received, NATS_ERR_WOULD_BLOCK if waiting,
   *         NATS_ERR_TIMEOUT if timed out
   */
  nats_err_t requestCheck(nats_request_t *req) {
    return nats_request_check(&m_client, req);
  }

  /**
   * @brief Cancel a pending request
   */
  nats_err_t requestCancel(nats_request_t *req) {
    return nats_request_cancel(&m_client, req);
  }

  /**
   * @brief Flush pending data (sends PING to confirm delivery)
   */
  nats_err_t flush() { return nats_flush(&m_client); }

  /**
   * @brief Begin graceful drain
   */
  nats_err_t drain() { return nats_drain(&m_client); }

  /**
   * @brief Generate a unique inbox subject
   */
  nats_err_t newInbox(char *inbox, size_t len) {
    return nats_new_inbox(&m_client, inbox, len);
  }

  /**
   * @brief Set event callback
   */
  void onEvent(nats_event_cb_t cb, void *userdata = nullptr) {
    nats_set_event_callback(&m_client, cb, userdata);
  }

  /**
   * @brief Get underlying core client (for advanced usage)
   */
  nats_client_t *core() { return &m_client; }

  /**
   * @brief Get last error
   */
  nats_err_t lastError() const { return nats_get_last_error(&m_client); }

  /**
   * @brief Get error string
   */
  const char *lastErrorStr() const { return nats_err_str(lastError()); }

  /**
   * @brief Get current state
   */
  nats_state_t state() const { return nats_get_state(&m_client); }

private:
  nats_client_t m_client;
  WiFiClient m_tcp;
  bool m_connected;

  void setupTransport() {
    // Set up transport callbacks (C++11-compatible initialization)
    nats_transport_t transport;
    transport.send = transportSend;
    transport.recv = transportRecv;
    transport.connected = transportConnected;
    transport.close = transportClose;
    transport.ctx = this;
    nats_set_transport(&m_client, &transport);
    nats_set_time_fn(&m_client, millis);
  }

  // Static transport callbacks (C-compatible)
  static int32_t transportSend(void *ctx, const uint8_t *data, size_t len) {
    if (ctx == nullptr) {
      return -1;
    }
    if ((data == nullptr) && (len > 0U)) {
      return -1;
    }
    NatsClient *self = static_cast<NatsClient *>(ctx);
    if (!self->m_tcp.connected()) {
      return -1;
    }
    if (len > static_cast<size_t>(INT32_MAX)) {
      len = static_cast<size_t>(INT32_MAX);
    }
    size_t written = self->m_tcp.write(data, len);
    return static_cast<int32_t>(written);
  }

  static int32_t transportRecv(void *ctx, uint8_t *data, size_t len) {
    if (ctx == nullptr) {
      return -1;
    }
    if ((data == nullptr) && (len > 0U)) {
      return -1;
    }
    NatsClient *self = static_cast<NatsClient *>(ctx);
    int available = self->m_tcp.available();
    if (available <= 0) {
      return 0; // No data available (non-blocking)
    }
    size_t toRead = (static_cast<size_t>(available) < len)
                        ? static_cast<size_t>(available)
                        : len;
    int read = self->m_tcp.read(data, toRead);
    return static_cast<int32_t>(read);
  }

  static bool transportConnected(void *ctx) {
    if (ctx == nullptr) {
      return false;
    }
    NatsClient *self = static_cast<NatsClient *>(ctx);
    return self->m_tcp.connected();
  }

  static void transportClose(void *ctx) {
    if (ctx == nullptr) {
      return;
    }
    NatsClient *self = static_cast<NatsClient *>(ctx);
    self->m_tcp.stop();
  }
};

#endif /* NATS_ARDUINO_H */
