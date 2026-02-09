/**
 * @file nats_atoms.h
 * @brief nats-atoms - Native NATS for IoT and Embedded Systems
 *
 * Single include header for repository development.
 * Packaging scripts transform paths for flat distribution structure.
 *
 * @copyright Copyright (c) 2026 Synadia Communications Inc.
 * @license Apache-2.0
 */
#ifndef NATS_ATOMS_H
#define NATS_ATOMS_H

/* Core protocol */
#include "proto/nats_core.h"

/* Safe parsing utilities */
#include "parse/nats_parse.h"

/* JSON utilities */
#include "json/nats_json.h"

/* Platform-specific transport (auto-detected) */
#if defined(ARDUINO)
#include "transport/nats_transport_arduino.h"
#elif defined(__unix__) || defined(__APPLE__)
#error "POSIX transport not included in this copy"
#endif

#endif /* NATS_ATOMS_H */
