#include <Wire.h>
#include <PN532_I2C.h>
#include <PN532.h>
#include <NfcAdapter.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiManager.h>
#include <FS.h>
#include <ArduinoJson.h>


bool setupDevice(String setupCode);

// ========== RETRY FILES ==========
#define WIFI_RETRY_FILE  "/wifi_retry.txt"
#define SETUP_RETRY_FILE "/setup_retry.txt"


// ========== NFC SETUP ==========
PN532_I2C pn532_i2c(Wire);
NfcAdapter nfc(pn532_i2c);
WiFiManager wm;

// ========== CONFIG ==========
String deviceName = "Device 1";
String setupCode = "";
String token = "";
String studentUrl = "";
String teacherUrl = "";
String macAddress = "";
String chipId = "";
String localIp = "";

// 🔽 HTTPS → HTTP
String setupUrl = "http://192.168.0.132:5000/api/devices/setup";

// ===== HEARTBEAT =====
String heartbeatUrl = "http://192.168.0.132:5000/api/devices/heartbeat";
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 7000;


// ========== NFC CONTROL ==========
String lastSentText = "None";
unsigned long lastScanTime = 0;
const unsigned long SCAN_DEBOUNCE_MS = 500; // prevent duplicate scans within 500ms
bool cardWasPresent = false; // track card state for faster detection

// ========== HARDWARE PINS ==========
#define LED_R D5   // GPIO14
#define LED_G D6   // GPIO12
#define LED_B D7   // GPIO13
#define BUZZER  D8 // GPIO15


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
void ledIdle() {
  ledOff();
}


void blinkBlueTimes(int times) {
  for (int i = 0; i < times; i++) {
    ledBlue();
    delay(250);
    ledOff();
    delay(250);
  }
}

void blinkRedTimes(int times) {
  for (int i = 0; i < times; i++) {
    ledRed();
    delay(300);
    ledOff();
    delay(300);
  }
}

void showSetupSuccess() {
  ledGreen();
  delay(2000);
  ledOff();
}

void showDuplicate() {
  ledOrange();   // yellow
  delay(800);
  ledOff();
}

void showWaiting() {
  ledOrange();   // yellow
}

void showSuccess() {
  ledGreen();
  delay(1500);
  ledOff();
}

void showFail() {
  ledRed();
  delay(1500);
  ledOff();
}




void buzzShort() {
  // short beep; using tone() with duration
  tone(BUZZER, 2500, BUZZ_BEEP_MS);
  delay(5); // small gap to allow tone to start reliably
}

