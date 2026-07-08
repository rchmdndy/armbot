/*
 * ServoManager.cpp — Thread-safe servo actuator implementation.
 *
 * Two writers exist:
 *   1. Command task  -> setTarget() / writeImmediate() / detachAll() / reattachAndHome()
 *   2. Servo task    -> rampTick()
 * ESP32Servo's internal LEDC timer allocation is not reentrant, so all
 * physical PWM writes go through the mutex. Target angles are stored
 * separately from current angles so the ramp task can read targets
 * lock-free and only briefly take the mutex to perform the step write.
 */
#include "types.h"        // ServoManager class declaration + servoName()
#include "config.h"        // LOG_* macros, RAMP params, home angles
#include <ESP32Servo.h>

void ServoManager::begin() {
    _mutex = xSemaphoreCreateMutex();
    configASSERT(_mutex);

    // Attach all servos in a staggered fashion with brief delays between
    // them. Staggering spreads the inrush current across time so the ESP32
    // 3.3V rail doesn't collapse and trigger a brownout reset.
    LOG_INFO("[SETUP] Attaching servos (staggered)...");
    for (uint8_t i = 0; i < SERVO_COUNT; ++i) {
        attachServo(static_cast<ServoId>(i));
        vTaskDelay(pdMS_TO_TICKS(150));
        writeImmediate(static_cast<ServoId>(i), _home[i]);
        vTaskDelay(pdMS_TO_TICKS(300));
        LOG_SERVO("%s attached pin=%d -> %d",
                  servoName(i), _pins[i], _home[i]);
    }
    LOG_INFO("[SETUP] All servos attached.");
}

void ServoManager::attachServo(ServoId id) {
    // Allow allocation across the full 16 timer slots; ESP32Servo picks free
    // channels. Using the default 50Hz for SG90 servos (20ms period, 1-2ms
    // pulse). _attached guarded by mutex (callers hold or set atomically).
    _servos[id].setPeriodHertz(50);
    _servos[id].attach(_pins[id]);
    _attached[id] = true;
}

void ServoManager::writeLocked(ServoId id, int value) {
    // Clamp to valid servo range. 360° base uses 0-180 as speed too.
    if (value < 0)   value = 0;
    if (value > 180) value = 180;
    _servos[id].write(value);
    _current[id] = value;
}

void ServoManager::writeImmediate(ServoId id, int value) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (!_attached[id]) { xSemaphoreGive(_mutex); return; }
    writeLocked(id, value);
    xSemaphoreGive(_mutex);
}

void ServoManager::setTarget(ServoId id, int value) {
    // Target only; ramp task performs the physical motion. Base (360°) is a
    // speed command, not a destination, so it bypasses the ramp and writes
    // immediately.
    if (id == SERVO_BASE) {
        writeImmediate(id, value);
        return;
    }
    if (value < 0)   value = 0;
    if (value > 180) value = 180;
    _target[id] = value;
}

void ServoManager::detachAll() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < SERVO_COUNT; ++i) {
        if (_attached[i]) {
            _servos[i].detach();
            _attached[i] = false;
        }
    }
    xSemaphoreGive(_mutex);
    LOG_WARN("All servos detached (emergency STOP)");
}

void ServoManager::reattachAndHome() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < SERVO_COUNT; ++i) {
        if (!_attached[i]) {
            _servos[i].setPeriodHertz(50);
            _servos[i].attach(_pins[i]);
            _attached[i] = true;
        }
        // Reset both current and target so the ramp task doesn't fight us.
        _target[i]  = _home[i];
        _current[i] = _home[i];
        _servos[i].write(_home[i]);
    }
    xSemaphoreGive(_mutex);
    LOG_INFO("Servos re-attached and homed (RESUME)");
}

bool ServoManager::rampTick() {
    bool moved = false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < SERVO_COUNT; ++i) {
        if (i == SERVO_BASE) continue;          // 360° — no ramping
        if (!_attached[i])   continue;
        int diff = _target[i] - _current[i];
        if (diff == 0) continue;

        // ADAPTIVE step: big |diff| -> RAMP_MAX_STEP_DEG (fast transit, kills
        // the old fixed 250°/s cap); small |diff| -> |diff| (exact snap, no
        // overshoot). Cadence stays 1 write per 20ms PWM frame (obs-776 fix)
        // — magnitude never stacks writes within a frame.
        int adiff = (diff > 0) ? diff : -diff;
        int stepMag;
        if (adiff <= RAMP_MIN_STEP_DEG)   stepMag = adiff;                     // exact snap
        else                             stepMag = (adiff < RAMP_MAX_STEP_DEG)
                                                 ? adiff : RAMP_MAX_STEP_DEG;  // fast transit
        int step = (diff > 0) ? stepMag : -stepMag;

        _servos[i].write(_current[i] + step);
        _current[i] += step;
        moved = true;
    }
    xSemaphoreGive(_mutex);
    return moved;
}

const char* servoName(uint8_t id) {
    switch (id) {
        case SERVO_BASE:    return "Base";
        case SERVO_UPDOWN:  return "Up/Down";
        case SERVO_ARM:     return "Arm";
        case SERVO_GRIPPER: return "Gripper";
        default:            return "?";
    }
}
