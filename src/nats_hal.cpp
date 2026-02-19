/**
 * @file nats_hal.cpp
 * @brief NATS Hardware Abstraction Layer - direct hardware access via NATS
 *
 * Subscribes to {device_name}.hal.> and routes requests to GPIO, ADC, PWM,
 * UART, system info, and registered device handlers.
 */

#include <Arduino.h>
#include "nats_hal.h"
#include "devices.h"
#include "soc/soc_caps.h"
#if !defined(CONFIG_IDF_TARGET_ESP32)
#include "driver/temperature_sensor.h"
extern temperature_sensor_handle_t g_temp_sensor;
#endif

/* Externs from main.cpp */
extern char cfg_device_name[32];
extern char toolCallJsonBuf[4096];
extern bool g_debug;

/* Shared reply buffer for all handlers */
static char g_hal_reply[512];

/* PWM value cache — so .get can return what was last set */
static uint8_t s_pwm_state[SOC_GPIO_PIN_COUNT] = {0};

/* Reserved HAL keywords — cannot be used as device names */
static const char *HAL_RESERVED[] = {
    "gpio", "adc", "pwm", "dac", "uart", "system", "device", "config"
};
#define HAL_RESERVED_COUNT (sizeof(HAL_RESERVED) / sizeof(HAL_RESERVED[0]))

/* Cached prefix length: strlen(cfg_device_name) + strlen(".hal.") */
static int s_prefix_len = 0;

static int halPrefixLen() {
    if (s_prefix_len == 0)
        s_prefix_len = strlen(cfg_device_name) + 5; /* ".hal." */
    return s_prefix_len;
}

/*============================================================================
 * Helpers
 *============================================================================*/

static void halError(nats_client_t *client, const nats_msg_t *msg,
                     const char *error, const char *detail) {
    snprintf(g_hal_reply, sizeof(g_hal_reply),
             "{\"error\":\"%s\",\"detail\":\"%s\"}", error, detail);
    if (msg->reply_len > 0)
        nats_msg_respond_str(client, msg, g_hal_reply);
}

static int parsePin(const char *s) {
    if (!s || !*s) return -1;
    char *end = nullptr;
    long v = strtol(s, &end, 10);
    if (end == s) return -1;
    return (int)v;
}

/* Advance past first segment, return pointer to rest (after '.') or NULL */
static const char *nextSegment(const char *s) {
    const char *dot = strchr(s, '.');
    if (!dot) return nullptr;
    return dot + 1;
}

/*============================================================================
 * Handler: gpio.{pin}.get / gpio.{pin}.set
 *============================================================================*/

static void halGpio(nats_client_t *client, const nats_msg_t *msg,
                    const char *rest, const char *payload) {
    /* rest = "{pin}.get" or "{pin}.set" */
    if (!rest || !*rest) {
        halError(client, msg, "bad_request", "gpio.{pin}.get or gpio.{pin}.set");
        return;
    }

    /* Extract pin number */
    char pinStr[8];
    const char *dot = strchr(rest, '.');
    if (!dot) {
        halError(client, msg, "bad_request", "missing .get or .set suffix");
        return;
    }
    size_t plen = dot - rest;
    if (plen >= sizeof(pinStr)) {
        halError(client, msg, "bad_request", "pin too long");
        return;
    }
    memcpy(pinStr, rest, plen);
    pinStr[plen] = '\0';

    int pin = parsePin(pinStr);
    if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) {
        halError(client, msg, "bad_pin",
                 pin < 0 ? "invalid pin number" : "pin out of range");
        return;
    }

    const char *action = dot + 1;

    if (strcmp(action, "get") == 0) {
        int val = digitalRead(pin);
        snprintf(g_hal_reply, sizeof(g_hal_reply), "%d", val);
        if (msg->reply_len > 0)
            nats_msg_respond_str(client, msg, g_hal_reply);
    } else if (strcmp(action, "set") == 0) {
        int val = 0;
        if (payload[0]) val = atoi(payload);
        pinMode(pin, OUTPUT);
        digitalWrite(pin, val ? HIGH : LOW);
        if (msg->reply_len > 0)
            nats_msg_respond_str(client, msg, "ok");
    } else {
        halError(client, msg, "bad_action", "use .get or .set");
    }
}

/*============================================================================
 * Handler: adc.{pin}.read
 *============================================================================*/