void blinkWifiWait() {
  while (WiFi.status() != WL_CONNECTED) {
    showWaiting();  // 🟡 yellow
    delay(400);
    ledOff();
    delay(400);
  }
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
int readRetryCount(const char* path) {
  File f = SPIFFS.open(path, "r");
  if (!f) return 0;
  int v = f.parseInt();
  f.close();
  return v;
}

void saveRetryCount(const char* path, int value) {
  File f = SPIFFS.open(path, "w");
  if (!f) return;
  f.print(value);
  f.close();
}

void resetRetryCount(const char* path) {
  SPIFFS.remove(path);
}


// helper to fully factory-reset device: erase SPIFFS + wifi creds + WiFiManager data, then restart
void performFactoryReset() {
  Serial.println("⚠️ Factory Reset initiated by server...");
blinkRedTimes(3);   // 🔴🔴🔴

  // 1) Format SPIFFS (delete all stored files)
  Serial.println("🗑️ Formatting SPIFFS (deleting ROM files)...");
  SPIFFS.format();
  delay(500);

  // 2) Clear Wi-Fi credentials stored in flash
  Serial.println("📡 Clearing Wi-Fi credentials...");
  // 'true' requests persistent erase of stored Wi-Fi config
  WiFi.disconnect(true);
  delay(500);

  // 3) Reset WiFiManager-stored settings (if any)
  Serial.println("🔁 Resetting WiFiManager stored settings...");
  wm.resetSettings(); // requires global wm
  delay(500);

  // optional: set wifi off to ensure clean restart
  WiFi.mode(WIFI_OFF);
  delay(200);

  Serial.println("🔄 Restarting ESP to complete factory reset...");
  delay(1500);
  ESP.restart();
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
     studentUrl = resp["studentURL"].as<String>();
teacherUrl = resp["teacherURL"].as<String>();

saveToFile("/student_url.txt", studentUrl);
saveToFile("/teacher_url.txt", teacherUrl);

      saveToFile("/token.txt", token);


      Serial.println("🔄 token & url updated from heartbeat");
bool factoryReset = resp["factoryReset"] | false;
if (factoryReset) {
  performFactoryReset(); // will not return (calls ESP.restart)
}



    } else {

      Serial.println("❌ Heartbeat failed. Trying setup again...");
      if (!setupDevice(setupCode)) {
        Serial.println("❌ Setup failed after heartbeat fail. Restarting...");
        delay(3000);
        ESP.restart();
      }
    }

  } else {

    Serial.println("❌ Heartbeat HTTP error. Trying setup again...");
    if (!setupDevice(setupCode)) {
      Serial.println("❌ Setup failed after HTTP error. Restarting...");
      delay(3000);
      ESP.restart();
    }

  }
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
      studentUrl = res["studentURL"].as<String>();
      teacherUrl = res["teacherURL"].as<String>();

      saveToFile("/student_url.txt", studentUrl);
      saveToFile("/teacher_url.txt", teacherUrl);
      saveToFile("/token.txt", token);

      resetRetryCount(SETUP_RETRY_FILE);

      Serial.println("🎉 Device activated successfully!");
      Serial.println("Token: " + token);
      Serial.println("Student URL: " + studentUrl);
      Serial.println("Teacher URL: " + teacherUrl);

      showSetupSuccess();   // 🟢 GREEN for 2 seconds (ONLY on success)

      http.end();
      return true;
    }

    // ❌ Setup response received but failed
    int setupRetries = readRetryCount(SETUP_RETRY_FILE) + 1;
    saveRetryCount(SETUP_RETRY_FILE, setupRetries);

    Serial.println("❌ Setup failed. Attempt " + String(setupRetries) + "/5");

    if (setupRetries >= 5) {
      resetRetryCount(SETUP_RETRY_FILE);
      performFactoryReset();
    }

  } else {
    // ❌ HTTP POST failed
    int setupRetries = readRetryCount(SETUP_RETRY_FILE) + 1;
    saveRetryCount(SETUP_RETRY_FILE, setupRetries);

    Serial.println("❌ Setup POST error. Attempt " + String(setupRetries) + "/5");

    if (setupRetries >= 5) {
      resetRetryCount(SETUP_RETRY_FILE);
      performFactoryReset();
    }
  }

  http.end();
  return false;
}

