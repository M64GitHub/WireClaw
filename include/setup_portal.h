/**
 * @file setup_portal.h
 * @brief WiFi AP captive portal for initial device configuration
 *
 * When no valid config exists or WiFi fails, boots into AP mode
 * and serves a web form for the user to configure the device.
 */

#ifndef SETUP_PORTAL_H
#define SETUP_PORTAL_H

/**
 * Start the WiFi AP captive portal for device configuration.
 * Blocks until the user saves a config or the 5-minute timeout expires,
 * then reboots the ESP32. Does not return.
 */
void runSetupPortal();

#endif /* SETUP_PORTAL_H */
