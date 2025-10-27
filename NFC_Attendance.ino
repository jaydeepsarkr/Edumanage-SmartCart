#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <FS.h>         // File system support
#include <LittleFS.h>   // Modern file system
#define SPIFFS LittleFS // Compatibility alias
#include <ArduinoJson.h>
#include <Wire.h>
#include <PN532_I2C.h>
#include <PN532.h>
#include <NfcAdapter.h>
#include <WiFiManager.h>
#include <ESP8266HTTPClient.h>

// =================== NFC PIN DEFINITIONS ===================
#define I2C_SDA D4
#define I2C_SCL D3

// =================== GLOBAL OBJECTS ===================
PN532_I2C pn532i2c(Wire);
PN532 nfc(pn532i2c);
ESP8266WebServer server(80);
WiFiManager wm;

// =================== CONFIG STRUCT ===================
struct Config {
  char ssid[32];
  char password[64];
  char apiToken[128];
  char apiUrl[256];
} config;

// =================== FUNCTION DECLARATIONS ===================
void handleSetupPage();
void handleConfigAPI();
void handleNotFound();
void loadConfig();
void saveConfig();
String decodeNdefTextRecord(const uint8_t* payload, uint16_t payloadLength);
void setupNFC();
void scanNFC();
void sendTextToServer(String text);

// =================== SETUP ===================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n🚀 Starting SmartCard Setup...");

  // I2C init
  Wire.begin(I2C_SDA, I2C_SCL);

  // File system init
  if (!SPIFFS.begin()) {
    Serial.println("❌ SPIFFS Mount Failed");
    return;
  }

  loadConfig();

  // WiFi Manager setup
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(20);

  if (strlen(config.ssid) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssid, config.password);
    Serial.print("🔌 Connecting to WiFi");

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ Connected to WiFi!");
      setupNFC();
      return;
    }
  }

  // Start setup portal
  Serial.println("🌐 Starting Setup Portal...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("SmartCardSetup");
  Serial.println("📶 Connect to AP: SmartCardSetup");

  // Setup routes
  server.on("/", handleSetupPage);
  server.on("/api/config", HTTP_POST, handleConfigAPI);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("✅ Web server running at http://192.168.4.1");
}

// =================== LOOP ===================
void loop() {
  server.handleClient();
  if (WiFi.status() == WL_CONNECTED && strlen(config.ssid) > 0) {
    scanNFC();
  }
}

