/*
 * config.h — Central configuration for FreeRTOS Robot Arm firmware.
 *
 * All tunables (pins, WiFi, MQTT, task params, calibration) live here so the
 * rest of the firmware references symbols, not magic numbers. Edit this file
 * to reconfigure your board.
 */
#pragma once

#include <Arduino.h>
#include <atomic>          // std::atomic<bool> for cross-core shared flags

// ========== WiFi Config ==========
constexpr const char* WIFI_SSID     = "Mine";
constexpr const char* WIFI_PASSWORD = "iqbal12345";

// ========== MQTT Server ==========
constexpr const char* MQTT_SERVER   = "10.185.170.67";
constexpr const int   MQTT_PORT     = 1883;

// ========== MQTT Topics (must match web UI) ==========
constexpr const char* TOPIC_CONTROL = "robot/control";   // ESP32 subscribes
constexpr const char* TOPIC_STATUS  = "robot/status";    // ESP32 publishes

// ========== Pin Mapping (unchanged from original firmware) ==========
constexpr int PIN_BASE    = 13;  // SG90 360° continuous rotation
constexpr int PIN_UPDOWN  = 12;  // SG90 180°
constexpr int PIN_ARM     = 14;  // SG90 180°
constexpr int PIN_GRIPPER = 27;  // SG90 180°

// ========== Servo IDs ==========
enum ServoId : uint8_t {
    SERVO_BASE    = 0,
    SERVO_UPDOWN  = 1,
    SERVO_ARM     = 2,
    SERVO_GRIPPER = 3,
    SERVO_COUNT   = 4,
};

// ========== Home / Initial Angles ==========
// 360° base: 90 = stop. 180° servos: neutral resting pose.
constexpr int HOME_BASE    = 90;
constexpr int HOME_UPDOWN  = 100;
constexpr int HOME_ARM     = 150;
constexpr int HOME_GRIPPER = 99;

// ========== Base Servo 360° Calibration ==========
// 90  = stop (neutral)
// <90 = rotate CCW (left),  further from 90 = faster
// >90 = rotate CW  (right), further from 90 = faster
constexpr int BASE_STOP_VALUE = 90;

// ========== Smooth Move Tuning (180° servos) ==========
// Ramping avoids instantaneous 90°->150° jumps that spike current and cause
// brownouts / jitter.
//
// The ramp cadence MUST match the servo PWM refresh period. SG90 runs at
// 50Hz (setPeriodHertz(50)), so it only latches a new position every 20ms.
// A faster ramp write() (e.g. every 4ms) stacks 5 updates inside one PWM
// period; the servo ignores the 4 in-between values and jumps the full 5° in
// one 20ms frame — visibly jagged/stuttery motion even though the numeric
// ramp is smooth. Ticking every 20ms means every write() lands on its own
// PWM frame, so motion is smooth. 5°/20ms = ~250°/s (same peak speed as the
// old 1°/4ms, just without the wasted, un-actuated sub-steps).
constexpr int RAMP_STEP_MS    = 20;    // ms per step — matches 50Hz PWM period
constexpr int RAMP_STEP_DEG   = 5;     // degrees per step (5°/20ms ≈ 250°/s)

// ========== Command Queue ==========
constexpr int CMD_QUEUE_LEN   = 16;    // commands buffered from MQTT callback
constexpr int CMD_QUEUE_WAIT  = pdMS_TO_TICKS(100);

// ========== Status Publish Queue ==========
// statusTask does NOT touch PubSubClient (it is not thread-safe). It builds
// a short status string and hands it to networkTask via this queue; only
// networkTask ever calls mqttClient.publish(). Length 4 is plenty — at most
// one heartbeat + one emergency/state change in flight.
constexpr int STATUS_QUEUE_LEN  = 4;
constexpr int STATUS_QUEUE_WAIT = pdMS_TO_TICKS(50);

// Max length of a status payload string (plain "online"/"offline"/"emergency"
// plus optional short diagnostics). 32 is generous.
constexpr int STATUS_PAYLOAD_MAX = 32;

// ========== Task Configuration ==========
// Stack sizes in bytes. Network task is largest (WiFi + PubSubClient buffers).
// Status task bumped to 4096 — it runs snprintf + queue send under preemption.
constexpr uint32_t STACK_NETWORK = 8192;
constexpr uint32_t STACK_COMMAND = 3072;
constexpr uint32_t STACK_SERVO   = 2048;
constexpr uint32_t STACK_STATUS  = 4096;

// FreeRTOS priorities (higher = more important). Arduino loop() = 1.
//
// PRIO_SERVO must be HIGHER than PRIO_COMMAND. servoTask ramps 180° servos on
// a fixed 4ms cadence (vTaskDelayUntil). If it shared a priority with
// commandTask on the same core (both core 1), FreeRTOS round-robin time-slices
// them: a burst of MQTT commands would keep commandTask on-CPU and delay the
// ramp tick, so motion sped up when MQTT was idle and slowed under load — the
// inconsistent-speed symptom. A higher priority lets the ramp preempt command
// dispatch. Each tick is tiny (4 servos) and then sleeps, so it never starves
// commandTask. It equals PRIO_NETWORK but runs on a different core, so no
// contention there.
constexpr UBaseType_t PRIO_NETWORK = 3;
constexpr UBaseType_t PRIO_COMMAND = 2;
constexpr UBaseType_t PRIO_SERVO   = 3;   // > command: ramp cadence must not be starved
constexpr UBaseType_t PRIO_STATUS  = 1;

// Core assignment. ESP32: core 0 = WiFi/BT protocol stack, core 1 = app.
constexpr BaseType_t CORE_NETWORK = 0;  // WiFi lives here
constexpr BaseType_t CORE_COMMAND = 1;  // app core
constexpr BaseType_t CORE_SERVO   = 1;
constexpr BaseType_t CORE_STATUS  = 1;

// ========== Status / Heartbeat ==========
constexpr uint32_t STATUS_PERIOD_MS     = 5000;  // publish "online" every 5s
constexpr uint32_t MQTT_KEEPALIVE_SEC   = 15;
constexpr uint32_t MQTT_RECONNECT_MS    = 3000;

// ========== Logging ==========
#define LOG_INFO(fmt, ...)  Serial.printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  Serial.printf("[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)   Serial.printf("[ERR ] " fmt "\n", ##__VA_ARGS__)
#define LOG_SERVO(fmt, ...) Serial.printf("[SRVO] " fmt "\n", ##__VA_ARGS__)
#define LOG_MQTT(fmt, ...)  Serial.printf("[MQTT] " fmt "\n", ##__VA_ARGS__)
