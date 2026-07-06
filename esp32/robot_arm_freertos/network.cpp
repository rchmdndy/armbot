/*
 * network.cpp — WiFi + MQTT task pinned to core 0.
 *
 * SINGLE-OWNER RULE: this task is the only one that touches mqttClient.
 * PubSubClient is not thread-safe, so the status task and command task must
 * NOT call mqttClient.* directly. They hand payloads to this task via
 * statusQueue (and cmdQueue for inbound commands).
 *
 * Status payload contract: the topic `robot/status` carries ONE of the bare
 * strings "online" | "offline" | "emergency". The web UI keys off strict
 * `msg === 'online'`, so we must never publish JSON or any other string to
 * that topic (that was the original flapping bug).
 */
#include "network.h"

WiFiClient        espClient;
PubSubClient      mqttClient(espClient);
QueueHandle_t     cmdQueue     = nullptr;
QueueHandle_t     statusQueue  = nullptr;
ServoManager      servos;
std::atomic<bool> mqttConnected{false};

// Forward declaration of the parser (defined in command.cpp).
void parseAndEnqueue(const char* topic, const char* msg);

// ========== MQTT Callback ==========
// Runs on the network task's context (core 0). Must return fast.
static void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char msg[64];
    size_t len = min(length, (unsigned int)63);
    memcpy(msg, payload, len);
    msg[len] = '\0';

    LOG_MQTT("RX topic=%s msg=%s", topic, msg);
    parseAndEnqueue(topic, msg);
}

// ========== Status publish helper (called by statusTask) ==========
// Copies a short payload into statusQueue; networkTask does the actual
// publish. This keeps PubSubClient access single-threaded.
void requestStatusPublish(const char* payload) {
    if (!statusQueue) return;
    char buf[STATUS_PAYLOAD_MAX];
    strncpy(buf, payload, STATUS_PAYLOAD_MAX - 1);
    buf[STATUS_PAYLOAD_MAX - 1] = '\0';
    // Non-blocking drop on full queue — a stale heartbeat is fine to skip.
    xQueueSend(statusQueue, buf, 0);
}

// ========== WiFi ==========
// Non-blocking connect with a bounded retry. We do NOT spin here for 20s
// blocking the network task — we attempt, poll a few times, and if it fails
// we yield back to the task loop so mqttClient.loop() keepalive can still
// run (and so a transient ~1s radio flap doesn't immediately yank MQTT).
//
// `debounceFailures` counts consecutive WL_DISCONNECTED polls; WiFi is only
// declared lost after WIFI_DEBOUNCE_FAILS in a row, smoothing short radio
// blips that would otherwise pass straight to the badge.
static constexpr int WIFI_POLL_MS         = 200;
static constexpr int WIFI_CONNECT_TIMEOUT = 8000;   // per attempt
static constexpr int WIFI_DEBOUNCE_FAILS  = 5;       // ~1s of sustained loss

void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;

    LOG_INFO("Connecting to WiFi '%s'...", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // Bounded connect wait. We poll and yield so the network task stays
    // responsive instead of blocking 20s while MQTT keepalive dies.
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - start) < WIFI_CONNECT_TIMEOUT) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_POLL_MS));
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println();
        LOG_INFO("WiFi connected. IP=%s", WiFi.localIP().toString().c_str());
    } else {
        LOG_WARN("WiFi connect attempt failed (status=%d), will retry next loop",
                 (int)WiFi.status());
    }
}

// ========== MQTT Reconnect ==========
bool reconnectMQTT() {
    if (mqttClient.connected()) return true;

    String clientId = "ESP32RobotArm-";
    clientId += String((uint32_t)ESP.getEfuseMac() & 0xFFFF, HEX);

    LOG_MQTT("Connecting as %s...", clientId.c_str());

    // Last Will: retained so a late-subscribing web UI immediately learns we
    // are offline. The "online" announcement is also retained for the same
    // reason — the badge shows the real state on first subscribe, not stale.
    bool ok = mqttClient.connect(
        clientId.c_str(),
        nullptr, nullptr,
        TOPIC_STATUS, 1, true, "offline");   // qos=1, retain=true

    if (ok) {
        // Announce online, retained.
        mqttClient.publish(TOPIC_STATUS, "online", true);
        mqttClient.subscribe(TOPIC_CONTROL);
        // Safety: stop base on reconnect so a stale speed command can't spin
        // the 360° servo uncontrolled after a reconnect.
        Command stop = {};
        stop.kind  = CmdKind::BASE_STOP;
        stop.id    = SERVO_BASE;
        stop.value = BASE_STOP_VALUE;
        xQueueSend(cmdQueue, &stop, 0);
        LOG_MQTT("connected; subscribed to %s", TOPIC_CONTROL);
    } else {
        LOG_ERR("MQTT connect failed rc=%d", mqttClient.state());
    }
    return ok;
}

// ========== Network Task ==========
void networkTask(void* arg) {
    (void)arg;

    // Brownout detector is disabled once in setup() (before any task starts),
    // so it's already off when this task runs.

    WiFi.setSleep(false);     // disable WiFi power-save for low latency

    uint32_t lastMqttAttempt = 0;
    uint32_t lastStatusPub   = 0;
    int      wifiDebounce    = 0;
    bool     wifiReportedUp  = false;
    char     pubBuf[STATUS_PAYLOAD_MAX];

    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(256);
    mqttClient.setKeepAlive(MQTT_KEEPALIVE_SEC);

    connectWiFi();

    for (;;) {
        // (1) WiFi liveness — debounced. A single dropped poll does NOT
        // yank the badge; we require WIFI_DEBOUNCE_FAILS in a row.
        if (WiFi.status() != WL_CONNECTED) {
            wifiDebounce++;
            if (wifiDebounce >= WIFI_DEBOUNCE_FAILS) {
                mqttConnected.store(false);
                LOG_WARN("WiFi lost (debounced), reconnecting...");
                connectWiFi();
                wifiDebounce = 0;
            }
        } else {
            if (!wifiReportedUp) {
                LOG_INFO("WiFi up.");
                wifiReportedUp = true;
            }
            wifiDebounce = 0;
        }

        // (2) MQTT liveness — reconnect with backoff.
        if (!mqttClient.connected()) {
            mqttConnected.store(false);
            uint32_t now = millis();
            if (now - lastMqttAttempt >= MQTT_RECONNECT_MS) {
                reconnectMQTT();
                lastMqttAttempt = now;
            }
        } else {
            mqttConnected.store(true);
            mqttClient.loop();

            // (3) Drain statusQueue and publish whatever the status task
            //     (or emergency path) queued. Single owner = no race.
            while (xQueueReceive(statusQueue, pubBuf, 0) == pdPASS) {
                mqttClient.publish(TOPIC_STATUS, pubBuf, true);
                LOG_MQTT("pub status: %s", pubBuf);
            }

            // (4) Periodic "online" heartbeat so the web UI badge refreshes
            //     even when nothing else is flowing. Plain string, retained.
            uint32_t now = millis();
            if (now - lastStatusPub >= STATUS_PERIOD_MS) {
                mqttClient.publish(TOPIC_STATUS, "online", true);
                lastStatusPub = now;
            }
        }

        // Yield ~10ms. Well under keepalive; keeps the task responsive.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