// =================== SETUP PAGE ===================
void handleSetupPage() {
  int n = WiFi.scanNetworks();
  String wifiOptions = "";
  for (int i = 0; i < n; ++i) {
    wifiOptions += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + "</option>";
  }

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0" />
<title>Educanium Setup</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: 'Poppins', sans-serif;
    background: linear-gradient(135deg, #36d1dc, #5b86e5);
    display: flex;
    justify-content: center;
    align-items: center;
    min-height: 100vh;
    color: #333;
  }
  .container {
    background: #fff;
    border-radius: 16px;
    box-shadow: 0 10px 40px rgba(0,0,0,0.1);
    padding: 40px;
    max-width: 420px;
    width: 100%;
    text-align: center;
    animation: fadeIn 0.5s ease-in-out;
  }
  h1 { font-size: 24px; color: #444; margin-bottom: 6px; }
  h2 { font-size: 14px; font-weight: 400; color: #777; margin-bottom: 30px; }
  label {
    text-align: left;
    display: block;
    font-weight: 600;
    margin-bottom: 6px;
    color: #444;
  }
  select, input {
    width: 100%;
    padding: 12px;
    margin-bottom: 20px;
    border-radius: 8px;
    border: 2px solid #ddd;
    font-size: 14px;
    transition: border-color 0.3s;
  }
  input:focus, select:focus { outline: none; border-color: #5b86e5; }
  button {
    width: 100%;
    padding: 12px;
    background: linear-gradient(135deg, #36d1dc, #5b86e5);
    color: #fff;
    border: none;
    border-radius: 8px;
    font-size: 15px;
    font-weight: 600;
    cursor: pointer;
    transition: transform 0.2s, box-shadow 0.2s;
  }
  button:hover {
    transform: translateY(-2px);
    box-shadow: 0 8px 20px rgba(91, 134, 229, 0.3);
  }
  .spinner {
    display: none;
    margin: 10px auto;
    border: 4px solid rgba(0,0,0,0.1);
    border-top: 4px solid #36d1dc;
    border-radius: 50%;
    width: 30px; height: 30px;
    animation: spin 1s linear infinite;
  }
  @keyframes fadeIn {
    from { opacity: 0; transform: translateY(20px); }
    to { opacity: 1; transform: translateY(0); }
  }
  @keyframes spin {
    0% { transform: rotate(0deg); }
    100% { transform: rotate(360deg); }
  }
  .success {
    display: none;
    text-align: center;
  }
  .success h3 { color: #36d1dc; margin-bottom: 10px; }
</style>
</head>
<body>
  <div class="container">
    <h1>Welcome to Educanium</h1>
    <h2>All-in-one School Management System</h2>

    <form id="setupForm">
      <label for="ssid">Select Wi-Fi Network</label>
      <select id="ssid" required>
        <option value="">-- Choose Wi-Fi --</option>
)"rawliteral";

  html += wifiOptions;

  html += R"rawliteral(
      </select>

      <label for="password">Wi-Fi Password</label>
      <input type="password" id="password" placeholder="Enter password" required />

      <label for="token">API Token</label>
      <input type="text" id="token" placeholder="Enter your API Token" required />

      <button type="submit" id="submitBtn">Configure Device</button>
      <div class="spinner" id="spinner"></div>
    </form>

    <div class="success" id="successMsg">
      <h3>✅ Configuration Saved!</h3>
      <p>Your Educanium device will restart shortly.</p>
    </div>
  </div>

<script>
const form = document.getElementById('setupForm');
const btn = document.getElementById('submitBtn');
const spinner = document.getElementById('spinner');
const successMsg = document.getElementById('successMsg');

form.addEventListener('submit', async (e) => {
  e.preventDefault();
  btn.disabled = true;
  spinner.style.display = 'block';
  btn.textContent = 'Configuring...';

  const ssid = document.getElementById('ssid').value;
  const password = document.getElementById('password').value;
  const token = document.getElementById('token').value;

  try {
    const res = await fetch('/api/config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ username: ssid, password, token })
    });

    if (res.ok) {
      form.style.display = 'none';
      successMsg.style.display = 'block';
      setTimeout(() => location.reload(), 3000);
    } else {
      alert('Failed to save configuration');
      btn.disabled = false;
    }
  } catch (err) {
    alert('Connection error');
    btn.disabled = false;
  } finally {
    spinner.style.display = 'none';
    btn.textContent = 'Configure Device';
  }
});
</script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

// =================== API CONFIG ===================
void handleConfigAPI() {
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    DynamicJsonDocument doc(512);
    deserializeJson(doc, body);

    strlcpy(config.ssid, doc["username"] | "", sizeof(config.ssid));
    strlcpy(config.password, doc["password"] | "", sizeof(config.password));
    strlcpy(config.apiToken, doc["token"] | "", sizeof(config.apiToken));
    strlcpy(config.apiUrl, "https://api.example.com", sizeof(config.apiUrl));

    saveConfig();
    server.send(200, "application/json", "{\"status\":\"success\"}");
    delay(2000);
    ESP.restart();
  } else {
    server.send(400, "application/json", "{\"status\":\"error\"}");
  }
}

void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}

// =================== CONFIG FILE OPS ===================
void loadConfig() {
  File file = SPIFFS.open("/config.json", "r");
  if (file) {
    DynamicJsonDocument doc(512);
    deserializeJson(doc, file);
    strlcpy(config.ssid, doc["ssid"] | "", sizeof(config.ssid));
    strlcpy(config.password, doc["password"] | "", sizeof(config.password));
    strlcpy(config.apiToken, doc["apiToken"] | "", sizeof(config.apiToken));
    strlcpy(config.apiUrl, doc["apiUrl"] | "https://api.example.com", sizeof(config.apiUrl));
    file.close();
    Serial.println("✅ Config loaded");
  } else {
    Serial.println("⚠️ No saved config found.");
  }
}

void saveConfig() {
  File file = SPIFFS.open("/config.json", "w");
  if (file) {
    DynamicJsonDocument doc(512);
    doc["ssid"] = config.ssid;
    doc["password"] = config.password;
    doc["apiToken"] = config.apiToken;
    doc["apiUrl"] = config.apiUrl;
    serializeJson(doc, file);
    file.close();
    Serial.println("💾 Config saved.");
  }
}

// =================== NFC SETUP ===================
void setupNFC() {
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("❌ Didn't find PN53x board");
    while (1);
  }

  Serial.print("✅ Found chip PN5");
  Serial.println((versiondata >> 24) & 0xFF, HEX);
  Serial.print("Firmware ver. ");
  Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versiondata >> 8) & 0xFF, DEC);

  nfc.setPassiveActivationRetries(0xFF);
  nfc.SAMConfig();
  Serial.println("📡 Waiting for an ISO14443A Card ...");
}

// =================== NFC SCAN ===================
void scanNFC() {
  uint8_t success;
  uint8_t uid[7];
  uint8_t uidLength;

  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);

  if (success) {
    NfcAdapter nfcAdapter(pn532i2c);
    if (nfcAdapter.tagPresent()) {
      NfcTag tag = nfcAdapter.read();
      if (tag.hasNdefMessage()) {
        NdefMessage message = tag.getNdefMessage();
        NdefRecord record = message.getRecord(0);
        if (record.getTnf() == TNF_WELL_KNOWN && record.getType() == "T") {
          uint8_t payload[record.getPayloadLength()];
          record.getPayload(payload);
          String text = decodeNdefTextRecord(payload, record.getPayloadLength());
          Serial.println("📗 Tag detected!");
          Serial.println("📗 Scanned text: " + text);
          sendTextToServer(text);
        }
      } else {
        Serial.println("⚠️ No NDEF message found.");
      }
    }
    delay(2000);
  }
}

// =================== TEXT DECODER ===================
String decodeNdefTextRecord(const uint8_t* payload, uint16_t payloadLength) {
  if (payloadLength < 1) return "";
  uint8_t statusByte = payload[0];
  uint8_t langLength = statusByte & 0x3F;
  String text = "";
  for (uint16_t i = 1 + langLength; i < payloadLength; i++) {
    char c = (char)payload[i];
    if (c >= 32 && c <= 126) text += c;
  }
  return text;
}

// =================== SERVER UPLOAD ===================
void sendTextToServer(String text) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    String url = String(config.apiUrl) + "/attendance";
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(config.apiToken));
    String payload = "{\"cardId\":\"" + text + "\",\"timestamp\":" + String(millis()) + "}";
    int httpCode = http.POST(payload);
    if (httpCode > 0) Serial.println("🌐 HTTP Response: " + String(httpCode));
    else Serial.println("❌ HTTP Request Failed");
    http.end();
  }
}
