/**
 * @file interface.h
 * @brief NATS Transport Interface
 *
 * All transport adapters implement this interface.
 * The application provides platform-specific implementations.
 */

#ifndef NATS_TRANSPORT_INTERFACE_H
#define NATS_TRANSPORT_INTERFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Transport function pointers (set by platform adapter)
 */
typedef struct {
  int32_t (*send)(void *ctx, const uint8_t *data, size_t len);
  int32_t (*recv)(void *ctx, uint8_t *data, size_t max_len);
  bool (*connected)(void *ctx);
  void (*close)(void *ctx);
  void *ctx; /* Platform-specific context (socket, client, etc.) */
} nats_transport_t;

#ifdef __cplusplus
}
#endif

#endif /* NATS_TRANSPORT_INTERFACE_H */
