#include <Wire.h>
#include <PN532_I2C.h>
#include <PN532.h>
#include <NfcAdapter.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiManager.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>


bool setupDevice(String setupCode);

// ========== RETRY FILES ==========
#define WIFI_RETRY_FILE  "/wifi_retry.txt"
#define SETUP_RETRY_FILE "/setup_retry.txt"
#define BOOT_COUNT_FILE "/boot_count.txt"
#define MAX_BOOT_COUNT 5
#define MAX_WIFI_RETRIES 3  // 👈 you can set 3 or 5


// ========== NFC SETUP ==========
PN532_I2C pn532_i2c(Wire);
NfcAdapter nfc(pn532_i2c);
WiFiManager wm;

// ========== CONFIG ==========

String setupCode = "";
String token = "";
String studentUrl = "";
String teacherUrl = "";
String macAddress = "";
String chipId = "";
String localIp = "";

String setupUrl = "https://app.educanium.com/api/devices/setup";
// ===== HEARTBEAT =====
String heartbeatUrl = "https://app.educanium.com/api/devices/heartbeat";


unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 5000;   // 5 seconds

unsigned long lastCardActivity = 0;
const unsigned long CARD_IDLE_TIMEOUT = 60000; // 1 minute



// ========== NFC CONTROL ==========
String lastSentText = "None";
unsigned long lastScanTime = 0;
const unsigned long SCAN_DEBOUNCE_MS = 500; // prevent duplicate scans within 500ms
bool cardWasPresent = false; // track card state for faster detection
unsigned long scanStartTime = 0;

// ========== HARDWARE PINS ==========
#define LED_R D1   // GPIO5 ✅
#define LED_G D2   // GPIO4 ✅
#define LED_B D6   // GPIO12 ✅

#define BUZZER D7  // GPIO13 ✅


// beep length chosen: 100ms (you selected option A)
const unsigned int BUZZ_BEEP_MS = 100;

// ========== LED / BUZZER HELPERS ==========
void ledOff() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, LOW);
}

void ledBlue() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, HIGH);
}

void ledRed() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, LOW);
}

void ledGreen() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, LOW);
}

void ledOrange() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, LOW);
}
// void ledOff() {
//   digitalWrite(LED_R, HIGH);
//   digitalWrite(LED_G, HIGH);
//   digitalWrite(LED_B, HIGH);
// }

// void ledOrange() {
//   digitalWrite(LED_R, LOW);
//   digitalWrite(LED_G, LOW);
//   digitalWrite(LED_B, HIGH);
// }

// void ledGreen() {
//   digitalWrite(LED_R, HIGH);
//   digitalWrite(LED_G, LOW);
//   digitalWrite(LED_B, HIGH);
// }

// void ledRed() {
//   digitalWrite(LED_R, LOW);
//   digitalWrite(LED_G, HIGH);
//   digitalWrite(LED_B, HIGH);
// }

// void ledBlue() {
//   digitalWrite(LED_R, HIGH);
//   digitalWrite(LED_G, HIGH);
//   digitalWrite(LED_B, LOW);
// }
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

  // 🔊 Buzzer for 1.5 seconds
  tone(BUZZER, 2500);
  delay(1500);
  noTone(BUZZER);

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

  // 🔊 Continuous double beep (no gap)
  tone(BUZZER, 2500);
  delay(200);   // total sound length
  noTone(BUZZER);

  // immediately start second beep
  tone(BUZZER, 2500);
  delay(200);
  noTone(BUZZER);

  ledOff();
}

void showFail() {
  ledRed();
  delay(1500);
  ledOff();
}

void blinkOrangeTimes(int times) {
  for (int i = 0; i < times; i++) {
    ledOrange();
    delay(300);
    ledOff();
    delay(300);
  }
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

  WiFiClientSecure client;
  client.setInsecure();   // ESP8266: skip TLS cert validation

  HTTPClient http;

  if (!http.begin(client, heartbeatUrl)) {
    Serial.println("❌ Heartbeat begin() failed");
    return;
  }

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
        http.end();
        performFactoryReset(); // never returns
      }

    } else {
      Serial.println("❌ Heartbeat rejected. Retrying setup...");
      http.end();

      if (!setupDevice(setupCode)) {
        delay(3000);
        ESP.restart();
      }
      return;
    }

  } else {
    Serial.println("❌ Heartbeat POST failed: " + http.errorToString(code));
    http.end();

    if (!setupDevice(setupCode)) {
      delay(3000);
      ESP.restart();
    }
    return;
  }

  http.end();  // ✅ IMPORTANT
}

