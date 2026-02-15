/**
 * @file devices.cpp
 * @brief Device registry - named sensors and actuators with persistence
 */

#include "devices.h"
#include "llm_client.h"
#include <LittleFS.h>
#if !defined(CONFIG_IDF_TARGET_ESP32)
#include "driver/temperature_sensor.h"
extern temperature_sensor_handle_t g_temp_sensor;
#endif
#include <math.h>
extern bool g_debug;

static Device g_devices[MAX_DEVICES];

/*============================================================================
 * Kind helpers
 *============================================================================*/

bool deviceIsSensor(DeviceKind kind) {
    return kind <= DEV_SENSOR_SERIAL_TEXT;
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
        case DEV_SENSOR_CLOCK_HOUR:    return "clock_hour";
        case DEV_SENSOR_CLOCK_MINUTE:  return "clock_minute";
        case DEV_SENSOR_CLOCK_HHMM:   return "clock_hhmm";
        case DEV_SENSOR_NATS_VALUE:    return "nats_value";
        case DEV_SENSOR_SERIAL_TEXT:   return "serial_text";
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
    if (strcmp(s, "clock_hour") == 0)    return DEV_SENSOR_CLOCK_HOUR;
    if (strcmp(s, "clock_minute") == 0)  return DEV_SENSOR_CLOCK_MINUTE;
    if (strcmp(s, "clock_hhmm") == 0)   return DEV_SENSOR_CLOCK_HHMM;
    if (strcmp(s, "nats_value") == 0)    return DEV_SENSOR_NATS_VALUE;
    if (strcmp(s, "serial_text") == 0)   return DEV_SENSOR_SERIAL_TEXT;
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

Device *deviceGetAllMutable() {
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
                    const char *unit, bool inverted,
                    const char *nats_subject, uint32_t baud) {
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

            /* NATS virtual sensor fields */
            if (nats_subject && nats_subject[0]) {
                strncpy(g_devices[i].nats_subject, nats_subject, sizeof(g_devices[i].nats_subject) - 1);
                g_devices[i].nats_subject[sizeof(g_devices[i].nats_subject) - 1] = '\0';
            } else {
                g_devices[i].nats_subject[0] = '\0';
            }
            g_devices[i].nats_value = 0.0f;
            g_devices[i].nats_msg[0] = '\0';
            g_devices[i].nats_sid = 0;
            g_devices[i].baud = baud;

            /* Initialize serial_text UART */
            if (kind == DEV_SENSOR_SERIAL_TEXT) {
                serialTextInit(baud);
            }

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
    if (dev->kind == DEV_SENSOR_SERIAL_TEXT) {
        serialTextDeinit();
    }
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
#if !defined(CONFIG_IDF_TARGET_ESP32)
            if (g_temp_sensor)
                temperature_sensor_get_celsius(g_temp_sensor, &t);
#endif
            return t;
        }

        case DEV_SENSOR_CLOCK_HOUR: {
            struct tm timeinfo;
            if (getLocalTime(&timeinfo, 0))
                return (float)timeinfo.tm_hour;
            return -1.0f;
        }

        case DEV_SENSOR_CLOCK_MINUTE: {
            struct tm timeinfo;
            if (getLocalTime(&timeinfo, 0))
                return (float)timeinfo.tm_min;
            return -1.0f;
        }

        case DEV_SENSOR_CLOCK_HHMM: {
            struct tm timeinfo;
            if (getLocalTime(&timeinfo, 0))
                return (float)(timeinfo.tm_hour * 100 + timeinfo.tm_min);
            return -1.0f;
        }

        case DEV_SENSOR_NATS_VALUE:
            return dev->nats_value;

        case DEV_SENSOR_SERIAL_TEXT:
            return serialTextGetValue();

        default:
            return 0.0f;
    }
}

/*============================================================================
 * Actuator Control
 *============================================================================*/

