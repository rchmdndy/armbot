/*
 * tasks.h — Servo ramp + status task declarations.
 */
#pragma once

#include <Arduino.h>
#include "config.h"

// Steps 180° servos toward their targets every RAMP_STEP_MS.
// 360° base is written immediately by the command path (speed control).
void servoTask(void* arg);

// Publishes ESP32 status (online/offline, free heap, command queue depth)
// to TOPIC_STATUS on a fixed cadence so the web UI badge reflects reality.
void statusTask(void* arg);
