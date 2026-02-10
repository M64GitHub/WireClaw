/**
 * @file devices.cpp
 * @brief Device registry - named sensors and actuators with persistence
 */

#include "devices.h"
#include "llm_client.h"
#include <LittleFS.h>
#include "driver/temperature_sensor.h"
#include <math.h>

/* External: temperature sensor handle from main.cpp */
extern temperature_sensor_handle_t g_temp_sensor;
extern bool g_debug;

static Device g_devices[MAX_DEVICES];

/*============================================================================
 * Kind helpers
 *============================================================================*/

bool deviceIsSensor(DeviceKind kind) {
    return kind <= DEV_SENSOR_INTERNAL_TEMP;
}

bool deviceIsActuator(DeviceKind kind) {
    return kind >= DEV_ACTUATOR_DIGITAL;
}

const char *deviceKindName(DeviceKind kind) {
    switch (kind) {
        case DEV_SENSOR_DIGITAL:       return "digital_in";
        case DEV_SENSOR_ANALOG_RAW:    return "analog_in";
        case DEV_SENSOR_NTC_10K:       return "ntc_10k";
        case DEV_SENSOR_LDR:           return "ldr";
        case DEV_SENSOR_INTERNAL_TEMP: return "internal_temp";
        case DEV_ACTUATOR_DIGITAL:     return "digital_out";
        case DEV_ACTUATOR_RELAY:       return "relay";
        case DEV_ACTUATOR_PWM:         return "pwm";
        default:                       return "unknown";
    }
}

static DeviceKind kindFromString(const char *s) {
    if (strcmp(s, "digital_in") == 0)    return DEV_SENSOR_DIGITAL;
    if (strcmp(s, "analog_in") == 0)     return DEV_SENSOR_ANALOG_RAW;
    if (strcmp(s, "ntc_10k") == 0)       return DEV_SENSOR_NTC_10K;
    if (strcmp(s, "ldr") == 0)           return DEV_SENSOR_LDR;
    if (strcmp(s, "internal_temp") == 0) return DEV_SENSOR_INTERNAL_TEMP;
    if (strcmp(s, "digital_out") == 0)   return DEV_ACTUATOR_DIGITAL;
    if (strcmp(s, "relay") == 0)         return DEV_ACTUATOR_RELAY;
    if (strcmp(s, "pwm") == 0)           return DEV_ACTUATOR_PWM;
    return DEV_SENSOR_DIGITAL;
}

/*============================================================================
 * CRUD
 *============================================================================*/

const Device *deviceGetAll() {
    return g_devices;
}

Device *deviceFind(const char *name) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_devices[i].used && strcmp(g_devices[i].name, name) == 0)
            return &g_devices[i];
    }
    return nullptr;
}

bool deviceRegister(const char *name, DeviceKind kind, uint8_t pin,
                    const char *unit, bool inverted) {
    /* Check for duplicate */
    if (deviceFind(name)) return false;

    /* Find free slot */
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!g_devices[i].used) {
            strncpy(g_devices[i].name, name, DEV_NAME_LEN - 1);
            g_devices[i].name[DEV_NAME_LEN - 1] = '\0';
            g_devices[i].kind = kind;
            g_devices[i].pin = pin;
            if (unit) {
                strncpy(g_devices[i].unit, unit, DEV_UNIT_LEN - 1);
                g_devices[i].unit[DEV_UNIT_LEN - 1] = '\0';
            } else {
                g_devices[i].unit[0] = '\0';
            }
            g_devices[i].inverted = inverted;
            g_devices[i].used = true;

            /* Configure GPIO for actuators */
            if (deviceIsActuator(kind) && pin != PIN_NONE) {
                pinMode(pin, OUTPUT);
            }

            return true;
        }
    }
    return false; /* No free slot */
}

bool deviceRemove(const char *name) {
    Device *dev = deviceFind(name);
    if (!dev) return false;
    dev->used = false;
    dev->name[0] = '\0';
    return true;
}

/*============================================================================
 * Sensor Reading
 *============================================================================*/

float deviceReadSensor(const Device *dev) {
    if (!dev || !dev->used) return 0.0f;

    switch (dev->kind) {
        case DEV_SENSOR_DIGITAL:
            pinMode(dev->pin, INPUT);
            return (float)digitalRead(dev->pin);

        case DEV_SENSOR_ANALOG_RAW:
            return (float)analogRead(dev->pin);

        case DEV_SENSOR_NTC_10K: {
            int raw = analogRead(dev->pin);
            if (raw <= 0 || raw >= 4095) return -999.0f;
            float resistance = 10000.0f * raw / (4095.0f - raw);
            float tempK = 1.0f / (1.0f / 298.15f + (1.0f / 3950.0f) * logf(resistance / 10000.0f));
            return tempK - 273.15f;
        }

        case DEV_SENSOR_LDR: {
            int raw = analogRead(dev->pin);
            /* Rough lux estimate: higher ADC = more light */
            return (float)raw * 100.0f / 4095.0f;
        }

        case DEV_SENSOR_INTERNAL_TEMP: {
            float t = 0.0f;
            if (g_temp_sensor)
                temperature_sensor_get_celsius(g_temp_sensor, &t);
            return t;
        }

        default:
            return 0.0f;
    }
}

