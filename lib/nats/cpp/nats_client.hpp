/**
 * @file nats_client.hpp
 * @brief C++ RAII wrapper for nats-esp32
 *
 * Header-only, zero-cost C++ wrapper providing RAII semantics
 * around the nats-esp32 C API. All methods inline to C functions.
 *
 * @copyright Copyright (c) 2026 Mario Schallner
 */

#ifndef NATS_CLIENT_HPP
#define NATS_CLIENT_HPP

#include "nats_core.h"

#include <cstdint>
#include <cstring>
#include <functional>

namespace nats {

/**
 * @brief Error codes (mirrors nats_err_t)
 */
using Error = nats_err_t;

/**
 * @brief Connection states (mirrors nats_state_t)
 */
using State = nats_state_t;

/**
 * @brief Events (mirrors nats_event_t)
 */
using Event = nats_event_t;

/**
 * @brief Message view (wraps nats_msg_t)
 *
 * Non-owning view of a message. Valid only during callback.
 */
class Message {
public:
    explicit Message(const nats_msg_t* msg) : msg_(msg) {}

    const char* subject() const { return msg_->subject; }
    size_t subject_len() const { return msg_->subject_len; }

    const char* reply() const { return msg_->reply; }
    size_t reply_len() const { return msg_->reply_len; }
    bool has_reply() const { return msg_->reply != nullptr && msg_->reply_len > 0; }

    const uint8_t* data() const { return msg_->data; }
    size_t size() const { return msg_->data_len; }

    // Convenience: view data as string (not null-terminated!)
    const char* data_str() const { return reinterpret_cast<const char*>(msg_->data); }

    uint16_t sid() const { return msg_->sid; }

    const nats_msg_t* raw() const { return msg_; }

private:
    const nats_msg_t* msg_;
};

/**
 * @brief Client options (wraps nats_options_t)
 */
struct Options {
    const char* name = "nats-embedded";
    const char* user = nullptr;
    const char* pass = nullptr;
    const char* token = nullptr;
    uint32_t ping_interval_ms = 30000;
    uint32_t connect_timeout_ms = 5000;
    uint8_t max_pings_out = 2;
    bool verbose = false;
    bool pedantic = false;
    bool echo = true;

    nats_options_t to_c() const {
        nats_options_t opts;
        opts.name = name;
        opts.user = user;
        opts.pass = pass;
        opts.token = token;
        opts.ping_interval_ms = ping_interval_ms;
        opts.connect_timeout_ms = connect_timeout_ms;
        opts.max_pings_out = max_pings_out;
        opts.verbose = verbose;
        opts.pedantic = pedantic;
        opts.echo = echo;
        return opts;
    }
};

/**
 * @brief Client statistics (wraps nats_stats_t)
 */
struct Stats {
    uint32_t msgs_in = 0;
    uint32_t msgs_out = 0;
    uint32_t bytes_in = 0;
    uint32_t bytes_out = 0;
    uint32_t reconnects = 0;
    uint32_t pings_sent = 0;
    uint32_t pongs_recv = 0;

    static Stats from_c(const nats_stats_t& s) {
        Stats stats;
        stats.msgs_in = s.msgs_in;
        stats.msgs_out = s.msgs_out;
        stats.bytes_in = s.bytes_in;
        stats.bytes_out = s.bytes_out;
        stats.reconnects = s.reconnects;
        stats.pings_sent = s.pings_sent;
        stats.pongs_recv = s.pongs_recv;
        return stats;
    }
};

/**
 * @brief Message callback type
 */
using MessageCallback = std::function<void(const Message&)>;

/**
 * @brief Event callback type
 */
using EventCallback = std::function<void(Event)>;

/**
 * @brief RAII NATS Client
 *
 * Zero-cost wrapper around nats_client_t. The client structure
 * is embedded directly (no heap allocation).
 *
 * Usage:
 * @code
 * nats::Client client;
 * client.set_transport(transport);
 * client.set_time_fn(millis);
 * client.handshake();
 * while (client.state() != nats::State::NATS_STATE_CONNECTED) {
 *     client.process();
 * }
 * client.subscribe("foo.>", [](const nats::Message& msg) {
 *     // handle message
 * });
 * @endcode
 */
class Client {
public:
    /**
     * @brief Construct with default options
     */
    Client() {
        nats_init(&client_);
    }

    /**
     * @brief Construct with custom options
     */
    explicit Client(const Options& opts) {
        nats_options_t c_opts = opts.to_c();
        nats_init_opts(&client_, &c_opts);
    }

    /**
     * @brief Destructor - closes connection
     */
    ~Client() {
        nats_close(&client_);
    }

    // Non-copyable
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Movable
    Client(Client&& other) noexcept : client_(other.client_) {
        // Invalidate source - it will init fresh if used
        nats_init(&other.client_);
    }

