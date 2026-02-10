/**
 * @file rules.cpp
 * @brief Local rule engine — persistent sensor->actuator automation
 */

#include "rules.h"
#include "devices.h"
#include "llm_client.h"
#include <LittleFS.h>
#include <nats_atoms.h>

/* Externs from main.cpp */
extern void led(uint8_t r, uint8_t g, uint8_t b);
extern bool g_led_user;
extern NatsClient natsClient;
extern bool g_nats_connected;
extern bool g_debug;
extern bool g_telegram_enabled;
extern int cfg_telegram_cooldown;
extern bool tgSendMessage(const char *text);

/* NATS events subject — built from device name in main.cpp */
extern char natsSubjectEvents[];

static Rule g_rules[MAX_RULES];
static int g_rule_counter = 0; /* auto-increment for IDs */

/*============================================================================
 * Name helpers
 *============================================================================*/

const char *conditionOpName(ConditionOp op) {
    switch (op) {
        case COND_GT:     return "gt";
        case COND_LT:     return "lt";
        case COND_EQ:     return "eq";
        case COND_NEQ:    return "neq";
        case COND_CHANGE: return "change";
        case COND_ALWAYS: return "always";
        default:          return "?";
    }
}

static ConditionOp conditionFromString(const char *s) {
    if (strcmp(s, "gt") == 0)     return COND_GT;
    if (strcmp(s, "lt") == 0)     return COND_LT;
    if (strcmp(s, "eq") == 0)     return COND_EQ;
    if (strcmp(s, "neq") == 0)    return COND_NEQ;
    if (strcmp(s, "change") == 0) return COND_CHANGE;
    if (strcmp(s, "always") == 0) return COND_ALWAYS;
    return COND_GT;
}

const char *actionTypeName(ActionType act) {
    switch (act) {
        case ACT_GPIO_WRITE:   return "gpio_write";
        case ACT_LED_SET:      return "led_set";
        case ACT_NATS_PUBLISH: return "nats_publish";
        case ACT_ACTUATOR:     return "actuator";
        case ACT_TELEGRAM:     return "telegram";
        default:               return "?";
    }
}

static ActionType actionFromString(const char *s) {
    if (strcmp(s, "gpio_write") == 0)   return ACT_GPIO_WRITE;
    if (strcmp(s, "led_set") == 0)      return ACT_LED_SET;
    if (strcmp(s, "nats_publish") == 0) return ACT_NATS_PUBLISH;
    if (strcmp(s, "actuator") == 0)     return ACT_ACTUATOR;
    if (strcmp(s, "telegram") == 0)     return ACT_TELEGRAM;
    return ACT_GPIO_WRITE;
}

/*============================================================================
 * CRUD
 *============================================================================*/

const Rule *ruleGetAll() {
    return g_rules;
}

Rule *ruleFind(const char *id) {
    for (int i = 0; i < MAX_RULES; i++) {
        if (g_rules[i].used && strcmp(g_rules[i].id, id) == 0)
            return &g_rules[i];
    }
    return nullptr;
}