// ========== SETUP API ==========
bool setupDevice(String setupCode) {

  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();   // ESP8266: disable cert validation

  HTTPClient http;

  if (!http.begin(client, setupUrl)) {
    Serial.println("❌ Setup HTTPS begin() failed");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<400> doc;

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

      showSetupSuccess();   // 🟢 success only

      http.end();
      return true;
    }

    // ❌ Setup rejected by server
    Serial.println("❌ Setup rejected by server");
  }
  else {
    // ❌ HTTPS POST failed
    Serial.println("❌ Setup POST failed: " + http.errorToString(code));
  }

  // ===== retry handling =====
  int setupRetries = readRetryCount(SETUP_RETRY_FILE) + 1;
  saveRetryCount(SETUP_RETRY_FILE, setupRetries);

  Serial.println("❌ Setup attempt " + String(setupRetries) + "/5 failed");

  http.end();   // ✅ always close before reset

  if (setupRetries >= 3) {
    resetRetryCount(SETUP_RETRY_FILE);
      blinkRedTimes(3);   // 🔴🔴 indicate setup failure before reset
  delay(500);
    performFactoryReset(); // never returns
  }

  return false;
}


// ========== SEND NFC TEXT ==========
bool sendTextToServer(String text) {

  buzzShort();
  ledOrange();

  // Wait for WiFi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ Waiting for WiFi...");
    blinkWifiWait();
  }

  // Validate NFC text
  text.trim();
  if (text.length() < 2) {
    Serial.println("❌ Invalid NFC data");
    showFail();
    return false;
  }

  char firstChar = text.charAt(0);
  String targetUrl = "";

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

  // Remove prefix
  // text.remove(0, 1);
  // text.trim();

  if (text.length() == 0 || token == "" || targetUrl == "") {
    Serial.println("❌ Missing ID / token / URL");
    showFail();
    return false;
  }

  // ===== HTTPS CLIENT =====
  WiFiClientSecure client;
  client.setInsecure();   // ESP8266 TLS

  HTTPClient http;

  if (!http.begin(client, targetUrl)) {
    Serial.println("❌ HTTPS begin() failed");
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

  // ===== TOKEN EXPIRED =====
  if (code == 401) {

    Serial.println("🔑 Token expired – running setup again...");

    http.end();   // close current connection

    if (setupDevice(setupCode)) {
      Serial.println("✅ Token refreshed. Retrying NFC once...");

      delay(300);
      return sendTextToServer(text);   // 🔁 retry same card once
    }

    Serial.println("❌ Setup failed after 401");
    delay(2000);
    ESP.restart();
  }

  if (code >= 200 && code < 300) {
    showSuccess();
    success = true;
  } else {
    showFail();
  }
}

  else {
    Serial.println("❌ POST failed: " + http.errorToString(code));
    showFail();          // 🔴
  }

  http.end();            // always close HTTPS
  cardWasPresent = false;

  return success;        // ✅ tells loop() if duplicate lock should apply
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
    wm.autoConnect("Educanium Device Setup");

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

  Serial.println("❌ WiFi connection failed. Attempt " + String(wifiRetries) + "/" + String(MAX_WIFI_RETRIES));

  if (wifiRetries >= MAX_WIFI_RETRIES) {
    Serial.println("🔥 WiFi failed multiple times → Factory Reset");

    resetRetryCount(WIFI_RETRY_FILE);
    blinkRedTimes(3);   // 🔴🔴🔴 indication
    delay(500);

    performFactoryReset();   // 💣 reset device
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
if (!wm.autoConnect("Educanium Device Setup")) {

  int wifiRetries = readRetryCount(WIFI_RETRY_FILE) + 1;
  saveRetryCount(WIFI_RETRY_FILE, wifiRetries);

  Serial.println("❌ WiFi reconnect failed. Attempt " + String(wifiRetries) + "/" + String(MAX_WIFI_RETRIES));

  if (wifiRetries >= MAX_WIFI_RETRIES) {
    Serial.println("🔥 WiFi failed multiple times → Factory Reset");

    resetRetryCount(WIFI_RETRY_FILE);
    blinkRedTimes(3);
    delay(500);

    performFactoryReset();
  }

  delay(2000);
  ESP.restart();
}
  }

    // ✅ ADD THIS LINE HERE (Wi-Fi SUCCESS)
  resetRetryCount(WIFI_RETRY_FILE);

  Serial.println("✅ Wi-Fi Connected: " + WiFi.localIP().toString());
  localIp = WiFi.localIP().toString();
  blinkBlueTimes(3);
}