bool deviceSetActuator(Device *dev, int value) {
    if (!dev || !dev->used || !deviceIsActuator(dev->kind)) return false;
    if (dev->pin == PIN_NONE) return false;

    dev->last_value = value;

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
 * NATS Virtual Sensor Helpers
 *============================================================================*/

void deviceSetNatsValue(Device *dev, float value, const char *msg) {
    if (!dev) return;
    dev->nats_value = value;
    if (msg) {
        strncpy(dev->nats_msg, msg, sizeof(dev->nats_msg) - 1);
        dev->nats_msg[sizeof(dev->nats_msg) - 1] = '\0';
    } else {
        dev->nats_msg[0] = '\0';
    }
}

const char *deviceGetNatsMsg(const Device *dev) {
    if (!dev || dev->kind != DEV_SENSOR_NATS_VALUE) return "";
    return dev->nats_msg;
}

void parseNatsPayload(const uint8_t *data, size_t len,
                      float *out_value, char *out_msg, size_t msg_len) {
    *out_value = 0.0f;
    out_msg[0] = '\0';

    if (len == 0 || !data) return;

    /* Null-terminate into a stack buffer */
    char buf[256];
    size_t copy = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, data, copy);
    buf[copy] = '\0';

    /* Skip leading whitespace */
    const char *p = buf;
    while (*p == ' ' || *p == '\t') p++;

    /* 1. Try bare number */
    char *endp = nullptr;
    float v = strtof(p, &endp);
    if (endp != p && (*endp == '\0' || *endp == ' ' || *endp == '\t' ||
                       *endp == '\n' || *endp == '\r')) {
        *out_value = v;
        return;
    }

    /* 2. Try JSON with "value" field */
    if (*p == '{') {
        const char *vk = strstr(p, "\"value\"");
        if (vk) {
            vk += 7;
            while (*vk == ' ' || *vk == ':') vk++;
            *out_value = strtof(vk, nullptr);
        }
        /* Extract "message" field if present */
        const char *mk = strstr(p, "\"message\"");
        if (mk) {
            mk += 9;
            while (*mk == ' ' || *mk == ':') mk++;
            if (*mk == '"') {
                mk++;
                size_t w = 0;
                while (*mk && *mk != '"' && w < msg_len - 1) {
                    if (*mk == '\\' && *(mk + 1)) mk++;
                    out_msg[w++] = *mk++;
                }
                out_msg[w] = '\0';
            }
        }
        return;
    }

    /* 3. Boolean-ish strings */
    if (strcasecmp(p, "on") == 0 || strcasecmp(p, "true") == 0 ||
        strcmp(p, "1") == 0) {
        *out_value = 1.0f;
        return;
    }
    if (strcasecmp(p, "off") == 0 || strcasecmp(p, "false") == 0 ||
        strcmp(p, "0") == 0) {
        *out_value = 0.0f;
        return;
    }

    /* 4. Anything else -> 0.0 */
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
            "{\"n\":\"%s\",\"k\":\"%s\",\"p\":%d,\"u\":\"%s\",\"i\":%s",
            d->name, deviceKindName(d->kind), d->pin,
            d->unit, d->inverted ? "true" : "false");
        if (d->nats_subject[0]) {
            w += snprintf(buf + w, sizeof(buf) - w,
                ",\"ns\":\"%s\"", d->nats_subject);
        }
        if (d->baud > 0) {
            w += snprintf(buf + w, sizeof(buf) - w,
                ",\"bd\":%u", (unsigned)d->baud);
        }
        w += snprintf(buf + w, sizeof(buf) - w, "}");

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

        char nats_subj[32] = "";
        devJsonGetString(objBuf, "ns", nats_subj, sizeof(nats_subj));
        uint32_t baud = (uint32_t)devJsonGetInt(objBuf, "bd", 0);

        DeviceKind kind = kindFromString(kind_str);
        deviceRegister(name, kind, (uint8_t)pin, unit, inverted,
                       nats_subj[0] ? nats_subj : nullptr, baud);
        count++;

        p = obj_end + 1;
    }

    Serial.printf("Devices: loaded %d from /devices.json\n", count);
}

/*============================================================================
 * Serial Text UART (one device max, UART1 on fixed pins)
 *============================================================================*/

static HardwareSerial SerialText(1);
static bool  g_serial_text_active = false;
static float g_serial_text_value  = 0.0f;
static char  g_serial_text_msg[64];
static char  g_serial_text_buf[128];
static int   g_serial_text_pos = 0;

