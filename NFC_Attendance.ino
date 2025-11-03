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
const unsigned long HEARTBEAT_INTERVAL = 120000;

// ========== NFC CONTROL ==========
String lastSentText = "None";
unsigned long lastScanTime = 0;
const unsigned long SCAN_DEBOUNCE_MS = 500; // prevent duplicate scans within 500ms
bool cardWasPresent = false; // track card state for faster detection

// ========== HARDWARE PINS ==========
#define LED_R D8   // GPIO15
#define LED_G D7   // GPIO13
#define LED_B D0   // GPIO16
#define BUZZER  D6 // GPIO12

// beep length chosen: 100ms (you selected option A)
const unsigned int BUZZ_BEEP_MS = 100;

// ========== LED / BUZZER HELPERS ==========
void ledOff() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, HIGH);
}

void ledOrange() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, HIGH);
}

void ledGreen() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, HIGH);
}

void ledRed() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, HIGH);
}

void ledBlue() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, LOW);
}

void buzzShort() {
  // short beep; using tone() with duration
  tone(BUZZER, 2500, BUZZ_BEEP_MS);
  delay(5); // small gap to allow tone to start reliably
}

void blinkWifiWait() {
  while (WiFi.status() != WL_CONNECTED) {
    ledBlue();        // ON
    delay(300);
    ledOff();         // OFF
    delay(300);
  }

  // when finally connects → show ORANGE for 1 sec then go idle
  ledGreen();
  delay(1000);
}

void blinkDuringScan() {
  ledBlue();
}

void blinkWhileSaving() {
  ledBlue();
  delay(200);
  ledOff();
  delay(200);
}

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
    }else {
  Serial.println("❌ Heartbeat failed. Trying setup again...");

  if (!setupDevice(setupCode)) {        // if setup also failed
    Serial.println("❌ Setup failed after heartbeat fail. Restarting...");
    delay(3000);
    ESP.restart();
  }
}

  } else {
    Serial.println("❌ Heartbeat HTTP error. Trying setup again...");
    setupDevice(setupCode);
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
  // Immediately: short beep + orange LED
  buzzShort();
  ledOrange();

  // wait until wifi connects
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ Waiting for WiFi...");
    blinkWifiWait(); // blink blue until it connects
  }

  if (token == "" || baseUrl == "") {
    Serial.println("❌ Missing token or URL");
    ledRed();
    delay(3000);
    ledOff();
    return;
  }

  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, baseUrl)) {
    Serial.println("❌ Invalid URL");
    ledRed();
    delay(3000);
    ledOff();
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

  String respStr = "";
  if (code > 0) {
    respStr = http.getString();
    Serial.println("HTTP POST code: " + String(code));
    Serial.println("Response: " + respStr);

    if (code == 200) {
      // SUCCESS
      ledGreen();
      delay(3000);
      ledOff();
    } else {
      // FAIL (any non-200)
      ledRed();
      delay(3000);
      ledOff();
    }
  } else {
    Serial.println("❌ POST error: " + String(http.errorToString(code)));
    ledRed();
    delay(3000);
    ledOff();
  }

  http.end();
  cardWasPresent = false;
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
  WiFi.mode(WIFI_STA);      // important!
  setupCode = readFromFile("/setupcode.txt");

  if (setupCode == "") {
    Serial.println("⚠️ No setup code found. Starting WiFi portal...");
    Serial.println("🔵 Blue LED flashing...");

    WiFiManagerParameter setupCodeField("setupCode", "Enter Setup Code", "", 32);
    wm.addParameter(&setupCodeField);

    wm.setConfigPortalBlocking(false);
    wm.autoConnect("Device-Setup");

    unsigned long portalStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - portalStart < 120000) {
      wm.process();
      // small non-blocking blink
      ledBlue();
      delay(100);
      ledOff();
      delay(100);
      yield();
    }

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("❌ WiFi connection failed. Restarting...");
      ESP.restart();
    }

    setupCode = setupCodeField.getValue();

    if (setupCode == "") {
      Serial.println("⚠️ No setup code entered, restarting...");
      ESP.restart();
    }

    Serial.println("💾 Saving credentials to ROM...");
    saveToFile("/setupcode.txt", setupCode);
    Serial.println("💾 Saved setup code: " + setupCode);
  } else {
    Serial.println("✅ Loaded setup code: " + setupCode);

    wm.setConfigPortalBlocking(true);  // normal mode now
    if (!wm.autoConnect("Device-Setup")) {
      Serial.println("❌ Failed to connect. Restarting...");
      ESP.restart();
    }
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

  // initialize LED and buzzer pins
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  ledOff();
  digitalWrite(BUZZER, LOW);

  setupWiFiAndPortal();

  Serial.println("🔄 Refreshing token/url using saved setup code...");
  if (setupDevice(setupCode)) {
    Serial.println("🎉 Setup complete. Ready for NFC scanning!");
  } else {
    Serial.println("❌ Setup failed. Restarting...");
    delay(3000);
    ESP.restart();
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

  // NO LED HERE – idle = no light

  bool tagNowPresent = nfc.tagPresent();

  if (tagNowPresent && !cardWasPresent) {
    // Card just appeared - read immediately without blocking loop
    cardWasPresent = true;

    ledOrange(); // ORANGE WHILE TAG IS PRESENT
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

      if (text != lastSentText && millis() - lastScanTime >= SCAN_DEBOUNCE_MS) {
  sendTextToServer(text);
  lastSentText = text;
  lastScanTime = millis();

  // ***** NEW line added *******
  cardWasPresent = false;   // reset immediately after success/fail

  ledOff(); // also turn LED off after sending
} else {
  Serial.println("⏭️ Duplicate scan skipped");
}

          }
        }
      }
    } else {
      Serial.println("⚠️ No NDEF message found on tag");
    }
  }
  else if (!tagNowPresent && cardWasPresent) {
    cardWasPresent = false;
    Serial.println("Tag removed, ready for next scan.\n");
    ledOff();   // turn off after tag removed
  }

  delay(30);
}