const char *ruleCreate(const char *name, const char *sensor_name, uint8_t sensor_pin,
                       bool sensor_analog, ConditionOp condition, int32_t threshold,
                       uint32_t interval_ms,
                       ActionType on_action, const char *on_actuator,
                       uint8_t on_pin, int32_t on_value,
                       const char *on_nats_subj, const char *on_nats_pay,
                       bool has_off, ActionType off_action, const char *off_actuator,
                       uint8_t off_pin, int32_t off_value,
                       const char *off_nats_subj, const char *off_nats_pay) {
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_RULES; i++) {
        if (!g_rules[i].used) { slot = i; break; }
    }
    if (slot < 0) return nullptr;

    Rule *r = &g_rules[slot];
    memset(r, 0, sizeof(Rule));

    /* Assign ID */
    g_rule_counter++;
    snprintf(r->id, RULE_ID_LEN, "rule_%02d", g_rule_counter);

    strncpy(r->name, name, RULE_NAME_LEN - 1);

    /* Sensor */
    if (sensor_name && sensor_name[0]) {
        strncpy(r->sensor_name, sensor_name, DEV_NAME_LEN - 1);
        r->sensor_pin = PIN_NONE;
    } else {
        r->sensor_name[0] = '\0';
        r->sensor_pin = sensor_pin;
        r->sensor_analog = sensor_analog;
    }

    r->condition = condition;
    r->threshold = threshold;
    r->interval_ms = interval_ms > 0 ? interval_ms : 5000;

    /* ON action */
    r->on_action = on_action;
    if (on_actuator && on_actuator[0])
        strncpy(r->on_actuator, on_actuator, DEV_NAME_LEN - 1);
    r->on_pin = on_pin;
    r->on_value = on_value;
    if (on_nats_subj && on_nats_subj[0])
        strncpy(r->on_nats_subj, on_nats_subj, RULE_NATS_SUBJ_LEN - 1);
    if (on_nats_pay && on_nats_pay[0])
        strncpy(r->on_nats_pay, on_nats_pay, RULE_NATS_PAY_LEN - 1);

    /* OFF action */
    r->has_off_action = has_off;
    if (has_off) {
        r->off_action = off_action;
        if (off_actuator && off_actuator[0])
            strncpy(r->off_actuator, off_actuator, DEV_NAME_LEN - 1);
        r->off_pin = off_pin;
        r->off_value = off_value;
        if (off_nats_subj && off_nats_subj[0])
            strncpy(r->off_nats_subj, off_nats_subj, RULE_NATS_SUBJ_LEN - 1);
        if (off_nats_pay && off_nats_pay[0])
            strncpy(r->off_nats_pay, off_nats_pay, RULE_NATS_PAY_LEN - 1);
    }

    r->fired = false;
    r->last_eval = 0;
    r->last_reading = 0;
    r->enabled = true;
    r->used = true;

    return r->id;
}

bool ruleDelete(const char *id) {
    if (strcmp(id, "all") == 0) {
        for (int i = 0; i < MAX_RULES; i++) {
            g_rules[i].used = false;
        }
        return true;
    }

    Rule *r = ruleFind(id);
    if (!r) return false;
    r->used = false;
    return true;
}

bool ruleEnable(const char *id, bool enable) {
    Rule *r = ruleFind(id);
    if (!r) return false;
    r->enabled = enable;
    r->fired = false; /* Reset triggered state when toggling */
    return true;
}

/*============================================================================
 * Action Execution
 *============================================================================*/

