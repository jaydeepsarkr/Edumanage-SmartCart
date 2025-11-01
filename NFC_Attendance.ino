#include <Wire.h>
#include <PN532_I2C.h>
#include <PN532.h>
#include <NfcAdapter.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiManager.h>
#include <FS.h>
#include <ArduinoJson.h>

// ========== NFC SETUP ==========
PN532_I2C pn532_i2c(Wire);
NfcAdapter nfc(pn532_i2c);

// ========== CONFIG ==========
String deviceName = "Device 2";
String setupCode = "";
String token = "";
String baseUrl = "";
String macAddress = "";
String chipId = "";
String localIp = "";

String setupUrl = "http://192.168.31.117:5000/api/devices/setup";

// ===== HEARTBEAT =====
String heartbeatUrl = "http://192.168.31.117:5000/api/devices/heartbeat";
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 7200; // 2 hrs

// ========== NFC CONTROL ==========
String lastSentText = "None";

// ========== FILE FUNCTIONS ==========
void saveToFile(String path, String content) {
  File f = SPIFFS.open(path, "w");
  if (!f) return;
  f.print(content);
  f.close();
}

String readFromFile(String path) {
  File f = SPIFFS.open(path, "r");
  if (!f) return "";
  String content = f.readString();
  f.close();
  return content;
}

// ===== HEARTBEAT FN =====
void sendHeartbeat() {

  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, heartbeatUrl)) return;

  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<300> doc;
  doc["token"] = token;
  doc["macAddress"] = macAddress;
  doc["chipId"] = chipId;
  doc["ip"] = localIp;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);

  if (code > 0) {
    String res = http.getString();
    Serial.println("❤️ Heartbeat Response: " + res);

    StaticJsonDocument<400> resp;
    if (deserializeJson(resp, res) == DeserializationError::Ok && resp["success"]) {
      token = resp["token"].as<String>();
      baseUrl = resp["url"].as<String>();

      saveToFile("/token.txt", token);
      saveToFile("/url.txt", baseUrl);

      Serial.println("🔄 token & url updated from heartbeat");
    }
  }
  http.end();
}

// ========== SETUP API ==========
bool setupDevice(String setupCode) {
  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, setupUrl)) {
    Serial.println("❌ Failed to connect to setup URL");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<400> doc;
  doc["deviceName"] = deviceName;
  doc["code"] = setupCode;
  doc["macAddress"] = macAddress;
  doc["chipId"] = chipId;
  doc["ip"] = localIp;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code > 0) {
    String response = http.getString();
    Serial.println("✅ Setup Response: " + response);

    StaticJsonDocument<512> res;
    if (deserializeJson(res, response) == DeserializationError::Ok && res["success"]) {
      token = res["token"].as<String>();
      baseUrl = res["url"].as<String>();

      saveToFile("/token.txt", token);
      saveToFile("/url.txt", baseUrl);

      Serial.println("🎉 Device activated successfully!");
      Serial.println("Token: " + token);
      Serial.println("URL: " + baseUrl);

      http.end();
      return true;
    } else {
      Serial.println("❌ Setup failed: Invalid response");
    }
  } else {
    Serial.println("❌ Setup POST error: " + String(http.errorToString(code)));
  }

  http.end();
  return false;
}

// ========== SEND NFC TEXT ==========
void sendTextToServer(String text) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ Wi-Fi not connected");
    return;
  }

  if (token == "" || baseUrl == "") {
    Serial.println("❌ Missing token or URL");
    return;
  }

  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, baseUrl)) {
    Serial.println("❌ Invalid URL");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + token);

  StaticJsonDocument<200> doc;
  doc["studentId"] = text;
  doc["status"] = "present";
  doc["attendanceByNFC"] = true;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code > 0) {
    Serial.println("✅ Attendance sent. Code: " + String(code));
    Serial.println(http.getString());
  } else {
    Serial.println("❌ POST error: " + String(http.errorToString(code)));
  }

  http.end();
}

// ========== NDEF TEXT DECODER ==========
String decodeNdefTextRecord(byte* payload, int payloadLength) {
  if (payloadLength < 2) return "";

  uint8_t status = payload[0];
  uint8_t langLength = status & 0x3F;
  int textStart = 1 + langLength;

  if (textStart >= payloadLength) return "";

  String text = "";
  for (int i = textStart; i < payloadLength; i++) {
    byte b = payload[i];
    if (b >= 32 && b <= 126) text += (char)b;
  }
  return text;
}

// ========== WIFI & PORTAL ==========
void setupWiFiAndPortal() {
  WiFiManager wm;

  setupCode = readFromFile("/setupcode.txt");

  if (setupCode == "") {
    Serial.println("⚠️ No setup code found. Starting WiFi portal...");

    WiFiManagerParameter setupCodeField("setupCode", "Enter Setup Code", "", 32);
    wm.addParameter(&setupCodeField);

    if (!wm.autoConnect("Device-Setup")) {
      Serial.println("❌ WiFi connection failed. Restarting...");
      ESP.restart();
    }

    setupCode = setupCodeField.getValue();

    if (setupCode == "") {
      Serial.println("⚠️ No setup code entered, restarting...");
      ESP.restart();
    }

    saveToFile("/setupcode.txt", setupCode);
    Serial.println("💾 Saved setup code: " + setupCode);
  } else {
    Serial.println("✅ Loaded setup code: " + setupCode);
    wm.autoConnect("Device-Setup");
  }

  Serial.println("✅ Wi-Fi Connected: " + WiFi.localIP().toString());
  localIp = WiFi.localIP().toString();
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(1000);
  SPIFFS.begin();

  Wire.begin(4, 5); // SDA=D2, SCL=D1
  Wire.setClock(100000);

  nfc.begin();
  Serial.println("✅ NFC initialized");

  chipId = String(ESP.getChipId());
  macAddress = WiFi.macAddress();

  token = readFromFile("/token.txt");
  baseUrl = readFromFile("/url.txt");

  setupWiFiAndPortal();

  if (token.length() > 0 && baseUrl.length() > 0) {
    Serial.println("✅ Loaded saved credentials");
  } else {
    Serial.println("🔑 Running setup process...");
    if (setupDevice(setupCode)) {
      Serial.println("🎉 Setup complete. Ready for NFC scanning!");
    } else {
      Serial.println("❌ Setup failed. Restarting...");
      delay(3000);
      ESP.restart();
    }
  }

  Serial.println("✅ System Ready for NFC Scanning...");
}

// ========== LOOP ==========
void loop() {

  // ===== HEARTBEAT TIMER =====
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }

  // NFC TAG READ
  if (nfc.tagPresent()) {
    NfcTag tag = nfc.read();

    if (tag.hasNdefMessage()) {
      NdefMessage message = tag.getNdefMessage();
      int recordCount = message.getRecordCount();

      for (int i = 0; i < recordCount; i++) {
        NdefRecord record = message.getRecord(i);

        int payloadLength = record.getPayloadLength();
        byte payload[payloadLength];
        record.getPayload(payload);

        if (payloadLength > 0) {
          String text = decodeNdefTextRecord(payload, payloadLength);
          text.trim();

          if (text.length() > 0) {
            Serial.println("📗 Scanned text: " + text);

            if (text != lastSentText) {
              sendTextToServer(text);
              lastSentText = text;
            } else {
              Serial.println("⏭️ Duplicate scan skipped");
            }
          }
        }
      }
    } else {
      Serial.println("⚠️ No NDEF message found on tag");
    }

    while (nfc.tagPresent()) delay(100);
    Serial.println("Tag removed, ready for next scan.\n");
  }

  delay(300);
}