void serialTextInit(uint32_t baud) {
    if (g_serial_text_active) return;
    if (baud == 0) baud = 9600;
    SerialText.begin(baud, SERIAL_8N1, SERIAL_TEXT_RX, SERIAL_TEXT_TX);
    g_serial_text_active = true;
    g_serial_text_pos = 0;
    g_serial_text_msg[0] = '\0';
    g_serial_text_value = 0.0f;
    Serial.printf("SerialText: UART1 at %u baud (RX=%d TX=%d)\n",
                  baud, SERIAL_TEXT_RX, SERIAL_TEXT_TX);
}

void serialTextDeinit() {
    if (!g_serial_text_active) return;
    SerialText.end();
    g_serial_text_active = false;
    g_serial_text_pos = 0;
    g_serial_text_msg[0] = '\0';
    g_serial_text_value = 0.0f;
    Serial.println("SerialText: stopped");
}

void serialTextPoll() {
    if (!g_serial_text_active) return;

    while (SerialText.available()) {
        char c = SerialText.read();

        if (c == '\n') {
            if (g_serial_text_pos == 0) continue;
            g_serial_text_buf[g_serial_text_pos] = '\0';

            /* Parse value + message using same logic as NATS */
            parseNatsPayload((const uint8_t *)g_serial_text_buf,
                             g_serial_text_pos,
                             &g_serial_text_value,
                             g_serial_text_msg, sizeof(g_serial_text_msg));

            /* If msg empty, store raw line as msg */
            if (g_serial_text_msg[0] == '\0') {
                strncpy(g_serial_text_msg, g_serial_text_buf,
                        sizeof(g_serial_text_msg) - 1);
                g_serial_text_msg[sizeof(g_serial_text_msg) - 1] = '\0';
            }

            if (g_debug) {
                Serial.printf("[SerialText] '%s' -> val=%.1f msg='%s'\n",
                              g_serial_text_buf, g_serial_text_value,
                              g_serial_text_msg);
            }
            g_serial_text_pos = 0;
            continue;
        }

        if (c == '\r') continue;

        if (g_serial_text_pos < (int)sizeof(g_serial_text_buf) - 1) {
            g_serial_text_buf[g_serial_text_pos++] = c;
        }
    }
}

bool serialTextSend(const char *text) {
    if (!g_serial_text_active || !text) return false;
    SerialText.print(text);
    /* Append newline if not already present */
    size_t len = strlen(text);
    if (len == 0 || text[len - 1] != '\n') {
        SerialText.print('\n');
    }
    return true;
}

const char *serialTextGetMsg() {
    return g_serial_text_msg;
}

float serialTextGetValue() {
    return g_serial_text_value;
}

bool serialTextActive() {
    return g_serial_text_active;
}

/*============================================================================
 * Init
 *============================================================================*/

void devicesInit() {
    memset(g_devices, 0, sizeof(g_devices));

    devicesLoad();

    /* Auto-register built-in virtual sensors if not already present */
    bool changed = false;
    if (!deviceFind("chip_temp")) {
        deviceRegister("chip_temp", DEV_SENSOR_INTERNAL_TEMP, PIN_NONE, "C", false);
        changed = true;
    }
    if (!deviceFind("clock_hour")) {
        deviceRegister("clock_hour", DEV_SENSOR_CLOCK_HOUR, PIN_NONE, "h", false);
        changed = true;
    }
    if (!deviceFind("clock_minute")) {
        deviceRegister("clock_minute", DEV_SENSOR_CLOCK_MINUTE, PIN_NONE, "m", false);
        changed = true;
    }
    if (!deviceFind("clock_hhmm")) {
        deviceRegister("clock_hhmm", DEV_SENSOR_CLOCK_HHMM, PIN_NONE, "", false);
        changed = true;
    }
    if (changed) devicesSave();

    int count = 0;
    for (int i = 0; i < MAX_DEVICES; i++)
        if (g_devices[i].used) count++;
    Serial.printf("Devices: %d registered\n", count);
}