static void executeAction(Rule *r, bool is_on) {
    ActionType action = is_on ? r->on_action : r->off_action;

    switch (action) {
        case ACT_GPIO_WRITE: {
            uint8_t pin = is_on ? r->on_pin : r->off_pin;
            int32_t val = is_on ? r->on_value : r->off_value;
            pinMode(pin, OUTPUT);
            digitalWrite(pin, val ? HIGH : LOW);
            if (g_debug) Serial.printf("[Rule] %s: GPIO %d = %d\n", r->id, pin, (int)val);
            break;
        }
        case ACT_LED_SET: {
            int32_t val = is_on ? r->on_value : r->off_value;
            uint8_t lr = (val >> 16) & 0xFF;
            uint8_t lg = (val >> 8) & 0xFF;
            uint8_t lb = val & 0xFF;
            led(lr, lg, lb);
            g_led_user = true;
            if (g_debug) Serial.printf("[Rule] %s: LED(%d,%d,%d)\n", r->id, lr, lg, lb);
            break;
        }
        case ACT_NATS_PUBLISH: {
            if (!g_nats_connected) break;
            const char *subj = is_on ? r->on_nats_subj : r->off_nats_subj;
            const char *pay = is_on ? r->on_nats_pay : r->off_nats_pay;
            if (subj[0]) natsClient.publish(subj, pay);
            if (g_debug) Serial.printf("[Rule] %s: NATS %s: %s\n", r->id, subj, pay);
            break;
        }
        case ACT_ACTUATOR: {
            const char *aname = is_on ? r->on_actuator : r->off_actuator;
            Device *dev = deviceFind(aname);
            if (dev) {
                deviceSetActuator(dev, is_on ? 1 : 0);
                if (g_debug) Serial.printf("[Rule] %s: actuator '%s' = %d\n",
                                           r->id, aname, is_on ? 1 : 0);
            }
            break;
        }
        case ACT_TELEGRAM: {
            if (!g_telegram_enabled) break;
            uint32_t now = millis();
            uint32_t cooldown_ms = (uint32_t)cfg_telegram_cooldown * 1000;
            if (cooldown_ms > 0 && now - r->last_telegram_ms < cooldown_ms) {
                if (g_debug) Serial.printf("[Rule] %s: Telegram cooldown, skipping\n", r->id);
                break;
            }
            const char *msg = is_on ? r->on_nats_pay : r->off_nats_pay;
            if (msg[0]) {
                tgSendMessage(msg);
                r->last_telegram_ms = now;
                if (g_debug) Serial.printf("[Rule] %s: Telegram: %s\n", r->id, msg);
            }
            break;
        }
    }
}

static void publishRuleEvent(const Rule *r, bool is_on) {
    if (!g_nats_connected) return;
    if (natsSubjectEvents[0] == '\0') return;

    static char eventBuf[192];
    snprintf(eventBuf, sizeof(eventBuf),
        "{\"event\":\"rule\",\"rule\":\"%s\",\"state\":\"%s\",\"reading\":%d,\"threshold\":%d}",
        r->name, is_on ? "on" : "off", (int)r->last_reading, (int)r->threshold);
    natsClient.publish(natsSubjectEvents, eventBuf);
}

/*============================================================================
 * Evaluation
 *============================================================================*/

void rulesEvaluate() {
    uint32_t now = millis();

    /* Sensor read cache — each unique sensor read once per cycle */
    struct SensorCache { char name[DEV_NAME_LEN]; float value; };
    SensorCache cache[MAX_RULES];
    int cacheCount = 0;

    for (int i = 0; i < MAX_RULES; i++) {
        Rule *r = &g_rules[i];
        if (!r->used || !r->enabled) continue;

        /* Throttle by interval */
        if (now - r->last_eval < r->interval_ms) continue;
        r->last_eval = now;

        /* Read sensor (with cache for named devices) */
        float reading = 0.0f;
        if (r->sensor_name[0]) {
            /* Check cache first */
            bool found = false;
            for (int c = 0; c < cacheCount; c++) {
                if (strcmp(cache[c].name, r->sensor_name) == 0) {
                    reading = cache[c].value;
                    found = true;
                    break;
                }
            }
            if (!found) {
                Device *dev = deviceFind(r->sensor_name);
                if (dev) {
                    reading = deviceReadSensor(dev);
                    /* Store in cache */
                    if (cacheCount < MAX_RULES) {
                        strncpy(cache[cacheCount].name, r->sensor_name, DEV_NAME_LEN - 1);
                        cache[cacheCount].name[DEV_NAME_LEN - 1] = '\0';
                        cache[cacheCount].value = reading;
                        cacheCount++;
                    }
                } else {
                    continue; /* Device not found — skip */
                }
            }
        } else {
            /* Raw GPIO — no caching */
            if (r->sensor_analog) {
                reading = (float)analogRead(r->sensor_pin);
            } else {
                pinMode(r->sensor_pin, INPUT);
                reading = (float)digitalRead(r->sensor_pin);
            }
        }

        int32_t prev_reading = r->last_reading;
        r->last_reading = (int32_t)reading;

        /* Evaluate condition */
        bool condition_met = false;
        switch (r->condition) {
            case COND_GT:     condition_met = (reading > (float)r->threshold); break;
            case COND_LT:     condition_met = (reading < (float)r->threshold); break;
            case COND_EQ:     condition_met = ((int32_t)reading == r->threshold); break;
            case COND_NEQ:    condition_met = ((int32_t)reading != r->threshold); break;
            case COND_CHANGE: condition_met = ((int32_t)reading != prev_reading); break;
            case COND_ALWAYS: condition_met = true; break;
        }

        /* Edge-triggered: fire on transition */
        if (condition_met && !r->fired) {
            r->fired = true;
            executeAction(r, true);
            publishRuleEvent(r, true);
            Serial.printf("[Rule] %s '%s' TRIGGERED (reading=%d, threshold=%d)\n",
                          r->id, r->name, (int)r->last_reading, (int)r->threshold);
        } else if (!condition_met && r->fired) {
            r->fired = false;
            if (r->has_off_action) {
                executeAction(r, false);
            }
            publishRuleEvent(r, false);
            Serial.printf("[Rule] %s '%s' CLEARED (reading=%d)\n",
                          r->id, r->name, (int)r->last_reading);
        }
    }
}

