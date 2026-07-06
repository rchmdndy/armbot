/*
 * types.h — Shared data structures for FreeRTOS Robot Arm firmware.
 *
 * The MQTT callback enqueues a `Command`; the command task dequeues and
 * routes it to the ServoManager. This decouples the network context (which
 * must return fast) from servo actuation (which may ramp over time).
 */
#pragma once

#include <Arduino.h>
#include <ESP32Servo.h>
#include "config.h"

// ========== Command Types ==========
enum class CmdKind : uint8_t {
    SET_SERVO,    // {id, value}: set 180° angle or 360° base speed
    BASE_STOP,    // explicit base stop (writes BASE_STOP_VALUE)
    EMERGENCY,    // {flag}: STOP = detach all, RESUME = re-attach + home
};

// Payload carried through the command queue.
struct Command {
    CmdKind kind;
    ServoId id;        // valid for SET_SERVO
    int     value;     // valid for SET_SERVO (angle/speed) and BASE_STOP
    bool    flag;      // valid for EMERGENCY (true=STOP, false=RESUME)
};

// ========== Servo Manager (thread-safe actuator layer) ==========
// Owns the four ESP32Servo objects and a mutex. The servo ramp task is the
// only writer of physical PWM; the command task sets target angles through
// setTarget(). 360° base is written instantly (speed control, no ramping —
// continuous-rotation servos expect a held velocity, not a destination).
class ServoManager {
public:
    void begin();

    // Set desired target angle (180°) or speed value (360° base).
    // Does not write PWM directly — the servo task polls targets and ramps.
    void setTarget(ServoId id, int value);

    // Immediate write (no ramp). Used for base speed control and emergency
    // stop. Acquires mutex internally.
    void writeImmediate(ServoId id, int value);

    // Detach all servos (cut PWM). Used on emergency STOP.
    void detachAll();

    // Re-attach and move all servos to home position. Used on RESUME.
    void reattachAndHome();

    // Called by the servo task on each tick to step 180° servos toward
    // their targets. Returns true if any servo moved (for diagnostics).
    bool rampTick();

    bool isEmergency() const { return _emergency; }
    void setEmergency(bool e) { _emergency = e; }

    // Snapshot current angle of a servo (last written or target).
    int  currentAngle(ServoId id) const { return _current[id]; }

private:
    void attachServo(ServoId id);
    void writeLocked(ServoId id, int value);

    Servo     _servos[SERVO_COUNT];
    int       _pins[SERVO_COUNT]   = { PIN_BASE, PIN_UPDOWN, PIN_ARM, PIN_GRIPPER };
    int       _home[SERVO_COUNT]   = { HOME_BASE, HOME_UPDOWN, HOME_ARM, HOME_GRIPPER };
    int       _target[SERVO_COUNT] = { HOME_BASE, HOME_UPDOWN, HOME_ARM, HOME_GRIPPER };
    int       _current[SERVO_COUNT]= { HOME_BASE, HOME_UPDOWN, HOME_ARM, HOME_GRIPPER };
    bool      _attached[SERVO_COUNT] = { false, false, false, false };
    bool      _emergency = false;
    SemaphoreHandle_t _mutex = nullptr;

    // ESP32Servo uses high-precision timers; concurrent attach/write from
    // multiple tasks corrupts its internal timer allocation. Mutex required.
};

// Free function for logging servo names from any translation unit.
const char* servoName(uint8_t id);
