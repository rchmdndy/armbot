/*
 * tasks.cpp — Servo ramp task + status/heartbeat task.
 *
 * servoTask:   the only writer of physical PWM for 180° servos. Ramps each
 *              servo toward its target by RAMP_STEP_DEG every RAMP_STEP_MS.
 *              The 360° base is excluded — its speed is set immediately by
 *              the command path.
 *
 * statusTask:  emits the ESP32 status string. It does NOT call
 *              mqttClient.publish() — PubSubClient is not thread-safe and
 *              only networkTask may touch it. Instead, statusTask pushes a
 *              short plain-string payload ("online" | "emergency") into
 *              statusQueue; networkTask drains the queue and publishes with
 *              retain=true. This is the single-owner pattern: one task owns
 *              the MQTT client, others hand data to it via a queue.
 */
#include "tasks.h"
#include "network.h"

// ========== Servo Ramp Task ==========
void servoTask(void* arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        bool moved = servos.rampTick();
        (void)moved;   // available for diagnostics if needed

        // Fixed-rate tick (RAMP_STEP_MS). vTaskDelayUntil keeps the cadence
        // stable regardless of rampTick duration — critical for smooth motion.
        vTaskDelayUntil(&last, pdMS_TO_TICKS(RAMP_STEP_MS));
    }
}

// ========== Status / Heartbeat Task ==========
// Publishes the ESP32 state on a fixed cadence. Because we only hand a
// short string to networkTask (never touch PubSubClient), the heartbeat
// keeps working even if the network task is momentarily busy in loop().
void statusTask(void* arg) {
    (void)arg;
    bool lastEmergency = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_PERIOD_MS));

        if (!mqttConnected.load()) continue;

        // Plain-string status contract — matches web UI `msg === 'online'`.
        // "emergency" is informational; the web UI will show offline for it,
        // which is correct (servos are detached, robot is not operational).
        bool em = servos.isEmergency();
        requestStatusPublish(em ? "emergency" : "online");

        // If emergency state just toggled, push an immediate update so the
        // badge reflects it without waiting for the next 5s tick.
        if (em != lastEmergency) {
            requestStatusPublish(em ? "emergency" : "online");
            lastEmergency = em;
        }
    }
}
