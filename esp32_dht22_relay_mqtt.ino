/*
  Smart Irrigation - ESP32 Edge Node
  DHT22 (Temp/Humidity) + Relay (Pump) + MQTT over TLS (HiveMQ Cloud)
  ------------------------------------------------
  Wiring:
    DHT22 VCC  -> 3.3V
    DHT22 GND  -> GND
    DHT22 DATA -> GPIO4 (10k pull-up between DATA and VCC)
    Relay IN   -> GPIO5
    Relay VCC  -> 5V   Relay GND -> GND
    NOTE: most relay modules are ACTIVE LOW (LOW = ON). Flip RELAY_ACTIVE_LOW if yours is backwards.

  Libraries (Arduino Library Manager):
    - "DHT sensor library" by Adafruit (+ "Adafruit Unified Sensor")
    - "PubSubClient" by Nick O'Leary

  Topics (device id = esp32-01):
    syntaxiot/irrigation/esp32-01/telemetry -> JSON every 5s
    syntaxiot/irrigation/esp32-01/status    -> "online"/"offline" (retained LWT)
    syntaxiot/irrigation/esp32-01/cmd/pump  -> subscribed, payload "ON"/"OFF"
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <DHT.h>

// ---------------- Wi-Fi ----------------
const char* WIFI_SSID     = "Ziad";
const char* WIFI_PASSWORD = "12345677";

// ---------------- MQTT (HiveMQ Cloud: TLS only, port 8883) ----------------
const char* MQTT_HOST = "3dd86772a3814eb0bb4cc64655a0742c.s1.eu.hivemq.cloud";
const int   MQTT_PORT = 8883;
const char* MQTT_USER = "Irrigation";
const char* MQTT_PASS = "12345678";
const char* DEVICE_ID = "esp32-01";   // MUST be defined before the topic strings below

String TOPIC_TELEMETRY = String("syntaxiot/irrigation/") + DEVICE_ID + "/telemetry";
String TOPIC_STATUS    = String("syntaxiot/irrigation/") + DEVICE_ID + "/status";
String TOPIC_CMD_PUMP  = String("syntaxiot/irrigation/") + DEVICE_ID + "/cmd/pump";

// ---------------- Clients (declared ONCE) ----------------
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

// ---------------- DHT22 ----------------
#define DHT_PIN  4
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);

// Fallback values if the DHT22 read fails
const float IDEAL_TEMPERATURE = 24.0;
const float IDEAL_HUMIDITY    = 50.0;

// ---------------- Relay (pump) ----------------
#define RELAY_PIN 5
#define RELAY_ACTIVE_LOW true

// ---------------- Timing ----------------
const unsigned long TELEMETRY_INTERVAL_MS = 5000;
unsigned long lastTelemetry = 0;

bool pumpState = false;

void relayWrite(bool on) {
  pumpState = on;
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(RELAY_PIN, level ? HIGH : LOW);
}

void connectWiFi() {
  Serial.print("Connecting to Wi-Fi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Wi-Fi connected, IP: ");
  Serial.println(WiFi.localIP());
}

void publishTelemetry(); // forward declaration

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();
  msg.toUpperCase();

  if (String(topic) == TOPIC_CMD_PUMP) {
    if (msg == "ON") {
      relayWrite(true);
      Serial.println("Pump -> ON (via MQTT)");
    } else if (msg == "OFF") {
      relayWrite(false);
      Serial.println("Pump -> OFF (via MQTT)");
    }
    publishTelemetry(); // echo the new state immediately
  }
}

void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("Connecting to MQTT broker...");
    String clientId = String(DEVICE_ID) + "-" + String(random(0xffff), HEX);

    bool ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                           TOPIC_STATUS.c_str(), 1, true, "offline");

    if (ok) {
      Serial.println("connected");
      mqtt.publish(TOPIC_STATUS.c_str(), "online", true);
      mqtt.subscribe(TOPIC_CMD_PUMP.c_str());
    } else {
      Serial.print("failed, rc="); Serial.print(mqtt.state());
      Serial.println(" - retrying in 3s");
      delay(3000);
    }
  }
}

void publishTelemetry() {
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();
  bool sensorOk = true;

  if (isnan(temp) || isnan(hum)) {
    sensorOk = false;
    temp = IDEAL_TEMPERATURE;
    hum  = IDEAL_HUMIDITY;
    Serial.println("DHT22 read failed - publishing fallback ideal values");
  }

  char payload[160];
  snprintf(payload, sizeof(payload),
    "{\"temperature\":%.1f,\"humidity\":%.1f,\"sensor_ok\":%s,\"pump\":%s}",
    temp, hum, sensorOk ? "true" : "false", pumpState ? "true" : "false");

  mqtt.publish(TOPIC_TELEMETRY.c_str(), payload);
  Serial.print("Published: "); Serial.println(payload);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false); // pump OFF at boot

  espClient.setInsecure();  // accept HiveMQ cert without a CA cert (ok for a project)
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  dht.begin();
  connectWiFi();

  Serial.println("ESP32 Irrigation Node Started");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  unsigned long now = millis();
  if (now - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
    lastTelemetry = now;
    publishTelemetry();
  }
}