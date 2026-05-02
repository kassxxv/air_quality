#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SensirionI2cScd4x.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ─── WiFi ────────────────────────────────────────────────
#define WIFI_SSID     "YOUR_SSID"
#define WIFI_PASS     "YOUR_PASSWORD"

// ─── Azure endpoints (fill in when partner is ready) ─────
#define AZURE_POST    "https://your-azure-app.azurewebsites.net/api/data"
#define AZURE_GET     "https://your-azure-app.azurewebsites.net/api/commands"

// ─── Pins ────────────────────────────────────────────────
#define PMS_RX      16
#define PMS_TX      17
#define LED_G       18
#define LED_Y       19
#define LED_R       23
#define BUZZER_PIN  15

// ─── Thresholds (overridable via cloud) ──────────────────
int co2Threshold  = 1000; // ppm
int pm25Threshold = 55;   // ug/m3
bool buzzerState  = false;

// ─── Sensor objects ──────────────────────────────────────
SensirionI2cScd4x scd4x;
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// ─── Sensor data globals ─────────────────────────────────
uint16_t co2  = 0;
float scdTemp = 0, scdHum = 0;
uint16_t pm25 = 0;
float pmsTemp = 0, pmsHum = 0;
float temp = 0;
float hum = 0;

enum BuzzerPhase { BUZZER_IDLE, BUZZER_BURSTING, BUZZER_COOLDOWN };
BuzzerPhase buzzerPhase = BUZZER_IDLE;
unsigned long buzzerPhaseStart = 0;
unsigned long buzzerToggleAt = 0;
bool buzzerToneOn = false;

// ─── Timing ──────────────────────────────────────────────
unsigned long lastSend       = 0;
unsigned long lastCommand    = 0;
unsigned long lastWifiCheck  = 0;
#define SEND_INTERVAL     5000
#define COMMAND_INTERVAL  2000
#define WIFI_RETRY_INTERVAL 300000

// ─────────────────────────────────────────────────────────

void connectWiFi() {
    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\nWiFi failed, running offline.");
    }
}

void readPMS() {
    if (Serial2.available() >= 32) {
        if (Serial2.read() == 0x42) {
            if (Serial2.read() == 0x4D) {
                uint8_t buf[32];
                Serial2.readBytes(&buf[2], 30);
                pm25    = (buf[12] << 8) | buf[13];
                pmsTemp = ((buf[24] << 8) | buf[25]) / 10.0;
                pmsHum  = ((buf[26] << 8) | buf[27]) / 10.0;
            }
        }
    }
}

void readSCD40() {
    bool ready = false;
    scd4x.getDataReadyStatus(ready);
    if (ready) {
        scd4x.readMeasurement(co2, scdTemp, scdHum);
    }
}

void updateDisplay() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);  display.printf("CO2:  %u ppm", co2);
    // row 2 blank
    display.setCursor(0, 24); display.printf("Temp: %.1fC", temp);
    display.setCursor(0, 36); display.printf("Hum:  %.1f%%", hum);
    display.setCursor(0, 48); display.printf("PM2.5:%u ug/m3", pm25);
    display.display();
}

