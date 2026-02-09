/**
 * @file devices.h
 * @brief Device registry for named sensors and actuators
 *
 * Provides a registry of named devices (sensors + actuators) that can be
 * referenced by name in rules and LLM tool calls instead of raw GPIO pins.
 */

#ifndef DEVICES_H
#define DEVICES_H

#include <Arduino.h>

#define MAX_DEVICES    16
#define DEV_NAME_LEN   24
#define DEV_UNIT_LEN   8
#define PIN_NONE        255    /* sentinel for virtual sensors (no GPIO pin) */

enum DeviceKind {
    /* Sensors */
    DEV_SENSOR_DIGITAL = 0,     /* digitalRead(pin) -> 0/1 */
    DEV_SENSOR_ANALOG_RAW,      /* analogRead(pin) -> raw ADC */
    DEV_SENSOR_NTC_10K,         /* analogRead(pin) -> C via Steinhart-Hart */
    DEV_SENSOR_LDR,             /* analogRead(pin) -> lux estimate */
    DEV_SENSOR_INTERNAL_TEMP,   /* temperature_sensor_get_celsius() -> C */
    /* Actuators */
    DEV_ACTUATOR_DIGITAL,       /* digitalWrite */
    DEV_ACTUATOR_RELAY,         /* digitalWrite (inverted flag) */
    DEV_ACTUATOR_PWM,           /* analogWrite 0-255 */
};

struct Device {
    char        name[DEV_NAME_LEN];
    DeviceKind  kind;
    uint8_t     pin;
    char        unit[DEV_UNIT_LEN];
    bool        inverted;
    bool        used;
};

/* Initialize device registry — loads from /devices.json, auto-registers chip_temp */
void devicesInit();

/* Persist device registry to /devices.json */
void devicesSave();

/* Register a new device. Returns true on success. */
bool deviceRegister(const char *name, DeviceKind kind, uint8_t pin,
                    const char *unit, bool inverted);

/* Remove a device by name. Returns true if found and removed. */
bool deviceRemove(const char *name);

/* Find a device by name. Returns nullptr if not found. */
Device *deviceFind(const char *name);

/* Read a sensor device. Returns the reading as a float. */
float deviceReadSensor(const Device *dev);

/* Set an actuator device. value: 0/1 for digital/relay, 0-255 for PWM. Returns true on success. */
bool deviceSetActuator(const Device *dev, int value);

/* Check if a DeviceKind is a sensor type */
bool deviceIsSensor(DeviceKind kind);

/* Check if a DeviceKind is an actuator type */
bool deviceIsActuator(DeviceKind kind);

/* Get the device array (for listing) */
const Device *deviceGetAll();

/* Get the kind name as a string */
const char *deviceKindName(DeviceKind kind);

#endif /* DEVICES_H */