static void halAdc(nats_client_t *client, const nats_msg_t *msg,
                   const char *rest, const char *payload) {
    (void)payload;
    if (!rest || !*rest) {
        halError(client, msg, "bad_request", "adc.{pin}.read");
        return;
    }

    char pinStr[8];
    const char *dot = strchr(rest, '.');
    size_t plen = dot ? (size_t)(dot - rest) : strlen(rest);
    if (plen >= sizeof(pinStr)) {
        halError(client, msg, "bad_request", "pin too long");
        return;
    }
    memcpy(pinStr, rest, plen);
    pinStr[plen] = '\0';

    int pin = parsePin(pinStr);
    if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) {
        halError(client, msg, "bad_pin",
                 pin < 0 ? "invalid pin number" : "pin out of range");
        return;
    }

    int val = analogRead(pin);
    snprintf(g_hal_reply, sizeof(g_hal_reply), "%d", val);
    if (msg->reply_len > 0)
        nats_msg_respond_str(client, msg, g_hal_reply);
}

/*============================================================================
 * Handler: pwm.{pin}.set / pwm.{pin}.get
 *============================================================================*/

static void halPwm(nats_client_t *client, const nats_msg_t *msg,
                   const char *rest, const char *payload) {
    if (!rest || !*rest) {
        halError(client, msg, "bad_request", "pwm.{pin}.set or pwm.{pin}.get");
        return;
    }

    char pinStr[8];
    const char *dot = strchr(rest, '.');
    if (!dot) {
        halError(client, msg, "bad_request", "missing .set or .get suffix");
        return;
    }
    size_t plen = dot - rest;
    if (plen >= sizeof(pinStr)) {
        halError(client, msg, "bad_request", "pin too long");
        return;
    }
    memcpy(pinStr, rest, plen);
    pinStr[plen] = '\0';

    int pin = parsePin(pinStr);
    if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) {
        halError(client, msg, "bad_pin",
                 pin < 0 ? "invalid pin number" : "pin out of range");
        return;
    }

    const char *action = dot + 1;

    if (strcmp(action, "set") == 0) {
        int val = payload[0] ? atoi(payload) : 0;
        val = constrain(val, 0, 255);
        analogWrite(pin, val);
        s_pwm_state[pin] = (uint8_t)val;
        if (msg->reply_len > 0)
            nats_msg_respond_str(client, msg, "ok");
    } else if (strcmp(action, "get") == 0) {
        snprintf(g_hal_reply, sizeof(g_hal_reply), "%d", s_pwm_state[pin]);
        if (msg->reply_len > 0)
            nats_msg_respond_str(client, msg, g_hal_reply);
    } else {
        halError(client, msg, "bad_action", "use .set or .get");
    }
}

/*============================================================================
 * Handler: dac — not available on C6/S3/C3
 *============================================================================*/

static void halDac(nats_client_t *client, const nats_msg_t *msg,
                   const char *rest, const char *payload) {
    (void)rest; (void)payload;
    halError(client, msg, "no_dac", "DAC not available on this chip");
}

/*============================================================================
 * Handler: uart.read / uart.write
 *============================================================================*/

static void halUart(nats_client_t *client, const nats_msg_t *msg,
                    const char *rest, const char *payload) {
    if (!rest || !*rest) {
        halError(client, msg, "bad_request", "uart.read or uart.write");
        return;
    }

    if (strcmp(rest, "read") == 0) {
        if (!serialTextActive()) {
            halError(client, msg, "no_uart", "no serial_text device registered");
            return;
        }
        const char *line = serialTextGetMsg();
        snprintf(g_hal_reply, sizeof(g_hal_reply), "%s", line);
        if (msg->reply_len > 0)
            nats_msg_respond_str(client, msg, g_hal_reply);
    } else if (strcmp(rest, "write") == 0) {
        if (!serialTextActive()) {
            halError(client, msg, "no_uart", "no serial_text device registered");
            return;
        }
        serialTextSend(payload);
        if (msg->reply_len > 0)
            nats_msg_respond_str(client, msg, "ok");
    } else {
        halError(client, msg, "bad_action", "use uart.read or uart.write");
    }
}

/*============================================================================
 * Handler: system.temperature / system.heap / system.uptime
 *============================================================================*/