void handleAutomation() {
    bool badCO2  = (co2  > 0 && co2  >= co2Threshold);
    bool badDust = (pm25 > 0 && pm25 >= pm25Threshold);
    bool alarm   = badCO2 || badDust;
    bool ok      = (co2 > 0 && co2 < 800) && (pm25 < 35);
    bool medium  = !ok && !alarm;

    digitalWrite(LED_G, ok     ? HIGH : LOW);
    digitalWrite(LED_Y, medium ? HIGH : LOW);
    digitalWrite(LED_R, alarm  ? HIGH : LOW);

    if (buzzerState) {
        tone(BUZZER_PIN, 1000);
        return;
    }

    unsigned long now = millis();
    switch (buzzerPhase) {
        case BUZZER_IDLE:
            noTone(BUZZER_PIN);
            if (alarm) {
                buzzerPhase = BUZZER_BURSTING;
                buzzerPhaseStart = now;
                buzzerToggleAt = now;
                buzzerToneOn = false;
            }
            break;
        case BUZZER_BURSTING:
            if (!alarm) {
                noTone(BUZZER_PIN);
                buzzerPhase = BUZZER_IDLE;
                break;
            }
            if (now - buzzerPhaseStart >= 10000) {
                noTone(BUZZER_PIN);
                buzzerPhase = BUZZER_COOLDOWN;
                buzzerPhaseStart = now;
                break;
            }
            if ((long)(now - buzzerToggleAt) >= 0) {
                buzzerToneOn = !buzzerToneOn;
                if (buzzerToneOn) tone(BUZZER_PIN, 1000);
                else noTone(BUZZER_PIN);
                buzzerToggleAt = now + 200;
            }
            break;
        case BUZZER_COOLDOWN:
            noTone(BUZZER_PIN);
            if (now - buzzerPhaseStart >= 300000) {
                buzzerPhase = BUZZER_IDLE;
            }
            break;
    }
}

void postData() {
    if (WiFi.status() != WL_CONNECTED) return;

    StaticJsonDocument<256> doc;
    doc["co2"]     = co2;
    doc["pm25"]    = pm25;
    doc["temp"]    = temp;
    doc["hum"]     = hum;
    doc["pmsTemp"] = pmsTemp;
    doc["pmsHum"]  = pmsHum;

    String body;
    serializeJson(doc, body);

    HTTPClient http;
    http.begin(AZURE_POST);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    Serial.printf("[POST] %d\n", code);
    http.end();
}

void fetchCommands() {
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(AZURE_GET);
    int code = http.GET();

    if (code == 200) {
        StaticJsonDocument<128> doc;
        deserializeJson(doc, http.getString());
        // Expected: {"led": 0/1, "buzzer": 0/1, "co2_threshold": 1000, "pm25_threshold": 55}
        if (doc.containsKey("led"))            digitalWrite(LED_R, (bool)doc["led"] ? HIGH : LOW);
        if (doc.containsKey("buzzer"))         buzzerState   = doc["buzzer"];
        if (doc.containsKey("co2_threshold"))  co2Threshold  = doc["co2_threshold"];
        if (doc.containsKey("pm25_threshold")) pm25Threshold = doc["pm25_threshold"];
    }
    http.end();
}

// ─────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(1000);

    pinMode(LED_G, OUTPUT);
    pinMode(LED_Y, OUTPUT);
    pinMode(LED_R, OUTPUT);

    Wire.begin(21, 22);

    // SCD40
    scd4x.begin(Wire, 0x62);
    scd4x.stopPeriodicMeasurement();
    delay(500);
    scd4x.startPeriodicMeasurement();

    // OLED
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("[ERROR] OLED not found");
    }
    display.clearDisplay();
    display.display();

    // PMS
    Serial2.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);

    connectWiFi();
    Serial.println("Setup done.");
}

void loop() {
    readPMS();
    readSCD40();

    if (pmsTemp > 0) {
        temp = 0.6f * scdTemp + 0.4f * pmsTemp;
        hum  = 0.6f * scdHum  + 0.4f * pmsHum;
    } else {
        // PMS not reported yet
        temp = scdTemp;
        hum  = scdHum;
    }

    updateDisplay();
    handleAutomation();

    unsigned long now = millis();

    if (now - lastSend >= SEND_INTERVAL) {
        postData();
        lastSend = now;
    }
    if (now - lastCommand >= COMMAND_INTERVAL) {
        fetchCommands();
        lastCommand = now;
    }
    if (WiFi.status() != WL_CONNECTED && now - lastWifiCheck >= WIFI_RETRY_INTERVAL) {
        Serial.println("WiFi down, retrying...");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        lastWifiCheck = now;
    }

    delay(100);
}
