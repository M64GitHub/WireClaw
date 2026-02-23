/**
 * @file nats_hal.h
 * @brief NATS Hardware Abstraction Layer for WireClaw
 *
 * Provides a {device_name}.hal.> wildcard subscription that gives
 * external systems direct request/reply access to GPIO, ADC, PWM,
 * UART, system info, and registered sensors/actuators.
 */

#ifndef NATS_HAL_H
#define NATS_HAL_H

#include <nats_esp32.h>

/**
 * NATS message callback for hal.> wildcard subscription.
 * Routes to appropriate handler based on subject suffix.
 */
void onNatsHal(nats_client_t *client, const nats_msg_t *msg, void *userdata);

/**
 * Check if a name is reserved by the HAL (gpio, adc, pwm, etc.).
 * Used to prevent device registration with HAL-reserved names.
 */
bool halIsReservedName(const char *name);

/**
 * Get cached PWM value for a pin (last value written via halPwmSet or NATS).
 */
uint8_t halPwmGet(uint8_t pin);

/**
 * Set PWM value on a pin (analogWrite + cache update).
 */
void halPwmSet(uint8_t pin, uint8_t value);

#endif /* NATS_HAL_H */