    Client& operator=(Client&& other) noexcept {
        if (this != &other) {
            nats_close(&client_);
            client_ = other.client_;
            nats_init(&other.client_);
        }
        return *this;
    }

    //--- Transport Setup ---//

    /**
     * @brief Set transport interface
     */
    Error set_transport(const nats_transport_t& transport) {
        return nats_set_transport(&client_, &transport);
    }

    /**
     * @brief Set time function
     */
    Error set_time_fn(nats_time_ms_t time_fn) {
        return nats_set_time_fn(&client_, time_fn);
    }

    //--- Connection ---//

    /**
     * @brief Begin protocol handshake
     */
    Error handshake() {
        return nats_handshake(&client_);
    }

    /**
     * @brief Close connection
     */
    Error close() {
        return nats_close(&client_);
    }

    //--- Main Loop ---//

    /**
     * @brief Process incoming data (call in loop)
     */
    Error process() {
        return nats_process(&client_);
    }

    /**
     * @brief Check and send PING if needed
     */
    Error check_ping() {
        return nats_check_ping(&client_);
    }

    //--- Publish ---//

    /**
     * @brief Publish binary data
     */
    Error publish(const char* subject, const uint8_t* data, size_t len) {
        return nats_publish(&client_, subject, data, len);
    }

    /**
     * @brief Publish string data
     */
    Error publish(const char* subject, const char* str) {
        return nats_publish_str(&client_, subject, str);
    }

    /**
     * @brief Publish with reply-to
     */
    Error publish(const char* subject, const char* reply,
                  const uint8_t* data, size_t len) {
        return nats_publish_reply(&client_, subject, reply, data, len);
    }

    //--- Subscribe ---//

    /**
     * @brief Subscribe with C-style callback
     */
    Error subscribe(const char* subject, nats_msg_cb_t cb,
                    void* userdata, uint16_t* sid = nullptr) {
        return nats_subscribe(&client_, subject, cb, userdata, sid);
    }

    /**
     * @brief Subscribe with queue group
     */
    Error subscribe_queue(const char* subject, const char* queue,
                          nats_msg_cb_t cb, void* userdata,
                          uint16_t* sid = nullptr) {
        return nats_subscribe_queue(&client_, subject, queue, cb, userdata, sid);
    }

    /**
     * @brief Unsubscribe
     */
    Error unsubscribe(uint16_t sid) {
        return nats_unsubscribe(&client_, sid);
    }

    /**
     * @brief Unsubscribe after N messages
     */
    Error unsubscribe_after(uint16_t sid, uint16_t max_msgs) {
        return nats_unsubscribe_after(&client_, sid, max_msgs);
    }

    //--- Request/Reply ---//

    /**
     * @brief Generate unique inbox
     */
    Error new_inbox(char* inbox, size_t len) {
        return nats_new_inbox(&client_, inbox, len);
    }

    /**
     * @brief Respond to a message
     */
    Error respond(const Message& msg, const uint8_t* data, size_t len) {
        return nats_msg_respond(&client_, msg.raw(), data, len);
    }

    /**
     * @brief Respond to a message with string
     */
    Error respond(const Message& msg, const char* str) {
        return nats_msg_respond_str(&client_, msg.raw(), str);
    }

    //--- Connection Management ---//

    /**
     * @brief Flush pending data
     */
    Error flush() {
        return nats_flush(&client_);
    }

    /**
     * @brief Begin graceful drain
     */
    Error drain() {
        return nats_drain(&client_);
    }

    //--- Status ---//

    /**
     * @brief Get current state
     */
    State state() const {
        return nats_get_state(&client_);
    }

    /**
     * @brief Check if connected
     */
    bool connected() const {
        return nats_is_connected(&client_);
    }

    /**
     * @brief Get last error
     */
    Error last_error() const {
        return nats_get_last_error(&client_);
    }

    /**
     * @brief Get statistics
     */
    Stats stats() const {
        nats_stats_t s;
        nats_get_stats(&client_, &s);
        return Stats::from_c(s);
    }

    //--- Raw Access ---//

    /**
     * @brief Get underlying C client (for advanced use)
     */
    nats_client_t* raw() { return &client_; }
    const nats_client_t* raw() const { return &client_; }

    //--- Static Utilities ---//

    /**
     * @brief Get error string
     */
    static const char* error_str(Error err) {
        return nats_err_str(err);
    }

    /**
     * @brief Get state string
     */
    static const char* state_str(State state) {
        return nats_state_str(state);
    }

    /**
     * @brief Get library version
     */
    static const char* version() {
        return nats_version();
    }

    /**
     * @brief Validate subject
     */
    static bool subject_valid(const char* subject, size_t max_len = NATS_MAX_SUBJECT_LEN) {
        return nats_subject_valid(subject, max_len);
    }

private:
    nats_client_t client_;
};

} // namespace nats

#endif // NATS_CLIENT_HPP