/*============================================================================
 * JSON Persistence — /rules.json
 *============================================================================*/

/* Simple JSON helpers (same pattern as devices.cpp) */
static bool ruleJsonGetString(const char *json, const char *key,
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

static int ruleJsonGetInt(const char *json, const char *key, int default_val) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    return atoi(p);
}

static bool ruleJsonGetBool(const char *json, const char *key, bool default_val) {
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

void rulesSave() {
    static char buf[4096];
    int w = 0;

    w += snprintf(buf + w, sizeof(buf) - w, "[");

    bool first = true;
    for (int i = 0; i < MAX_RULES; i++) {
        if (!g_rules[i].used) continue;
        const Rule *r = &g_rules[i];

        if (!first) w += snprintf(buf + w, sizeof(buf) - w, ",");
        first = false;

        w += snprintf(buf + w, sizeof(buf) - w,
            "{\"id\":\"%s\",\"nm\":\"%s\",\"sn\":\"%s\",\"sp\":%d,\"sa\":%s,"
            "\"co\":\"%s\",\"th\":%d,\"iv\":%u,"
            "\"oa\":\"%s\",\"oac\":\"%s\",\"op\":%d,\"ov\":%d,"
            "\"ons\":\"%s\",\"onp\":\"%s\","
            "\"ho\":%s,\"fa\":\"%s\",\"fac\":\"%s\",\"fp\":%d,\"fv\":%d,"
            "\"fns\":\"%s\",\"fnp\":\"%s\","
            "\"en\":%s}",
            r->id, r->name, r->sensor_name, r->sensor_pin,
            r->sensor_analog ? "true" : "false",
            conditionOpName(r->condition), (int)r->threshold, (unsigned)r->interval_ms,
            actionTypeName(r->on_action), r->on_actuator, r->on_pin, (int)r->on_value,
            r->on_nats_subj, r->on_nats_pay,
            r->has_off_action ? "true" : "false",
            actionTypeName(r->off_action), r->off_actuator, r->off_pin, (int)r->off_value,
            r->off_nats_subj, r->off_nats_pay,
            r->enabled ? "true" : "false");

        if (w >= (int)sizeof(buf) - 1) break;
    }

    w += snprintf(buf + w, sizeof(buf) - w, "]");

    File f = LittleFS.open("/rules.json", "w");
    if (f) {
        f.print(buf);
        f.close();
    }

    if (g_debug) Serial.printf("Rules: saved to /rules.json (%d bytes)\n", w);
}

static void rulesLoad() {
    static char buf[4096];
    File f = LittleFS.open("/rules.json", "r");
    if (!f) return;

    int len = f.readBytes(buf, sizeof(buf) - 1);
    buf[len] = '\0';
    f.close();

    if (len <= 2) return;

    const char *p = buf;
    int count = 0;
    int max_counter = 0;

    while (*p && count < MAX_RULES) {
        const char *obj = strchr(p, '{');
        if (!obj) break;

        /* Find matching '}' — simple scan (no nested objects) */
        const char *obj_end = strchr(obj, '}');
        if (!obj_end) break;

        int obj_len = obj_end - obj + 1;
        static char objBuf[512];
        if (obj_len >= (int)sizeof(objBuf)) { p = obj_end + 1; continue; }
        memcpy(objBuf, obj, obj_len);
        objBuf[obj_len] = '\0';

        Rule *r = &g_rules[count];
        memset(r, 0, sizeof(Rule));

        ruleJsonGetString(objBuf, "id", r->id, RULE_ID_LEN);
        ruleJsonGetString(objBuf, "nm", r->name, RULE_NAME_LEN);
        ruleJsonGetString(objBuf, "sn", r->sensor_name, DEV_NAME_LEN);
        r->sensor_pin = (uint8_t)ruleJsonGetInt(objBuf, "sp", PIN_NONE);
        r->sensor_analog = ruleJsonGetBool(objBuf, "sa", false);

        char cond_str[16];
        ruleJsonGetString(objBuf, "co", cond_str, sizeof(cond_str));
        r->condition = conditionFromString(cond_str);
        r->threshold = ruleJsonGetInt(objBuf, "th", 0);
        r->interval_ms = (uint32_t)ruleJsonGetInt(objBuf, "iv", 5000);

        char act_str[24];
        ruleJsonGetString(objBuf, "oa", act_str, sizeof(act_str));
        r->on_action = actionFromString(act_str);
        ruleJsonGetString(objBuf, "oac", r->on_actuator, DEV_NAME_LEN);
        r->on_pin = (uint8_t)ruleJsonGetInt(objBuf, "op", 0);
        r->on_value = ruleJsonGetInt(objBuf, "ov", 0);
        ruleJsonGetString(objBuf, "ons", r->on_nats_subj, RULE_NATS_SUBJ_LEN);
        ruleJsonGetString(objBuf, "onp", r->on_nats_pay, RULE_NATS_PAY_LEN);

        r->has_off_action = ruleJsonGetBool(objBuf, "ho", false);
        ruleJsonGetString(objBuf, "fa", act_str, sizeof(act_str));
        r->off_action = actionFromString(act_str);
        ruleJsonGetString(objBuf, "fac", r->off_actuator, DEV_NAME_LEN);
        r->off_pin = (uint8_t)ruleJsonGetInt(objBuf, "fp", 0);
        r->off_value = ruleJsonGetInt(objBuf, "fv", 0);
        ruleJsonGetString(objBuf, "fns", r->off_nats_subj, RULE_NATS_SUBJ_LEN);
        ruleJsonGetString(objBuf, "fnp", r->off_nats_pay, RULE_NATS_PAY_LEN);

        r->enabled = ruleJsonGetBool(objBuf, "en", true);
        r->fired = false;
        r->last_eval = 0;
        r->last_reading = 0;
        r->used = true;

        /* Track highest counter for auto-increment */
        int num = atoi(r->id + 5); /* skip "rule_" */
        if (num > max_counter) max_counter = num;

        count++;
        p = obj_end + 1;
    }

    g_rule_counter = max_counter;
    Serial.printf("Rules: loaded %d from /rules.json\n", count);
}

void rulesInit() {
    memset(g_rules, 0, sizeof(g_rules));
    g_rule_counter = 0;
    rulesLoad();

    int count = 0;
    for (int i = 0; i < MAX_RULES; i++)
        if (g_rules[i].used) count++;
    Serial.printf("Rules: %d active\n", count);
}
