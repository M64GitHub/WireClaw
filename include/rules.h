/**
 * @file rules.h
 * @brief Local rule engine for automated sensor->actuator logic
 *
 * Rules are evaluated in the main loop without LLM involvement.
 * Supports device registry names and raw GPIO pins.
 */

#ifndef RULES_H
#define RULES_H

#include <Arduino.h>
#include "devices.h"

#define MAX_RULES           16
#define RULE_NAME_LEN       32
#define RULE_NATS_SUBJ_LEN  64
#define RULE_NATS_PAY_LEN   64
#define RULE_ID_LEN         12

enum ConditionOp { COND_GT, COND_LT, COND_EQ, COND_NEQ, COND_CHANGE, COND_ALWAYS };
enum ActionType  { ACT_GPIO_WRITE, ACT_LED_SET, ACT_NATS_PUBLISH, ACT_ACTUATOR, ACT_TELEGRAM, ACT_SERIAL_SEND };

struct Rule {
    char id[RULE_ID_LEN];              /* "rule_01" etc. */
    char name[RULE_NAME_LEN];

    /* Sensor source */
    char sensor_name[DEV_NAME_LEN];    /* device name (preferred) */
    uint8_t sensor_pin;                 /* fallback raw GPIO */
    bool sensor_analog;                 /* for raw pin: analogRead vs digitalRead */

    /* Condition */
    ConditionOp condition;
    int32_t threshold;

    /* ON action */
    ActionType on_action;
    char on_actuator[DEV_NAME_LEN];    /* for ACT_ACTUATOR */
    uint8_t on_pin;                     /* for ACT_GPIO_WRITE */
    int32_t on_value;
    char on_nats_subj[RULE_NATS_SUBJ_LEN];
    char on_nats_pay[RULE_NATS_PAY_LEN];

    /* OFF action */
    bool has_off_action;
    ActionType off_action;
    char off_actuator[DEV_NAME_LEN];
    uint8_t off_pin;
    int32_t off_value;
    char off_nats_subj[RULE_NATS_SUBJ_LEN];
    char off_nats_pay[RULE_NATS_PAY_LEN];

    /* Timing */
    uint32_t interval_ms;
    uint32_t last_eval;                 /* runtime only, not persisted */
    uint32_t last_triggered;            /* millis() of last action fire, persisted */
    uint32_t last_telegram_ms;          /* runtime only, cooldown tracking */

    /* State */
    bool fired;                         /* runtime only */
    int32_t last_reading;               /* runtime only */
    bool enabled;
    bool used;
};

/* Initialize rule engine - loads from /rules.json */
void rulesInit();

/* Persist rules to /rules.json */
void rulesSave();

/* Evaluate all enabled rules. Call from loop(). */
void rulesEvaluate();

/* Create a new rule. Returns the rule ID (static buffer) or nullptr on failure. */
const char *ruleCreate(const char *name, const char *sensor_name, uint8_t sensor_pin,
                       bool sensor_analog, ConditionOp condition, int32_t threshold,
                       uint32_t interval_ms,
                       /* ON action */
                       ActionType on_action, const char *on_actuator,
                       uint8_t on_pin, int32_t on_value,
                       const char *on_nats_subj, const char *on_nats_pay,
                       /* OFF action */
                       bool has_off, ActionType off_action, const char *off_actuator,
                       uint8_t off_pin, int32_t off_value,
                       const char *off_nats_subj, const char *off_nats_pay);

/* Delete a rule by ID. "all" deletes everything. Returns true if found. */
bool ruleDelete(const char *id);

/* Enable/disable a rule by ID. Returns true if found. */
bool ruleEnable(const char *id, bool enable);

/* Find a rule by ID. Returns nullptr if not found. */
Rule *ruleFind(const char *id);

/* Get the rules array (for listing) */
const Rule *ruleGetAll();

/* Get condition op name as string */
const char *conditionOpName(ConditionOp op);

/* Get action type name as string */
const char *actionTypeName(ActionType act);

#endif /* RULES_H */