/*============================================================================
 * Actuator Control
 *============================================================================*/

bool deviceSetActuator(const Device *dev, int value) {
    if (!dev || !dev->used || !deviceIsActuator(dev->kind)) return false;
    if (dev->pin == PIN_NONE) return false;

    switch (dev->kind) {
        case DEV_ACTUATOR_DIGITAL:
            pinMode(dev->pin, OUTPUT);
            digitalWrite(dev->pin, value ? HIGH : LOW);
            return true;

        case DEV_ACTUATOR_RELAY:
            pinMode(dev->pin, OUTPUT);
            if (dev->inverted)
                digitalWrite(dev->pin, value ? LOW : HIGH);
            else
                digitalWrite(dev->pin, value ? HIGH : LOW);
            return true;

        case DEV_ACTUATOR_PWM:
            pinMode(dev->pin, OUTPUT);
            analogWrite(dev->pin, constrain(value, 0, 255));
            return true;

        default:
            return false;
    }
}

/*============================================================================
 * JSON Persistence - /devices.json
 *============================================================================*/

/* Simple JSON string extractor (matches pattern from main.cpp) */
static bool devJsonGetString(const char *json, const char *key,
                              char *dst, int dst_len) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    int w = 0;
    while (*p && *p != '"' && w < dst_len - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        dst[w++] = *p++;
    }
    dst[w] = '\0';
    return w > 0;
}

static int devJsonGetInt(const char *json, const char *key, int default_val) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    return atoi(p);
}

static bool devJsonGetBool(const char *json, const char *key, bool default_val) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return default_val;
}

void devicesSave() {
    static char buf[2048];
    int w = 0;

    w += snprintf(buf + w, sizeof(buf) - w, "[");

    bool first = true;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!g_devices[i].used) continue;
        const Device *d = &g_devices[i];

        if (!first) w += snprintf(buf + w, sizeof(buf) - w, ",");
        first = false;

        w += snprintf(buf + w, sizeof(buf) - w,
            "{\"n\":\"%s\",\"k\":\"%s\",\"p\":%d,\"u\":\"%s\",\"i\":%s}",
            d->name, deviceKindName(d->kind), d->pin,
            d->unit, d->inverted ? "true" : "false");

        if (w >= (int)sizeof(buf) - 1) break;
    }

    w += snprintf(buf + w, sizeof(buf) - w, "]");

    File f = LittleFS.open("/devices.json", "w");
    if (f) {
        f.print(buf);
        f.close();
    }

    if (g_debug) Serial.printf("Devices: saved to /devices.json (%d bytes)\n", w);
}

static void devicesLoad() {
    static char buf[2048];
    File f = LittleFS.open("/devices.json", "r");
    if (!f) return;

    int len = f.readBytes(buf, sizeof(buf) - 1);
    buf[len] = '\0';
    f.close();

    if (len <= 2) return;

    /* Parse array of device objects */
    const char *p = buf;
    int count = 0;

    while (*p && count < MAX_DEVICES) {
        /* Find next object */
        const char *obj = strchr(p, '{');
        if (!obj) break;

        /* Find end of object */
        const char *obj_end = strchr(obj, '}');
        if (!obj_end) break;

        /* Extract into a temporary null-terminated substring */
        int obj_len = obj_end - obj + 1;
        static char objBuf[256];
        if (obj_len >= (int)sizeof(objBuf)) { p = obj_end + 1; continue; }
        memcpy(objBuf, obj, obj_len);
        objBuf[obj_len] = '\0';

        char name[DEV_NAME_LEN];
        char kind_str[24];
        char unit[DEV_UNIT_LEN];

        if (!devJsonGetString(objBuf, "n", name, sizeof(name))) { p = obj_end + 1; continue; }
        if (!devJsonGetString(objBuf, "k", kind_str, sizeof(kind_str))) { p = obj_end + 1; continue; }

        int pin = devJsonGetInt(objBuf, "p", PIN_NONE);
        devJsonGetString(objBuf, "u", unit, sizeof(unit));
        bool inverted = devJsonGetBool(objBuf, "i", false);

        DeviceKind kind = kindFromString(kind_str);
        deviceRegister(name, kind, (uint8_t)pin, unit, inverted);
        count++;

        p = obj_end + 1;
    }

    Serial.printf("Devices: loaded %d from /devices.json\n", count);
}

void devicesInit() {
    memset(g_devices, 0, sizeof(g_devices));

    devicesLoad();

    /* Auto-register chip_temp if not already present */
    if (!deviceFind("chip_temp")) {
        deviceRegister("chip_temp", DEV_SENSOR_INTERNAL_TEMP, PIN_NONE, "C", false);
        devicesSave();
    }

    int count = 0;
    for (int i = 0; i < MAX_DEVICES; i++)
        if (g_devices[i].used) count++;
    Serial.printf("Devices: %d registered\n", count);
}