static void halSystem(nats_client_t *client, const nats_msg_t *msg,
                      const char *rest, const char *payload) {
    (void)payload;
    if (!rest || !*rest) {
        halError(client, msg, "bad_request",
                 "system.temperature, system.heap, or system.uptime");
        return;
    }

    if (strcmp(rest, "temperature") == 0) {
#if !defined(CONFIG_IDF_TARGET_ESP32)
        float temp = 0.0f;
        if (g_temp_sensor)
            temperature_sensor_get_celsius(g_temp_sensor, &temp);
        snprintf(g_hal_reply, sizeof(g_hal_reply), "%.1f", temp);
#else
        snprintf(g_hal_reply, sizeof(g_hal_reply), "unsupported");
#endif
    } else if (strcmp(rest, "heap") == 0) {
        snprintf(g_hal_reply, sizeof(g_hal_reply), "%u", ESP.getFreeHeap());
    } else if (strcmp(rest, "uptime") == 0) {
        snprintf(g_hal_reply, sizeof(g_hal_reply), "%lu", millis() / 1000);
    } else {
        halError(client, msg, "bad_key",
                 "use temperature, heap, or uptime");
        return;
    }

    if (msg->reply_len > 0)
        nats_msg_respond_str(client, msg, g_hal_reply);
}

/*============================================================================
 * Handler: device.list — JSON array of all registered devices
 *============================================================================*/

static void halDevice(nats_client_t *client, const nats_msg_t *msg,
                      const char *rest, const char *payload) {
    (void)payload;
    /* Only "list" sub-command for now */
    if (rest && strcmp(rest, "list") != 0) {
        halError(client, msg, "bad_action", "use device.list");
        return;
    }

    int w = 0;
    w += snprintf(toolCallJsonBuf + w, sizeof(toolCallJsonBuf) - w, "[");

    Device *devs = deviceGetAll();
    bool first = true;
    for (int i = 0; i < MAX_DEVICES && w < (int)sizeof(toolCallJsonBuf) - 200; i++) {
        if (!devs[i].used) continue;
        Device *d = &devs[i];
        if (!first) toolCallJsonBuf[w++] = ',';
        first = false;

        if (deviceIsSensor(d->kind)) {
            float val = deviceReadSensor(d);
            w += snprintf(toolCallJsonBuf + w, sizeof(toolCallJsonBuf) - w,
                "{\"name\":\"%s\",\"kind\":\"%s\",\"value\":%.1f,\"unit\":\"%s\"}",
                d->name, deviceKindName(d->kind), val, d->unit);
        } else {
            w += snprintf(toolCallJsonBuf + w, sizeof(toolCallJsonBuf) - w,
                "{\"name\":\"%s\",\"kind\":\"%s\",\"pin\":%d,\"value\":%d}",
                d->name, deviceKindName(d->kind), d->pin, d->last_value);
        }
    }

    w += snprintf(toolCallJsonBuf + w, sizeof(toolCallJsonBuf) - w, "]");

    if (msg->reply_len > 0)
        nats_msg_respond_str(client, msg, toolCallJsonBuf);
}

/*============================================================================
 * Fallback: look up registered sensor/actuator by name
 * {sensor}       -> read sensor value
 * {sensor}.info  -> JSON with device details
 * {actuator}.set -> set actuator value (payload)
 * {actuator}.get -> read last actuator value
 *============================================================================*/