void checkRapidBoots() {

  int boots = readRetryCount(BOOT_COUNT_FILE);
  boots++;

  saveRetryCount(BOOT_COUNT_FILE, boots);

  Serial.println("⚡ Boot #: " + String(boots));

  if (boots >= MAX_BOOT_COUNT) {
    Serial.println("🔥 Power-cycle factory reset!");

    SPIFFS.remove(BOOT_COUNT_FILE);
    performFactoryReset();   // never returns
  }
}

// ========== SETUP ==========
void setup() {
  analogWriteRange(1023);

  Serial.begin(115200);
  delay(1000);

  // ===== FILE SYSTEM =====
  SPIFFS.begin();
checkRapidBoots();
  // 🔁 Load saved URLs & token (important after reboot)
  token = readFromFile("/token.txt");
  studentUrl = readFromFile("/student_url.txt");
  teacherUrl = readFromFile("/teacher_url.txt");

  Serial.println("📂 Loaded from SPIFFS:");
  Serial.println("Token: " + token);
  Serial.println("Student URL: " + studentUrl);
  Serial.println("Teacher URL: " + teacherUrl);

  // ===== I2C & NFC =====
 Wire.begin(2, 14); // SDA=D4, SCL=D5
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
  lastCardActivity = millis();
}


// ========== LOOP ==========
void loop() {

  // ===== CLEAR RAPID BOOT COUNTER AFTER 10s STABLE =====
  static unsigned long stableStart = millis();
  if (millis() - stableStart > 10000) {
    SPIFFS.remove(BOOT_COUNT_FILE);
  }

  // ===== IDLE LED =====
  if (!cardWasPresent) {
    ledIdle();
  }

  // ===== HEARTBEAT =====
  bool cardIdle = (millis() - lastCardActivity) > CARD_IDLE_TIMEOUT;

  if (cardIdle) {
    if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
      Serial.println("💓 Heartbeat (idle mode)");
      sendHeartbeat();
      lastHeartbeat = millis();
    }
  } else {
    lastHeartbeat = millis();
  }

  // ===== NFC =====
  bool tagNowPresent = nfc.tagPresent();

  // ===== NEW SCAN DETECTED =====
  if (tagNowPresent && !cardWasPresent) {

    // 🔥 Prevent false trigger (fast removal fix)
    delay(10);
    if (!nfc.tagPresent()) return;

    cardWasPresent = true;
    scanStartTime = millis();   // ⏱ start timeout

    ledOrange();

    NfcTag tag = nfc.read();

// 🛡 extra protection (VERY IMPORTANT)
if (!nfc.tagPresent()) {
  cardWasPresent = false;
  ledOff();
  return;
}

    // 🚨 If read fails → instant reset (NO WAIT)
    if (!tag.hasNdefMessage()) {
       Serial.println("⚠️ Fast remove / empty read");
      cardWasPresent = false;
      ledOff();
      return;
    }

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
              lastSentText = text;
              lastScanTime = millis();
              lastCardActivity = millis();
            } else {
              lastSentText = "";
            }

            // ✅ instant reset (NO WAIT)
            cardWasPresent = false;
            ledOff();
          }
          else {
            Serial.println("⏭️ Duplicate scan skipped");

            // ❌ removed delay(3000)
            blinkOrangeTimes(2);

            cardWasPresent = false;
            ledOff();
          }
        }
      }
    }
  }

  // ===== TAG REMOVED =====
  if (!tagNowPresent && cardWasPresent) {
    cardWasPresent = false;
    ledOff();   // ⚡ instant ready (no message, no delay)
  }

  // ===== FORCE RECOVERY (MOST IMPORTANT) =====
  if (cardWasPresent && (millis() - scanStartTime > 200)) {
    Serial.println("⚡ Force reset scan (fast recovery)");
    cardWasPresent = false;
    ledOff();
  }

  yield();   // ⚡ smoother loop (no blocking)
}
