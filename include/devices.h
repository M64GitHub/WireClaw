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
    DEV_SENSOR_CLOCK_HOUR,      /* getLocalTime() -> 0-23 */
    DEV_SENSOR_CLOCK_MINUTE,    /* getLocalTime() -> 0-59 */
    DEV_SENSOR_CLOCK_HHMM,     /* getLocalTime() -> hour*100+minute (e.g. 1830) */
    DEV_SENSOR_NATS_VALUE,     /* value received from NATS subject */
    DEV_SENSOR_SERIAL_TEXT,    /* text received from UART serial port */
    /* Actuators */
    DEV_ACTUATOR_DIGITAL,       /* digitalWrite */
    DEV_ACTUATOR_RELAY,         /* digitalWrite (inverted flag) */
    DEV_ACTUATOR_PWM,           /* analogWrite 0-255 */
    DEV_ACTUATOR_RGB_LED,       /* rgbLedWrite packed 0xRRGGBB */
};

struct Device {
    char        name[DEV_NAME_LEN];
    DeviceKind  kind;
    uint8_t     pin;
    char        unit[DEV_UNIT_LEN];
    bool        inverted;
    bool        used;
    /* NATS virtual sensor fields (only meaningful for DEV_SENSOR_NATS_VALUE) */
    char        nats_subject[32];
    float       nats_value;
    char        nats_msg[64];
    uint16_t    nats_sid;
    /* Serial text baud rate (only meaningful for DEV_SENSOR_SERIAL_TEXT) */
    uint32_t    baud;
    /* Last value set on actuator (for display; not persisted, resets on boot) */
    int         last_value;
    /* EMA-smoothed sensor value (runtime only, not persisted) */
    float       ema;
    bool        ema_init;
    /* Recent readings for sparkline (runtime only, not persisted) */
    #define DEV_HISTORY_LEN 6
    float       history[DEV_HISTORY_LEN];
    uint8_t     history_idx;
    bool        history_full;
};

/* Initialize device registry - loads from /devices.json, auto-registers chip_temp */
void devicesInit();

/* Background sensor poll - call from main loop to keep EMA warm */
void sensorsPoll();

/* Persist device registry to /devices.json */
void devicesSave();

/* Clear all devices (deinits serial_text if active, zeroes array) */
void devicesClear();

/* Reload devices from /devices.json, re-register builtins */
void devicesReload();

/* Register a new device. Returns true on success. */
bool deviceRegister(const char *name, DeviceKind kind, uint8_t pin,
                    const char *unit, bool inverted,
                    const char *nats_subject = nullptr,
                    uint32_t baud = 0);

/* Remove a device by name. Returns true if found and removed. */
bool deviceRemove(const char *name);

/* Find a device by name. Returns nullptr if not found. */
Device *deviceFind(const char *name);

/* Read a sensor device. Returns the reading as a float. */
float deviceReadSensor(Device *dev, bool record_hist = false);

/* Set an actuator device. value: 0/1 for digital/relay, 0-255 for PWM. Returns true on success. */
bool deviceSetActuator(Device *dev, int value);

/* Check if a DeviceKind is a sensor type */
bool deviceIsSensor(DeviceKind kind);

/* Check if a DeviceKind is an actuator type */
bool deviceIsActuator(DeviceKind kind);

/* Get the device array (for listing) */
Device *deviceGetAll();

/* Get the kind name as a string */
const char *deviceKindName(DeviceKind kind);

/* Get mutable device array (for NATS subscription management) */
Device *deviceGetAllMutable();

/* Set the NATS value + message on a device */
void deviceSetNatsValue(Device *dev, float value, const char *msg);

/* Get the NATS message string from a device */
const char *deviceGetNatsMsg(const Device *dev);

/* Parse a NATS payload into value + message */
void parseNatsPayload(const uint8_t *data, size_t len,
                      float *out_value, char *out_msg, size_t msg_len);

/* --- Serial text UART (one device max, fixed pins per chip) --- */

/* Fixed UART1 pins per chip variant */
#if defined(CONFIG_IDF_TARGET_ESP32C6)
#define SERIAL_TEXT_RX  4
#define SERIAL_TEXT_TX  5
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define SERIAL_TEXT_RX  19
#define SERIAL_TEXT_TX  20
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#define SERIAL_TEXT_RX  4
#define SERIAL_TEXT_TX  5
#else
#define SERIAL_TEXT_RX  16
#define SERIAL_TEXT_TX  17
#endif

void serialTextInit(uint32_t baud);
void serialTextDeinit();
void serialTextPoll();
bool serialTextSend(const char *text);
const char *serialTextGetMsg();
float serialTextGetValue();
bool serialTextActive();

/* Returns true if the rgb_led device is set to a non-zero color (suppresses heartbeat) */
bool rgbLedOverride();

#endif /* DEVICES_H */