static void halDeviceLookup(nats_client_t *client, const nats_msg_t *msg,
                            const char *name_and_suffix, const char *payload) {
    /* Split "name" and optional ".info" / ".set" / ".get" suffix */
    char devName[DEV_NAME_LEN];
    const char *suffix = nullptr;
    const char *dot = strchr(name_and_suffix, '.');
    if (dot) {
        size_t nlen = dot - name_and_suffix;
        if (nlen >= sizeof(devName)) nlen = sizeof(devName) - 1;
        memcpy(devName, name_and_suffix, nlen);
        devName[nlen] = '\0';
        suffix = dot + 1;
    } else {
        strncpy(devName, name_and_suffix, sizeof(devName) - 1);
        devName[sizeof(devName) - 1] = '\0';
    }

    Device *dev = deviceFind(devName);
    if (!dev) {
        halError(client, msg, "not_found", devName);
        return;
    }

    if (suffix && strcmp(suffix, "info") == 0) {
        /* Build JSON info */
        if (deviceIsSensor(dev->kind)) {
            float val = deviceReadSensor(dev);
            snprintf(g_hal_reply, sizeof(g_hal_reply),
                "{\"name\":\"%s\",\"kind\":\"%s\",\"unit\":\"%s\","
                "\"value\":%.1f,\"pin\":%d}",
                dev->name, deviceKindName(dev->kind), dev->unit,
                val, dev->pin);
        } else {
            snprintf(g_hal_reply, sizeof(g_hal_reply),
                "{\"name\":\"%s\",\"kind\":\"%s\",\"pin\":%d,\"value\":%d}",
                dev->name, deviceKindName(dev->kind), dev->pin, dev->last_value);
        }
        if (msg->reply_len > 0)
            nats_msg_respond_str(client, msg, g_hal_reply);
        return;
    }

    if (suffix && strcmp(suffix, "set") == 0) {
        if (!deviceIsActuator(dev->kind)) {
            halError(client, msg, "not_actuator", devName);
            return;
        }
        int val = payload[0] ? atoi(payload) : 0;
        deviceSetActuator(dev, val);
        if (msg->reply_len > 0)
            nats_msg_respond_str(client, msg, "ok");
        return;
    }

    if (suffix && strcmp(suffix, "get") == 0) {
        if (deviceIsActuator(dev->kind)) {
            snprintf(g_hal_reply, sizeof(g_hal_reply), "%d", dev->last_value);
        } else {
            float val = deviceReadSensor(dev);
            snprintf(g_hal_reply, sizeof(g_hal_reply), "%.1f", val);
        }
        if (msg->reply_len > 0)
            nats_msg_respond_str(client, msg, g_hal_reply);
        return;
    }

    /* No suffix: read sensor or get actuator state */
    if (deviceIsSensor(dev->kind)) {
        float val = deviceReadSensor(dev);
        snprintf(g_hal_reply, sizeof(g_hal_reply), "%.1f", val);
    } else {
        snprintf(g_hal_reply, sizeof(g_hal_reply), "%d", dev->last_value);
    }
    if (msg->reply_len > 0)
        nats_msg_respond_str(client, msg, g_hal_reply);
}

/*============================================================================
 * Public API
 *============================================================================*/

bool halIsReservedName(const char *name) {
    if (!name) return false;
    for (size_t i = 0; i < HAL_RESERVED_COUNT; i++) {
        if (strcmp(name, HAL_RESERVED[i]) == 0) return true;
    }
    return false;
}

uint8_t halPwmGet(uint8_t pin) {
    if (pin >= SOC_GPIO_PIN_COUNT) return 0;
    return s_pwm_state[pin];
}

void halPwmSet(uint8_t pin, uint8_t value) {
    if (pin >= SOC_GPIO_PIN_COUNT) return;
    analogWrite(pin, value);
    s_pwm_state[pin] = value;
}

void onNatsHal(nats_client_t *client, const nats_msg_t *msg, void *userdata) {
    (void)userdata;

    /* Skip prefix: "{device_name}.hal." */
    if ((int)msg->subject_len <= halPrefixLen()) return;
    const char *suffix = msg->subject + halPrefixLen();

    /* Copy payload into null-terminated stack buffer */
    char payload[64];
    size_t plen = msg->data_len < sizeof(payload) - 1
                  ? msg->data_len : sizeof(payload) - 1;
    if (msg->data && plen > 0)
        memcpy(payload, msg->data, plen);
    payload[plen] = '\0';

    if (g_debug)
        Serial.printf("[NATS] hal: %s (payload='%s')\n", suffix, payload);

    /* Extract first segment */
    char segment[24];
    const char *dot = strchr(suffix, '.');
    size_t slen = dot ? (size_t)(dot - suffix) : strlen(suffix);
    if (slen >= sizeof(segment)) slen = sizeof(segment) - 1;
    memcpy(segment, suffix, slen);
    segment[slen] = '\0';

    const char *rest = dot ? dot + 1 : nullptr;

    /* Route to handler */
    if (strcmp(segment, "gpio") == 0)        halGpio(client, msg, rest, payload);
    else if (strcmp(segment, "adc") == 0)    halAdc(client, msg, rest, payload);
    else if (strcmp(segment, "pwm") == 0)    halPwm(client, msg, rest, payload);
    else if (strcmp(segment, "dac") == 0)    halDac(client, msg, rest, payload);
    else if (strcmp(segment, "uart") == 0)   halUart(client, msg, rest, payload);
    else if (strcmp(segment, "system") == 0) halSystem(client, msg, rest, payload);
    else if (strcmp(segment, "device") == 0) halDevice(client, msg, rest, payload);
    else                                     halDeviceLookup(client, msg, suffix, payload);
}