// ========== SEND NFC TEXT ==========
bool sendTextToServer(String text) {
  // Immediately: short beep + orange LED
  buzzShort();
  ledOrange();

  // wait until wifi connects
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ Waiting for WiFi...");
    blinkWifiWait();
  }

  // 🔍 Validate raw NFC text
  text.trim();
  if (text.length() < 2) {
    Serial.println("❌ Invalid NFC data");
    showFail();
    return false;
  }

  char firstChar = text.charAt(0);
  String targetUrl = "";

  // 🔀 Decide route
  if (firstChar == 'T' || firstChar == 't') {
    targetUrl = teacherUrl;
    Serial.println("👨‍🏫 Teacher card detected");
  }
  else if (firstChar == 'S' || firstChar == 's') {
    targetUrl = studentUrl;
    Serial.println("🎓 Student card detected");
  }
  else {
    Serial.println("❌ Invalid card prefix");
    showFail();
    return false;
  }

  // ✂️ Remove prefix
  // text.remove(0, 1);
  // text.trim();

  if (text.length() == 0) {
    Serial.println("❌ Empty ID after prefix removal");
    showFail();
    return false;
  }

  if (token == "" || targetUrl == "") {
    Serial.println("❌ Missing token or target URL");
    showFail();
    return false;
  }

  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, targetUrl)) {
    Serial.println("❌ Invalid target URL");
    showFail();
    return false;
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
  bool success = false;

  if (code > 0) {
    String respStr = http.getString();
    Serial.println("HTTP POST code: " + String(code));
    Serial.println("Response: " + respStr);

    if (code >= 200 && code < 300) {
      showSuccess();   // 🟢
      success = true;
    } else {
      showFail();      // 🔴
    }
  } else {
    Serial.println("❌ POST error: " + http.errorToString(code));
    showFail();        // 🔴
  }

  http.end();
  cardWasPresent = false;

  return success;
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

  int wifiRetries = readRetryCount(WIFI_RETRY_FILE) + 1;
  saveRetryCount(WIFI_RETRY_FILE, wifiRetries);

  Serial.println("❌ WiFi connection failed. Attempt " + String(wifiRetries) + "/5");

  if (wifiRetries >= 5) {
    resetRetryCount(WIFI_RETRY_FILE);
    performFactoryReset();
  }

  delay(2000);
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

    // ✅ ADD THIS LINE HERE (Wi-Fi SUCCESS)
  resetRetryCount(WIFI_RETRY_FILE);

  Serial.println("✅ Wi-Fi Connected: " + WiFi.localIP().toString());
  localIp = WiFi.localIP().toString();
  blinkBlueTimes(3);
}


// ========== SETUP ==========
void setup() {
  analogWriteRange(1023);

  Serial.begin(115200);
  delay(1000);

  // ===== FILE SYSTEM =====
  SPIFFS.begin();

  // 🔁 Load saved URLs & token (important after reboot)
  token = readFromFile("/token.txt");
  studentUrl = readFromFile("/student_url.txt");
  teacherUrl = readFromFile("/teacher_url.txt");

  Serial.println("📂 Loaded from SPIFFS:");
  Serial.println("Token: " + token);
  Serial.println("Student URL: " + studentUrl);
  Serial.println("Teacher URL: " + teacherUrl);

  // ===== I2C & NFC =====
  Wire.begin(4, 5); // SDA=D2, SCL=D1
  Wire.setClock(100000);

  nfc.begin();
  Serial.println("✅ NFC initialized");

  // ===== DEVICE INFO =====
  chipId = String(ESP.getChipId());
  macAddress = WiFi.macAddress();

  // ===== HARDWARE =====
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  ledOff();
  digitalWrite(BUZZER, LOW);

  // ===== WIFI + SETUP CODE =====
  setupWiFiAndPortal();

  // ===== DEVICE SETUP / REFRESH =====
  Serial.println("🔄 Refreshing token & URLs using setup code...");
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

   // idle color if no tag present
  if (! cardWasPresent) {
    ledIdle();
  }

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
bool success = sendTextToServer(text);

if (success) {
  lastSentText = text;        // duplicate lock ONLY on success
  lastScanTime = millis();
} else {
  lastSentText = "";          // allow unlimited retries
}


  // ***** NEW line added *******
  cardWasPresent = false;   // reset immediately after success/fail

  ledOff(); // also turn LED off after sending
} else {
  Serial.println("⏭️ Duplicate scan skipped");
   showDuplicate();
  // after 3 seconds allow same card again
  delay(3000);
  cardWasPresent = false;
  ledOff();
}

          }
        }
      }
    } else {
      Serial.println("⚠️ No NDEF message found on tag");

  delay(500);          // small pause
  cardWasPresent = false;
  ledOff();
  return;
    }
  }
  else if (!tagNowPresent && cardWasPresent) {
    cardWasPresent = false;
    Serial.println("Tag removed, ready for next scan.\n");
    ledOff();   // turn off after tag removed
  }

  delay(30);
}
