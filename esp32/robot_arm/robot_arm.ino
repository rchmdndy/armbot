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
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>

// ========== WiFi Config ==========
const char* WIFI_SSID     = "Mine";
const char* WIFI_PASSWORD = "iqbal12345";

// ========== MQTT Server ==========
// TODO: Ganti dengan IP Address Laptop/PC Anda yang menjalankan Docker Mosquitto
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
// Speed is now controlled by the web UI slider (sends numeric value 0-180)
int baseStopValue  = 90;    // Neutral/stop position (calibrate: 88-92 if needed)

// ========== Function Prototypes ==========
void connectWiFi();
void reconnectMQTT();
void callback(char* topic, byte* payload, unsigned int length);
void handleCommand(const char* msg);
void setServoAngle(Servo& servo, int angle, const char* name);

// ========== Setup ==========
void setup() {
    Serial.begin(115200);
    Serial.println("\n===== Robot Arm ESP32 Starting (MQTT) =====");

    // Attach servos
    servoBase.attach(PIN_BASE);
    servoUpdown.attach(PIN_UPDOWN);
    servoArm.attach(PIN_ARM);
    servoGripper.attach(PIN_GRIPPER);

    // Home position (90°)
    setServoAngle(servoBase,    90, "Base");
    setServoAngle(servoUpdown,  90, "Up/Down");
    setServoAngle(servoArm,     90, "Arm");
    setServoAngle(servoGripper, 90, "Gripper");

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
            servoBase.attach(PIN_BASE);
            servoUpdown.attach(PIN_UPDOWN);
            servoArm.attach(PIN_ARM);
            servoGripper.attach(PIN_GRIPPER);
            // 360° base: stop position
            servoBase.write(baseStopValue);
            // 180° servos: home position
            setServoAngle(servoUpdown,  90, "Up/Down");
            setServoAngle(servoArm,     90, "Arm");
            setServoAngle(servoGripper, 90, "Gripper");
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
    // The further from 90, the faster the rotation
    if (strcmp(servo, "base") == 0) {
        if (strcmp(valueStr, "stop") == 0) {
            servoBase.write(baseStopValue);
            Serial.printf("Base 360° -> STOP (value=%d)\n", baseStopValue);
        } else {
            int val = atoi(valueStr);
            if (val < 0 || val > 180) {
                Serial.println("Invalid base value (0-180)");
                return;
            }
            servoBase.write(val);
            Serial.printf("Base 360° -> speed %d\n", val);
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

// ========== Servo Helper ==========
void setServoAngle(Servo& servo, int angle, const char* name) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    servo.write(angle);
    Serial.printf("Servo %s -> %d°\n", name, angle);
}
