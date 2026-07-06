/*
 * command.cpp — Command parser + dispatcher task.
 *
 * Flow:
 *   MQTT callback -> parseAndEnqueue() -> cmdQueue
 *   commandTask() pulls from cmdQueue -> routes to ServoManager
 *
 * Parsing is split from dispatch on purpose. Parsing must be lightweight and
 * run in the network context; dispatch may take decisions (emergency gating,
 * home sequencing) that are easier to reason about in a dedicated task.
 */
#include "network.h"
#include "config.h"

// servoName() is declared in types.h (included via network.h) and defined
// in ServoManager.cpp — no forward decl needed here.

// ========== Parse "name:value" into a Command and enqueue ==========
void parseAndEnqueue(const char* topic, const char* msg) {
    if (strcmp(topic, TOPIC_CONTROL) != 0) return;

    const char* colon = strchr(msg, ':');
    if (!colon) {
        LOG_WARN("Invalid format (no colon): '%s'", msg);
        return;
    }

    char name[16];
    int nameLen = colon - msg;
    if (nameLen > 15) nameLen = 15;
    memcpy(name, msg, nameLen);
    name[nameLen] = '\0';

    const char* valueStr = colon + 1;

    Command cmd = {};

    // --- Emergency ---
    if (strcmp(name, "emergency") == 0) {
        cmd.kind = CmdKind::EMERGENCY;
        cmd.flag = (strcmp(valueStr, "STOP") == 0);
        // RESUME = flag=false; anything else we still treat as resume but log.
        xQueueSend(cmdQueue, &cmd, CMD_QUEUE_WAIT);
        return;
    }

    // --- Base (360° continuous): "base:stop" or "base:<n>" ---
    if (strcmp(name, "base") == 0) {
        if (strcmp(valueStr, "stop") == 0) {
            cmd.kind  = CmdKind::BASE_STOP;
            cmd.id    = SERVO_BASE;
            cmd.value = BASE_STOP_VALUE;
        } else {
            int val = atoi(valueStr);
            if (val < 0 || val > 180) {
                LOG_WARN("Invalid base value (0-180): %d", val);
                return;
            }
            cmd.kind  = CmdKind::SET_SERVO;
            cmd.id    = SERVO_BASE;
            cmd.value = val;
        }
        xQueueSend(cmdQueue, &cmd, CMD_QUEUE_WAIT);
        return;
    }

    // --- 180° servos ---
    ServoId id;
    if      (strcmp(name, "updown")  == 0) id = SERVO_UPDOWN;
    else if (strcmp(name, "arm")     == 0) id = SERVO_ARM;
    else if (strcmp(name, "gripper") == 0) id = SERVO_GRIPPER;
    else {
        LOG_WARN("Unknown servo: '%s'", name);
        return;
    }

    int angle = atoi(valueStr);
    if (angle < 0 || angle > 180) {
        LOG_WARN("Invalid angle (0-180): %d", angle);
        return;
    }

    cmd.kind  = CmdKind::SET_SERVO;
    cmd.id    = id;
    cmd.value = angle;
    xQueueSend(cmdQueue, &cmd, CMD_QUEUE_WAIT);
}

// ========== Command Dispatcher Task ==========
void commandTask(void* arg) {
    (void)arg;
    Command cmd;

    for (;;) {
        if (xQueueReceive(cmdQueue, &cmd, portMAX_DELAY) == pdPASS) {
            switch (cmd.kind) {

            case CmdKind::EMERGENCY: {
                if (cmd.flag) {                 // STOP
                    servos.setEmergency(true);
                    servos.detachAll();
                    LOG_WARN(">>> EMERGENCY STOP <<<");
                    // Reflect immediately on the badge (servos detached =
                    // robot not operational). Goes through statusQueue so
                    // only networkTask touches PubSubClient.
                    requestStatusPublish("emergency");
                } else {                        // RESUME
                    servos.reattachAndHome();
                    servos.setEmergency(false);
                    LOG_WARN(">>> EMERGENCY RESUME <<<");
                    requestStatusPublish("online");
                }
                break;
            }

            case CmdKind::BASE_STOP:
                servos.writeImmediate(SERVO_BASE, cmd.value);
                LOG_SERVO("Base -> STOP (%d)", cmd.value);
                break;

            case CmdKind::SET_SERVO:
                if (servos.isEmergency()) {
                    LOG_WARN("Emergency mode — ignoring %s:%d",
                             servoName(cmd.id), cmd.value);
                    break;
                }
                if (cmd.id == SERVO_BASE) {
                    LOG_SERVO("Base -> speed %d", cmd.value);
                } else {
                    LOG_SERVO("%s -> target %d (ramp)",
                              servoName(cmd.id), cmd.value);
                }
                servos.setTarget(cmd.id, cmd.value);
                break;
            }
        }
    }
}
