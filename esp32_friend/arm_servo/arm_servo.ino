/*
 * ============================================
 *  Robot Arm Controller — ESP32 + MQTT
 * ============================================
 *  Board   : ESP32
 *  Libraries: ESP32Servo, PubSubClient, WiFiManager, Preferences
 *
 *  Servo mapping:
 *    Index 0 → Base     → Pin 27 → 360° continuous rotation
 *              write(90)=STOP, <90=CW, >90=CCW
 *    Index 1 → Shoulder → Pin 26 → 180° positional
 *    Index 2 → Elbow    → Pin 25 → 180° positional
 *    Index 3 → Gripper  → Pin 33 → 180° positional
 *
 *  MQTT topic : robot/arm/control
 *  Payload    : "servo_name:value"  (e.g. "base:90", "shoulder:45")
 * ============================================
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include "soc/soc.h"           // Brownout detector disable
#include "soc/rtc_cntl_reg.h"  // WRITE_PERI_REG

// ─── Servo Configuration ───────────────────────────────────
struct ServoConfig {
  Servo servo;
  int pin;
  String name;
  int initialPosition;
  bool isContinuous;  // true = 360° continuous rotation, false = 180° positional
};

ServoConfig servos[] = {
  { Servo(), 14, "base",     90, true  },  // 360° continuous: 90=STOP, <90=CW, >90=CCW
  { Servo(), 27, "shoulder", 90, false },  // 180° positional
  { Servo(), 26, "elbow",    90, false },  // 180° positional
  { Servo(), 25, "gripper",  90, false },  // 180° positional
};
const int SERVO_COUNT = sizeof(servos) / sizeof(servos[0]);

// ─── MQTT Configuration ────────────────────────────────────
#define MQTT_TOPIC        "robot/arm/control"
#define MQTT_STATUS_TOPIC "robot/arm/status"

char mqttBrokerIP[40]   = "10.101.243.137";  // default broker IP
char mqttPort[6]        = "1883";            // default port

WiFiClient   espClient;
PubSubClient mqttClient(espClient);
Preferences  preferences;

// WiFiManager custom parameters
WiFiManagerParameter customMqttIP("mqtt_ip", "MQTT Broker IP", mqttBrokerIP, 40);
WiFiManagerParameter customMqttPort("mqtt_port", "MQTT Port", mqttPort, 6);

// ─── Reconnect timing ──────────────────────────────────────
unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL = 5000;  // 5 seconds

// ─── MQTT retry limit ──────────────────────────────────────
int mqttRetryCount = 0;
const int MAX_MQTT_RETRIES = 10;  // After 10 failed attempts (~50s), open config portal

// Flag to save config when WiFiManager sets new params
bool shouldSaveConfig = false;

// ════════════════════════════════════════════════════════════
//  MQTT CALLBACK — parse payload "servo_name:value"
// ════════════════════════════════════════════════════════════
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Build string from payload efficiently using buffer
  char buffer[length + 1];
  memcpy(buffer, payload, length);
  buffer[length] = '\0';
  String message = String(buffer);

  Serial.print("[MQTT] Received on ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(message);

  // Parse "servo_name:value"
  int separatorIndex = message.indexOf(':');
  if (separatorIndex == -1) {
    Serial.println("[MQTT] ERROR: Invalid format. Expected 'name:value'");
    return;
  }

  String servoName = message.substring(0, separatorIndex);
  int value = message.substring(separatorIndex + 1).toInt();

  servoName.trim();
  servoName.toLowerCase();

  // Find matching servo and write value
  bool found = false;
  for (int i = 0; i < SERVO_COUNT; i++) {
    if (servoName == servos[i].name) {
      found = true;

      if (servos[i].isContinuous) {
        // ── 360° Continuous Rotation Servo ──
        // Value 0–180 maps directly to servo.write()
        // 90 = STOP, <90 = CW, >90 = CCW
        int safeValue = constrain(value, 0, 180);
        servos[i].servo.write(safeValue);

        // Detailed logging
        Serial.print("[SERVO] ");
        Serial.print(servos[i].name);
        if (safeValue == 90) {
          Serial.println(": STOP");
        } else if (safeValue < 90) {
          Serial.print(": CW speed ");
          Serial.println(90 - safeValue);
        } else {
          Serial.print(": CCW speed ");
          Serial.println(safeValue - 90);
        }
      } else {
        // ── 180° Positional Servo ──
        if (value < 0 || value > 180) {
          Serial.print("[MQTT] ERROR: Angle ");
          Serial.print(value);
          Serial.print(" out of range for ");
          Serial.print(servos[i].name);
          Serial.println(" (0-180)");
          return;
        }
        servos[i].servo.write(value);
        Serial.print("[SERVO] ");
        Serial.print(servos[i].name);
        Serial.print(" → ");
        Serial.print(value);
        Serial.println("°");
      }
      break;
    }
  }

  if (!found) {
    Serial.print("[MQTT] ERROR: Unknown servo name '");
    Serial.print(servoName);
    Serial.println("'");
  }
}

// ════════════════════════════════════════════════════════════
//  MQTT CONNECT / RECONNECT
// ════════════════════════════════════════════════════════════

/// Attempt to connect to the MQTT broker. Returns true on success.
bool mqttConnect() {
  Serial.print("[MQTT] Connecting to ");
  Serial.print(mqttBrokerIP);
  Serial.print(":");
  Serial.print(mqttPort);
  Serial.println("...");

  // Generate a unique client ID
  String clientId = "ESP32Arm-" + String(random(0xffff), HEX);

  // Connect with LWT: publish "offline" to status topic when disconnected unexpectedly
  if (mqttClient.connect(clientId.c_str(), MQTT_STATUS_TOPIC, 1, true, "offline")) {
    Serial.println("[MQTT] Connected!");
    mqttRetryCount = 0;  // Reset retry counter on success

    // Publish "online" status (retained) so Flutter app knows ESP32 is alive
    mqttClient.publish(MQTT_STATUS_TOPIC, "online", true);
    Serial.println("[MQTT] Published 'online' to status topic");

    // Safety: STOP all continuous rotation servos on connect/reconnect
    for (int i = 0; i < SERVO_COUNT; i++) {
      if (servos[i].isContinuous) {
        servos[i].servo.write(90);  // 90 = STOP
        Serial.print("[SERVO] Safety STOP: ");
        Serial.println(servos[i].name);
      }
    }

    mqttClient.subscribe(MQTT_TOPIC);
    Serial.print("[MQTT] Subscribed to topic: ");
    Serial.println(MQTT_TOPIC);
    return true;
  } else {
    mqttRetryCount++;
    Serial.print("[MQTT] Failed, rc=");
    Serial.print(mqttClient.state());
    Serial.print(" — attempt ");
    Serial.print(mqttRetryCount);
    Serial.print("/");
    Serial.println(MAX_MQTT_RETRIES);
    return false;
  }
}

// ════════════════════════════════════════════════════════════
//  PREFERENCES — Save / Load MQTT config from NVS
// ════════════════════════════════════════════════════════════

/// Load MQTT broker IP and port from NVS (Preferences)
void loadMqttConfig() {
  preferences.begin("mqtt", true);  // read-only
  String savedIP   = preferences.getString("broker_ip", mqttBrokerIP);
  String savedPort = preferences.getString("broker_port", mqttPort);
  preferences.end();

  savedIP.toCharArray(mqttBrokerIP, sizeof(mqttBrokerIP));
  savedPort.toCharArray(mqttPort, sizeof(mqttPort));

  Serial.print("[CONFIG] Loaded MQTT Broker: ");
  Serial.print(mqttBrokerIP);
  Serial.print(":");
  Serial.println(mqttPort);
}

/// Save MQTT broker IP and port to NVS (Preferences)
void saveMqttConfig() {
  preferences.begin("mqtt", false);  // read-write
  preferences.putString("broker_ip", mqttBrokerIP);
  preferences.putString("broker_port", mqttPort);
  preferences.end();

  Serial.print("[CONFIG] Saved MQTT Broker: ");
  Serial.print(mqttBrokerIP);
  Serial.print(":");
  Serial.println(mqttPort);
}

// ════════════════════════════════════════════════════════════
//  WiFiManager save-config callback
// ════════════════════════════════════════════════════════════
void saveConfigCallback() {
  Serial.println("[WIFI] Config save requested");
  shouldSaveConfig = true;
}

// ════════════════════════════════════════════════════════════
//  SERVO SETUP
// ════════════════════════════════════════════════════════════

/// Attach all servos to their pins and write initial positions.
/// Uses staggered delays to reduce simultaneous current draw.
void setupServos() {
  for (int i = 0; i < SERVO_COUNT; i++) {
    servos[i].servo.attach(servos[i].pin);
    delay(150);  // stagger to avoid simultaneous inrush current
    servos[i].servo.write(servos[i].initialPosition);
    delay(300);  // wait for servo to settle before attaching next one
    Serial.print("[SERVO] ");
    Serial.print(servos[i].name);
    Serial.print(" attached to pin ");
    Serial.print(servos[i].pin);
    Serial.print(" → initial ");
    Serial.println(servos[i].initialPosition);
  }
}

// ════════════════════════════════════════════════════════════
//  WiFi SETUP via WiFiManager
// ════════════════════════════════════════════════════════════

/// Initialize WiFi using WiFiManager (auto AP mode if no saved credentials)
void setupWiFi() {
  WiFiManager wm;

  // Set callback for when config is saved
  wm.setSaveConfigCallback(saveConfigCallback);

  // Add custom MQTT parameters to the WiFiManager portal
  wm.addParameter(&customMqttIP);
  wm.addParameter(&customMqttPort);

  // Set timeout for AP portal (5 minutes)
  wm.setConfigPortalTimeout(300);

  Serial.println("[WIFI] Starting WiFiManager...");
  Serial.println("[WIFI] If no saved credentials, connect to AP 'RobotArm-Setup'");

  // autoConnect will:
  //  - Try saved credentials first
  //  - If fail, open an AP named "RobotArm-Setup" with captive portal
  bool connected = wm.autoConnect("RobotArm-Setup");

  if (!connected) {
    Serial.println("[WIFI] Failed to connect! Restarting...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("[WIFI] Connected!");
  Serial.print("[WIFI] IP Address: ");
  Serial.println(WiFi.localIP());

  // If user entered new MQTT config via the portal, save it
  _applyPortalConfig();
}

// ════════════════════════════════════════════════════════════
//  CONFIG PORTAL — Open AP for MQTT reconfiguration
// ════════════════════════════════════════════════════════════

/// Open WiFiManager config portal on-demand so user can change MQTT settings.
/// Called when MQTT retries are exhausted. WiFi stays connected; only the
/// captive portal is opened for MQTT IP/port reconfiguration.
void openConfigPortal() {
  Serial.println();
  Serial.println("╔══════════════════════════════════════════════╗");
  Serial.println("║  MQTT UNREACHABLE — Opening Config Portal    ║");
  Serial.println("║  Connect to WiFi AP 'RobotArm-Setup'        ║");
  Serial.println("║  to change MQTT Broker IP/Port               ║");
  Serial.println("╚══════════════════════════════════════════════╝");
  Serial.println();

  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.addParameter(&customMqttIP);
  wm.addParameter(&customMqttPort);
  wm.setConfigPortalTimeout(180);  // 3 minutes timeout

  // startConfigPortal opens AP without disconnecting existing WiFi STA
  wm.startConfigPortal("RobotArm-Setup");

  // Apply any new config entered by user
  _applyPortalConfig();

  // Reconfigure MQTT client with new settings
  int port = atoi(mqttPort);
  mqttClient.setServer(mqttBrokerIP, port);

  // Reset retry counter for fresh attempt
  mqttRetryCount = 0;
  lastReconnectAttempt = 0;

  Serial.println("[PORTAL] Config portal closed. Resuming MQTT connection...");
}

/// Helper: If WiFiManager saved new config, copy it to globals and persist.
void _applyPortalConfig() {
  if (shouldSaveConfig) {
    strncpy(mqttBrokerIP, customMqttIP.getValue(), sizeof(mqttBrokerIP) - 1);
    strncpy(mqttPort, customMqttPort.getValue(), sizeof(mqttPort) - 1);
    saveMqttConfig();
    shouldSaveConfig = false;  // Reset flag
  }
}

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  // Disable brownout detector — servos can cause voltage dips on startup
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("========================================");
  Serial.println("  Robot Arm Controller — ESP32 + MQTT");
  Serial.println("========================================");


  // 1. Setup servos (staggered to reduce inrush current)
  setupServos();

  // 2. Load saved MQTT config from NVS
  loadMqttConfig();

  // 3. Connect WiFi via WiFiManager
  setupWiFi();

  // 4. Configure MQTT client
  int port = atoi(mqttPort);
  mqttClient.setServer(mqttBrokerIP, port);
  mqttClient.setCallback(mqttCallback);

  // 5. Initial MQTT connection attempt
  mqttConnect();
}

// ════════════════════════════════════════════════════════════
//  LOOP — maintain MQTT connection
// ════════════════════════════════════════════════════════════
void loop() {
  // If MQTT disconnected, try to reconnect periodically
  if (!mqttClient.connected()) {
    // Check if we've exceeded max retries → open config portal
    if (mqttRetryCount >= MAX_MQTT_RETRIES) {
      Serial.print("[MQTT] ");
      Serial.print(MAX_MQTT_RETRIES);
      Serial.println(" attempts failed. Opening config portal...");
      openConfigPortal();
      return;  // After portal closes, loop will start fresh
    }

    unsigned long now = millis();
    if (now - lastReconnectAttempt > RECONNECT_INTERVAL) {
      lastReconnectAttempt = now;
      Serial.println("[MQTT] Connection lost. Reconnecting...");
      mqttConnect();
    }
  } else {
    // Process incoming MQTT messages
    mqttClient.loop();
  }
}