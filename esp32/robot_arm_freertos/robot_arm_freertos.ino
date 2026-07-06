/*
 * Robot Arm - ESP32 Firmware (MQTT, FreeRTOS)
 *
 * FreeRTOS refactor of the original single-loop firmware. The work is split
 * across four tasks pinned to the appropriate core:
 *
 *   networkTask  (core 0, prio 3) - WiFi + PubSubClient. MQTT callback only
 *                                   parses + enqueues; never touches servos.
 *   commandTask  (core 1, prio 2) - dequeues commands, routes to ServoManager,
 *                                   handles emergency STOP/RESUME gating.
 *   servoTask    (core 1, prio 2) - ramps 180° servos toward targets at a
 *                                   fixed cadence (smooth motion, no current
 *                                   spikes). 360° base is set immediately.
 *   statusTask   (core 1, prio 1) - publishes heartbeat/status JSON.
 *
 * Why this is better than the original single-loop design:
 *   - WiFi/MQTT stay responsive while servos ramp (no delay() blocking loop).
 *   - MQTT callback cannot trip the broker keepalive — actuation is async.
 *   - 180° servos ramp 1°/4ms (~250°/s) instead of jumping instantly,
 *     eliminating brownout resets and jitter on the 3.3V rail.
 *   - One mutex guards all physical servo writes — no timer-allocation races.
 *
 * Hardware (unchanged):
 *   ESP32 (any dev board)
 *   1x SG90 360° Continuous Rotation Servo (Base)   - GPIO 13
 *   3x SG90 180° Servo (Up/Down, Arm, Gripper)       - GPIO 12, 14, 27
 *
 * Required libraries:
 *   - PubSubClient  by Nick O'Leary
 *   - ESP32Servo    by Kevin Harrington, John K. Bennett
 *
 * MQTT protocol (unchanged, compatible with existing web UI):
 *   Subscribe:  robot/control    -> "name:value" commands
 *   Publish:    robot/status     -> "online" / JSON heartbeat / "offline" (LWT)
 *   Commands:   base:<0-180|stop>, updown:<0-180>, arm:<0-180>,
 *               gripper:<0-180>, emergency:STOP|RESUME
 */

#include <Arduino.h>
#include "config.h"
#include "types.h"
#include "network.h"
#include "tasks.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// Globals owned by main, referenced by other TUs via extern in network.h.
// (cmdQueue, servos, espClient, mqttClient, mqttConnected are defined in
// network.cpp; only task handles live here.)

static TaskHandle_t hNetwork = nullptr;
static TaskHandle_t hCommand = nullptr;
static TaskHandle_t hServo   = nullptr;
static TaskHandle_t hStatus  = nullptr;

void setup() {
    // Brownout detector off — servos dip the rail on attach.
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(300);
    Serial.println("\n===== Robot Arm ESP32 (FreeRTOS) =====");

    // Create the queues before any task that uses them starts.
    cmdQueue = xQueueCreate(CMD_QUEUE_LEN, sizeof(Command));
    configASSERT(cmdQueue);

    statusQueue = xQueueCreate(STATUS_QUEUE_LEN, STATUS_PAYLOAD_MAX);
    configASSERT(statusQueue);

    // Initialize servo hardware (attaches + homes). Done in setup() so the
    // servo task starts with servos already at rest pose.
    servos.begin();

    // Spawn tasks. Each pinned to its preferred core.
    BaseType_t r;
    r = xTaskCreatePinnedToCore(networkTask, "net",  STACK_NETWORK, nullptr,
                                PRIO_NETWORK, &hNetwork, CORE_NETWORK);
    configASSERT(r == pdPASS);

    r = xTaskCreatePinnedToCore(commandTask, "cmd",  STACK_COMMAND, nullptr,
                                PRIO_COMMAND, &hCommand, CORE_COMMAND);
    configASSERT(r == pdPASS);

    r = xTaskCreatePinnedToCore(servoTask,   "servo", STACK_SERVO,   nullptr,
                                PRIO_SERVO,   &hServo,   CORE_SERVO);
    configASSERT(r == pdPASS);

    r = xTaskCreatePinnedToCore(statusTask,  "stat",  STACK_STATUS,  nullptr,
                                PRIO_STATUS,  &hStatus,  CORE_STATUS);
    configASSERT(r == pdPASS);

    LOG_INFO("Tasks spawned. Free heap=%d", (int)ESP.getFreeHeap());
}

// Arduino loop() runs on core 1 at priority 1. All real work happens in the
// FreeRTOS tasks above, so loop() just idles — we keep it alive to satisfy
// the Arduino framework but yield the CPU to the scheduler immediately.
void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
