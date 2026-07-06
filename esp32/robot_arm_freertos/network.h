/*
 * network.h — Network (WiFi + MQTT) task declarations.
 *
 * The network task owns WiFi + PubSubClient and runs on core 0 (where the
 * WiFi/BT protocol stack already lives). The MQTT callback is intentionally
 * tiny: it parses the payload into a `Command` and pushes it onto a queue.
 * The command task on core 1 then performs any servo work. This keeps the
 * callback's execution time bounded — the broker ping timeout never trips
 * because of slow actuation.
 */
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <atomic>
#include "config.h"
#include "types.h"

// Shared handles (defined in network.cpp, consumed by main + command task).
extern WiFiClient      espClient;
extern PubSubClient    mqttClient;
extern QueueHandle_t   cmdQueue;       // MQTT callback -> command task
extern QueueHandle_t   statusQueue;    // status task -> network task (publish)
extern ServoManager    servos;

// Cross-core visibility: std::atomic<bool> guarantees the status task reads a
// consistent value written by the network task on the other core. A plain
// `volatile bool` does NOT guarantee multi-core visibility on Xtensa LX6/LX7.
extern std::atomic<bool> mqttConnected;

// Network task entry point. This task is the SOLE owner of mqttClient — no
// other task may call mqttClient.*() directly (PubSubClient is not
// thread-safe). Other tasks hand payloads to it via statusQueue.
void networkTask(void* arg);

// Command dispatcher task entry point (defined in command.cpp).
void commandTask(void* arg);

// Parse an incoming MQTT message into a Command and enqueue it. Declared here
// because network.cpp calls it; defined in command.cpp.
void parseAndEnqueue(const char* topic, const char* msg);

// WiFi connection (non-blocking polling; does not stall networkTask).
void connectWiFi();

// MQTT reconnect with retained LWT and resubscribe. Returns true on success.
bool reconnectMQTT();

// Helper used by statusTask: copies a short status string into the
// statusQueue so networkTask publishes it. Never touches PubSubClient.
void requestStatusPublish(const char* payload);

