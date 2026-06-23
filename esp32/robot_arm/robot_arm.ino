/*
 * Robot Arm - ESP32 Firmware (MQTT)
 *
 * Hardware:
 *   ESP32 (any dev board)
 *   1x SG90 360° Continuous Rotation Servo (Base)
 *   3x SG90 180° Servo (Up/Down, Arm, Gripper)
 *
 * Pin Mapping (change to match your wiring):
 *   GPIO 13 - Base Rotation (SG90 360° continuous)
 *   GPIO 12 - Arm Up/Down (SG90 180°)
 *   GPIO 14 - Arm Forward/Back (SG90 180°)
 *   GPIO 27 - Gripper (SG90 180°)
 *
 * Required Libraries:
 *   - PubSubClient by Nick O'Leary
 *   - ESP32Servo by Kevin Harrington, John K. Bennett
 *
 * Base Servo 360° Calibration:
 *   90 = stop (neutral point)
 *   < 90 = rotate left (CCW)
 *   > 90 = rotate right (CW)
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include "soc/soc.h"           // Brownout detector disable
#include "soc/rtc_cntl_reg.h"  // WRITE_PERI_REG

// ========== WiFi Config ==========
const char* WIFI_SSID     = "Mine";
const char* WIFI_PASSWORD = "iqbal12345";

// ========== MQTT Server ==========
const char* MQTT_SERVER   = "10.226.42.67";
const int   MQTT_PORT     = 1883;

// ========== Pin Mapping ==========
const int PIN_BASE    = 13;
const int PIN_UPDOWN  = 12;
const int PIN_ARM     = 14;
const int PIN_GRIPPER = 27;

// ========== Servo Objects ==========
Servo servoBase;
Servo servoUpdown;
Servo servoArm;
Servo servoGripper;

// ========== MQTT Client ==========
WiFiClient espClient;
PubSubClient client(espClient);

// ========== State ==========
bool emergencyStop = false;

// ========== Base Servo 360° Calibration ==========
// For 360° continuous rotation servo:
//   90 = stop (neutral point)
//   < 90 = rotate left (CCW), further from 90 = faster
//   > 90 = rotate right (CW), further from 90 = faster
int baseStopValue  = 90;    // Neutral/stop position

// ========== Function Prototypes ==========
void connectWiFi();
void reconnectMQTT();
void callback(char* topic, byte* payload, unsigned int length);
void handleCommand(const char* msg);
void setServoAngle(Servo& servo, int angle, const char* name);
void setupServosStaggered();

// ========== Setup ==========
void setup() {
    // Disable brownout detector — servos cause voltage dips on startup
    // This prevents ESP32 from resetting when servos draw high current
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(1000);
    Serial.println("\n===== Robot Arm ESP32 Starting (MQTT) =====");

    // Setup servos with staggered delays to reduce inrush current
    setupServosStaggered();

    connectWiFi();

    client.setServer(MQTT_SERVER, MQTT_PORT);
    client.setCallback(callback);
}

// ========== Loop ==========
void loop() {
    if (!client.connected()) {
        reconnectMQTT();
    }
    client.loop();
}

// ========== WiFi ==========
void connectWiFi() {
    Serial.print("Connecting to WiFi");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
}

// ========== MQTT Reconnect ==========
void reconnectMQTT() {
    while (!client.connected()) {
        Serial.print("Attempting MQTT connection...");
        // Create a random client ID
        String clientId = "ESP32Client-";
        clientId += String(random(0xffff), HEX);

        // Connect with Last Will and Testament (LWT)
        if (client.connect(clientId.c_str(), NULL, NULL, "robot/status", 0, false, "offline")) {
            Serial.println("connected");

            // Once connected, publish an announcement...
            client.publish("robot/status", "online");

            // ... and resubscribe
            client.subscribe("robot/control");

            // Safety: STOP base servo on reconnect
            servoBase.write(baseStopValue);
            Serial.println("[SAFETY] Base 360° STOP on reconnect");
        } else {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" try again in 5 seconds");
            // Wait 5 seconds before retrying
            delay(5000);
        }
    }
}

// ========== MQTT Callback ==========
void callback(char* topic, byte* payload, unsigned int length) {
    // Ensure null-terminated string
    char msg[64];
    size_t len = min(length, (unsigned int)63);
    memcpy(msg, payload, len);
    msg[len] = '\0';

    Serial.print("[MQTT] RX: ");
    Serial.println(msg);

    if (strcmp(topic, "robot/control") == 0) {
        handleCommand(msg);
    }
}

// ========== Command Handler ==========
void handleCommand(const char* msg) {
    // Parse "servo_name:value" format
    const char* colon = strchr(msg, ':');
    if (colon == NULL) {
        Serial.println("Invalid format (no colon)");
        return;
    }

    // Extract servo name
    char servo[16];
    int nameLen = colon - msg;
    if (nameLen > 15) nameLen = 15;
    memcpy(servo, msg, nameLen);
    servo[nameLen] = '\0';

    // Extract value
    const char* valueStr = colon + 1;

    // Emergency commands
    if (strcmp(servo, "emergency") == 0) {
        if (strcmp(valueStr, "STOP") == 0) {
            emergencyStop = true;
            Serial.println(">>> EMERGENCY STOP <<<");
            servoBase.detach();
            servoUpdown.detach();
            servoArm.detach();
            servoGripper.detach();
        } else if (strcmp(valueStr, "RESUME") == 0) {
            emergencyStop = false;
            Serial.println(">>> EMERGENCY RESUME <<<");
            setupServosStaggered();
            // 360° base: stop position
            servoBase.write(baseStopValue);
        }
        return;
    }

    // Ignore commands if emergency stop is active
    if (emergencyStop) {
        Serial.println("Emergency mode - ignoring command");
        return;
    }

    // Base Servo (SG90 360° — speed control via numeric value)
    // Web UI sends numeric value: <90 = CCW, 90 = stop, >90 = CW
    if (strcmp(servo, "base") == 0) {
        if (strcmp(valueStr, "stop") == 0) {
            servoBase.write(baseStopValue);
            Serial.printf("[SERVO] Base -> STOP (value=%d)\n", baseStopValue);
        } else {
            int val = atoi(valueStr);
            if (val < 0 || val > 180) {
                Serial.println("Invalid base value (0-180)");
                return;
            }
            servoBase.write(val);
            Serial.printf("[SERVO] Base -> speed %d\n", val);
        }
        return;
    }

    // 180° Servos — angle control
    int angle = atoi(valueStr);
    if (angle < 0 || angle > 180) {
        Serial.println("Invalid angle (0-180)");
        return;
    }

    // Route to correct servo
    if (strcmp(servo, "updown") == 0) {
        setServoAngle(servoUpdown, angle, "Up/Down");
    } else if (strcmp(servo, "arm") == 0) {
        setServoAngle(servoArm, angle, "Arm");
    } else if (strcmp(servo, "gripper") == 0) {
        setServoAngle(servoGripper, angle, "Gripper");
    } else {
        Serial.print("Unknown servo: ");
        Serial.println(servo);
    }
}

// ========== Staggered Servo Setup (Friend's Method) ==========
// Attach servos one at a time with delays to reduce inrush current
// This prevents voltage drops that can cause erratic servo behavior
void setupServosStaggered() {
    Serial.println("[SETUP] Attaching servos (staggered)...");

    // Base servo first
    servoBase.attach(PIN_BASE);
    delay(150);  // Wait for servo to initialize
    servoBase.write(baseStopValue);
    delay(300);  // Wait for servo to settle
    Serial.printf("[SERVO] Base attached to pin %d -> STOP\n", PIN_BASE);

    // Up/Down servo
    servoUpdown.attach(PIN_UPDOWN);
    delay(150);
    setServoAngle(servoUpdown, 90, "Up/Down");
    delay(300);
    Serial.printf("[SERVO] Up/Down attached to pin %d -> 90°\n", PIN_UPDOWN);

    // Arm servo
    servoArm.attach(PIN_ARM);
    delay(150);
    setServoAngle(servoArm, 90, "Arm");
    delay(300);
    Serial.printf("[SERVO] Arm attached to pin %d -> 90°\n", PIN_ARM);

    // Gripper servo
    servoGripper.attach(PIN_GRIPPER);
    delay(150);
    setServoAngle(servoGripper, 90, "Gripper");
    delay(300);
    Serial.printf("[SERVO] Gripper attached to pin %d -> 90°\n", PIN_GRIPPER);

    Serial.println("[SETUP] All servos attached successfully!");
}

// ========== Servo Helper ==========
void setServoAngle(Servo& servo, int angle, const char* name) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    servo.write(angle);
    Serial.printf("[SERVO] %s -> %d°\n", name, angle);
}
