// Barcode Scanner MVP - Arduino R4 WiFi + USB Host Shield
#include <WiFiS3.h>
#include <hidboot.h>
#include <usbhub.h>
#include <SPI.h>
#include <EEPROM.h>
#include <new>

// Config
const char defaultSsid[] = "prog";
const char defaultPass[] = "Alvorlig5And";
char ssid[33] = "";
char pass[65] = "";
const char* serverHost = "inventorysystem.acceptable.pro";
const int serverPort = 443;
const char* cfClientId = "fd0f2455c8ef15b1f83d6315190ee48b.access";
const char* cfClientSecret = "7e4a0b4cefbd617e5b0a9be60444ea07eafcd7d6c2fe76cddf458ad5885bf7e4";
const unsigned long TIMEOUT_MS = 60000;
const unsigned long WIFI_CONNECT_TOTAL_TIMEOUT_MS = 12000;

// EEPROM layout
const uint16_t EEPROM_MAGIC = 0xBEEF;
const int EEPROM_MAGIC_ADDR = 0;
const int EEPROM_SSID_ADDR = 4;
const int EEPROM_PASS_ADDR = 37;

// Pins
const int buzzerPin = 8;
const int redPin = 7;
const int greenPin = 6;
const int bluePin = 5;

// State
struct BarcodeNode {
  String value;
  BarcodeNode* next;
};

BarcodeNode* head = nullptr;
BarcodeNode* tail = nullptr;
int count = 0;
int status = WL_IDLE_STATUS;
unsigned long lastScan = 0;
String barcode = "";
bool ready = false;
bool wifiSetupMode = false;
bool scannerInputLocked = false;
bool wifiConnectInProgress = false;
String pendingWifiSsid = "";
String pendingWifiPass = "";

enum WiFiConnectState {
  WIFI_IDLE,
  WIFI_BEGIN,
  WIFI_WAIT,
  WIFI_SUCCESS,
  WIFI_FAIL
};

enum WiFiConnectPurpose {
  WIFI_PURPOSE_NONE,
  WIFI_PURPOSE_BOOT_SAVED,
  WIFI_PURPOSE_BOOT_DEFAULT,
  WIFI_PURPOSE_PROVISION,
  WIFI_PURPOSE_RECONNECT
};

WiFiConnectState wifiState = WIFI_IDLE;
WiFiConnectPurpose wifiPurpose = WIFI_PURPOSE_NONE;
unsigned long wifiStartMs = 0;
String wifiSsidTmp = "";
String wifiPassTmp = "";

// USB
USB Usb;
HIDBoot<USB_HID_PROTOCOL_KEYBOARD> Kbd(&Usb);
WiFiSSLClient client; // SSL client for HTTPS

class KbdParser : public KeyboardReportParser {
protected:
  void OnKeyDown(uint8_t mod, uint8_t key) override {
    if (scannerInputLocked) return;

    uint8_t c = OemToAscii(mod, key);
    if (c) {
      if (c == 13 || c == 10) { if (barcode.length() > 0) ready = true; }
      else barcode += (char)c;
    }
  }
};
KbdParser parser;

void initEepromStorage() {
#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
  EEPROM.begin(512);
#endif
}

void flushEepromStorage() {
  // Arduino Uno R4 and other platforms: write to specific addresses to ensure flush
#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
  EEPROM.commit();
#else
  // For non-ESP platforms, explicitly write a marker at a safe location
  EEPROM.write(100, 0xAA);
#endif
}

void setLed(bool redOn, bool greenOn, bool blueOn) {
  digitalWrite(redPin, redOn ? HIGH : LOW);
  digitalWrite(greenPin, greenOn ? HIGH : LOW);
  digitalWrite(bluePin, blueOn ? HIGH : LOW);
}

void lockScannerInput() {
  scannerInputLocked = true;
  barcode = "";
  ready = false;
}

void unlockScannerInput() {
  barcode = "";
  ready = false;
  scannerInputLocked = false;
}

void delayWithUsb(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    Usb.Task();
    delay(1);
  }
}

void beepWifiAlert() {
  tone(buzzerPin, 523, 150); delayWithUsb(180);
  tone(buzzerPin, 494, 150); delayWithUsb(180);
  tone(buzzerPin, 440, 250); delayWithUsb(280);
}

void playWiFiConnectedTune() {
  // Startup success tune kept from the original version.
  tone(buzzerPin, 659, 150); delayWithUsb(180);
  tone(buzzerPin, 659, 150); delayWithUsb(180);
  delayWithUsb(150);

  tone(buzzerPin, 659, 150); delayWithUsb(180);
  delayWithUsb(150);

  tone(buzzerPin, 523, 150); delayWithUsb(180);
  tone(buzzerPin, 659, 150); delayWithUsb(180);
  tone(buzzerPin, 784, 300); delayWithUsb(350);
  delayWithUsb(200);

  tone(buzzerPin, 392, 300); delayWithUsb(350);
}

void copyCredentials(const char* newSsid, const char* newPass) {
  strncpy(ssid, newSsid, sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = '\0';
  strncpy(pass, newPass, sizeof(pass) - 1);
  pass[sizeof(pass) - 1] = '\0';
}

bool loadSavedWiFi() {
  uint16_t magic = 0;
  EEPROM.get(EEPROM_MAGIC_ADDR, magic);
  if (magic != EEPROM_MAGIC) return false;

  EEPROM.get(EEPROM_SSID_ADDR, ssid);
  EEPROM.get(EEPROM_PASS_ADDR, pass);
  ssid[sizeof(ssid) - 1] = '\0';
  pass[sizeof(pass) - 1] = '\0';
  return strlen(ssid) > 0;
}

void saveWiFi(const char* newSsid, const char* newPass) {
  copyCredentials(newSsid, newPass);
  uint16_t magic = EEPROM_MAGIC;
  EEPROM.put(EEPROM_MAGIC_ADDR, magic);
  EEPROM.put(EEPROM_SSID_ADDR, ssid);
  EEPROM.put(EEPROM_PASS_ADDR, pass);
  flushEepromStorage();
}

void clearSavedWiFi() {
  memset(ssid, 0, sizeof(ssid));
  memset(pass, 0, sizeof(pass));
  uint16_t magic = 0;
  EEPROM.put(EEPROM_MAGIC_ADDR, magic);
  EEPROM.put(EEPROM_SSID_ADDR, ssid);
  EEPROM.put(EEPROM_PASS_ADDR, pass);
  flushEepromStorage();
  // Additional safety: zero-fill the entire credential storage area
  for (int i = EEPROM_SSID_ADDR; i < EEPROM_PASS_ADDR + 65; i++) {
    EEPROM.write(i, 0);
  }
  flushEepromStorage();
  delay(50); // Ensure EEPROM write completes
}

void startWiFiConnect(const char* s, const char* p, WiFiConnectPurpose purpose) {
  if (wifiConnectInProgress) return;

  Serial.println("[WiFi] connect started");
  wifiSsidTmp = s;
  wifiPassTmp = p;
  wifiPurpose = purpose;
  wifiStartMs = millis();
  wifiState = WIFI_BEGIN;
  wifiConnectInProgress = true;
  lockScannerInput();
}

void handleWiFiConnect() {
  if (wifiState == WIFI_IDLE) return;

  switch (wifiState) {
    case WIFI_BEGIN:
      WiFi.disconnect();
      status = WiFi.begin(wifiSsidTmp.c_str(), wifiPassTmp.c_str());
      wifiState = WIFI_WAIT;
      break;

    case WIFI_WAIT:
      if (WiFi.status() == WL_CONNECTED) {
        wifiState = WIFI_SUCCESS;
      } else if (millis() - wifiStartMs > WIFI_CONNECT_TOTAL_TIMEOUT_MS) {
        wifiState = WIFI_FAIL;
      }
      break;

    case WIFI_SUCCESS:
      Serial.println("[WiFi] connected");
      Serial.print("IP: "); Serial.println(WiFi.localIP());
      wifiState = WIFI_IDLE;
      wifiConnectInProgress = false;
      unlockScannerInput();
      Serial.println("[WiFi] finalize (unlock scanner)");
      break;

    case WIFI_FAIL:
      Serial.print("[WiFi] timeout/fail after ms: ");
      Serial.println(millis() - wifiStartMs);
      WiFi.disconnect();
      wifiState = WIFI_IDLE;
      wifiConnectInProgress = false;
      unlockScannerInput();
      Serial.println("[WiFi] finalize (unlock scanner)");
      break;

    default:
      break;
  }
}

void enterWifiSetupMode() {
  wifiSetupMode = true;
  pendingWifiSsid = "";
  pendingWifiPass = "";
  setLed(true, false, false);
  beepWifiAlert();
  Serial.println("WiFi missing/unconnectable. Scan SSID, then password.");
}

void flashBlueForSsidScanned() {
  setLed(false, false, true);
  delayWithUsb(180);
  setLed(true, false, false);
}

void handleWifiProvisionScan(const String& rawScan) {
  String scan = rawScan;
  scan.trim();

  String upper = scan;
  upper.toUpperCase();
  if (upper == "SEND" || upper == "UNDO" || upper == "RESET") {
    Serial.println("Ignored command barcode during WiFi setup mode.");
    return;
  }

  if (pendingWifiSsid.length() == 0) {
    pendingWifiSsid = scan;
    Serial.print("SSID scanned: "); Serial.println(pendingWifiSsid);
    flashBlueForSsidScanned();
    return;
  }

  setLed(false, false, true); // steady blue while connecting
  Serial.print("Connecting using SSID: "); Serial.println(pendingWifiSsid);
  pendingWifiPass = scan;
  startWiFiConnect(pendingWifiSsid.c_str(), pendingWifiPass.c_str(), WIFI_PURPOSE_PROVISION);
}

bool containsBarcode(const String& value) {
  BarcodeNode* current = head;
  while (current) {
    if (current->value == value) return true;
    current = current->next;
  }
  return false;
}

bool appendBarcode(const String& value) {
  BarcodeNode* node = new (std::nothrow) BarcodeNode{value, nullptr};
  if (!node) return false;

  if (!head) {
    head = node;
    tail = node;
  } else {
    tail->next = node;
    tail = node;
  }
  count++;
  return true;
}

bool popLastBarcode() {
  if (!head) return false;

  if (head == tail) {
    delete head;
    head = nullptr;
    tail = nullptr;
    count = 0;
    return true;
  }

  BarcodeNode* current = head;
  while (current->next != tail) current = current->next;

  delete tail;
  tail = current;
  tail->next = nullptr;
  count--;
  return true;
}

void setup() {
  Serial.begin(115200);
  while (!Serial);
  
  pinMode(buzzerPin, OUTPUT);
  pinMode(redPin, OUTPUT);
  pinMode(greenPin, OUTPUT);
  pinMode(bluePin, OUTPUT);
  // Show explicit startup/waiting state immediately.
  setLed(true, false, false);
  
  if (Usb.Init() == -1) { Serial.println("USB failed"); while(1); }
  delay(200);
  Kbd.SetReportParser(0, &parser);
  initEepromStorage();

  bool hasSaved = loadSavedWiFi();
  Serial.println("Trying saved/default WiFi...");
  setLed(true, false, false);
  if (hasSaved) {
    startWiFiConnect(ssid, pass, WIFI_PURPOSE_BOOT_SAVED);
  } else {
    copyCredentials(defaultSsid, defaultPass);
    startWiFiConnect(defaultSsid, defaultPass, WIFI_PURPOSE_BOOT_DEFAULT);
  }
  
  lastScan = millis();
  printList();
}

void loop() {
  Usb.Task();
  handleWiFiConnect();

  // Safety: never leave scanner locked once connect attempt is done.
  if (!wifiConnectInProgress && scannerInputLocked) {
    unlockScannerInput();
    // Force clear any buffered input after WiFi operations
    barcode = "";
    ready = false;
  }

  // Handle completed WiFi attempts by purpose.
  static bool lastConnectInProgress = false;
  if (lastConnectInProgress && !wifiConnectInProgress && wifiState == WIFI_IDLE) {
    bool connected = (WiFi.status() == WL_CONNECTED);
    WiFiConnectPurpose finishedPurpose = wifiPurpose;
    wifiPurpose = WIFI_PURPOSE_NONE;

    if (finishedPurpose == WIFI_PURPOSE_BOOT_SAVED) {
      if (connected) {
        setLed(false, true, false);
        playWiFiConnectedTune();
        Serial.print("WiFi connected. IP: "); Serial.println(WiFi.localIP());
      } else {
        copyCredentials(defaultSsid, defaultPass);
        startWiFiConnect(defaultSsid, defaultPass, WIFI_PURPOSE_BOOT_DEFAULT);
      }
    } else if (finishedPurpose == WIFI_PURPOSE_BOOT_DEFAULT) {
      if (connected) {
        setLed(false, true, false);
        playWiFiConnectedTune();
        Serial.print("Connected with default WiFi. IP: "); Serial.println(WiFi.localIP());
      } else {
        enterWifiSetupMode();
      }
    } else if (finishedPurpose == WIFI_PURPOSE_PROVISION) {
      if (connected) {
        saveWiFi(pendingWifiSsid.c_str(), pendingWifiPass.c_str());
        wifiSetupMode = false;
        pendingWifiSsid = "";
        pendingWifiPass = "";
        setLed(false, true, false);
        playWiFiConnectedTune();
        Serial.println("WiFi connected and saved.");
      } else {
        pendingWifiSsid = "";
        pendingWifiPass = "";
        setLed(true, false, false);
        beepWifiAlert();
        Serial.println("WiFi connect failed. Credentials not saved.");
      }
    } else if (finishedPurpose == WIFI_PURPOSE_RECONNECT) {
      if (connected) {
        setLed(false, true, false);
        Serial.print("Reconnected IP: "); Serial.println(WiFi.localIP());
      } else {
        enterWifiSetupMode();
      }
    }
  }
  lastConnectInProgress = wifiConnectInProgress;

  // Periodic WiFi check every 30 seconds
  static unsigned long lastWifiCheck = 0;
  if (!wifiSetupMode && !wifiConnectInProgress && millis() - lastWifiCheck > 30000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected, attempting reconnect...");
      startWiFiConnect(ssid, pass, WIFI_PURPOSE_RECONNECT);
    }
  }

  if (ready) {
    String scanned = barcode;
    barcode = "";
    ready = false;
    scanned.trim();

    if (scanned == "RESET_WIFI") {
      clearSavedWiFi();
      WiFi.disconnect();
      enterWifiSetupMode();
      Serial.println("Saved WiFi cleared");
      lastScan = millis();
      return;
    }

    if (wifiSetupMode) {
      // In setup mode: first scan is SSID, second scan is password.
      handleWifiProvisionScan(scanned);
      lastScan = millis();
      return;
    }

    process(scanned);
    lastScan = millis();
  }

  if (count > 0 && millis() - lastScan > TIMEOUT_MS) {
    beepError();
    reset();
    printList();
    lastScan = millis();
  }
}

void process(String b) {
  String bUpper = b;
  bUpper.trim();
  bUpper.toUpperCase();
  beepScan();
  Serial.print("> "); Serial.println(bUpper);

  if (bUpper == "SEND") {
    // No blue light for SEND - handle separately
    if (count < 2) { Serial.println("Need 2+ barcodes"); beepError(); }
    else {
      int result = relocateWithStatus();
      if (result == 1) {
        Serial.println("OK");
        // Success - blink blue 3 times (longer duration)
        for (int i = 0; i < 3; i++) {
          digitalWrite(greenPin, LOW);
          digitalWrite(bluePin, HIGH);
          tone(buzzerPin, 2000, 100);
          delay(300);
          digitalWrite(bluePin, LOW);
          digitalWrite(greenPin, HIGH);
          delay(150);
          Usb.Task();
        }
        reset();
      }
      else if (result == -1) {
        // Connection error - distinct feedback, keep barcodes for retry
        beepConnectionError();
        Serial.println("Connection failed - barcodes kept, try SEND again");
        // Don't reset - let user retry
      }
      else {
        // Server rejected (result == 0)
        Serial.println("FAILED - Server rejected");
        beepError();
        reset();
      }
    }
    printList();
    return;
  }

  // Blue light while processing regular barcodes
  digitalWrite(greenPin, LOW);
  digitalWrite(bluePin, HIGH);

  if (bUpper == "UNDO") { if (count > 0) { popLastBarcode(); beepOK(); } }
  else if (bUpper == "RESET") { reset(); beepOK(); }
  else {
    if (containsBarcode(bUpper)) { Serial.println("Duplicate"); beepError(); printList(); digitalWrite(bluePin, LOW); digitalWrite(greenPin, HIGH); return; }
    if (!appendBarcode(bUpper)) { Serial.println("Out of memory"); beepError(); printList(); digitalWrite(bluePin, LOW); digitalWrite(greenPin, HIGH); return; }
    Serial.println("Added");
    beepOK();
  }

  // Back to green after processing
  digitalWrite(bluePin, LOW);
  digitalWrite(greenPin, HIGH);
  printList();
}

void reset() {
  BarcodeNode* current = head;
  while (current) {
    BarcodeNode* next = current->next;
    delete current;
    current = next;
  }
  head = nullptr;
  tail = nullptr;
  count = 0;
}

void beepOK() { tone(buzzerPin, 2000, 30); delay(50); }
void beepError() { 
  digitalWrite(bluePin, LOW);
  digitalWrite(greenPin, LOW);
  digitalWrite(redPin, HIGH); 
  tone(buzzerPin, 500, 80); 
  delay(100); 
  digitalWrite(redPin, LOW); 
  digitalWrite(greenPin, HIGH);
}
void beepConnectionError() {
  // Super Mario Bros - Game Over Theme (Server Not Responding 🎮)
  Serial.println("CONNECTION ERROR - Server not responding!");
  digitalWrite(greenPin, LOW);
  digitalWrite(bluePin, LOW);
  digitalWrite(redPin, HIGH);
  
  // Game Over melody
  tone(buzzerPin, 392, 200); delay(220);   // G4
  Usb.Task();
  tone(buzzerPin, 350, 200); delay(220);   // ~F4
  Usb.Task();
  tone(buzzerPin, 330, 200); delay(220);   // E4
  Usb.Task();
  
  // Blink red/blue
  digitalWrite(redPin, LOW);
  digitalWrite(bluePin, HIGH);
  
  tone(buzzerPin, 262, 200); delay(220);   // C4
  Usb.Task();
  tone(buzzerPin, 294, 200); delay(220);   // D4
  Usb.Task();
  tone(buzzerPin, 247, 200); delay(220);   // B3
  Usb.Task();
  
  digitalWrite(bluePin, LOW);
  digitalWrite(redPin, HIGH);
  
  tone(buzzerPin, 262, 300); delay(350);   // C4 (held)
  Usb.Task();
  tone(buzzerPin, 196, 500); delay(550);   // G3 (low end)
  
  digitalWrite(redPin, LOW);
  digitalWrite(greenPin, HIGH); // Return to ready state
}
void beepScan() { tone(buzzerPin, 1500, 15); }

void printList() {
  Serial.print("List["); Serial.print(count); Serial.print("]: ");
  BarcodeNode* current = head;
  while (current) {
    Serial.print(current->value);
    if (current->next) Serial.print(",");
    current = current->next;
  }
  Serial.println();
}

// Return codes: 1 = success, 0 = server rejected, -1 = connection error
int httpRequestWithStatus(String method, String url, String body = "") {
  // Check WiFi and reconnect if needed
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    if (!wifiConnectInProgress) {
      startWiFiConnect(ssid, pass, WIFI_PURPOSE_RECONNECT);
    }
    return -1;
  }
  
  // Make sure previous connection is closed
  client.stop();
  delay(10);
  
  // Try to connect with retries
  int connectRetries = 2;
  bool connected = false;
  while (connectRetries > 0 && !connected) {
    Usb.Task();
    if (client.connect(serverHost, serverPort)) {
      connected = true;
    } else {
      connectRetries--;
      if (connectRetries > 0) {
        Serial.println("Connect retry...");
        delay(500);
        Usb.Task();
      }
    }
  }
  
  if (!connected) {
    Serial.println("Connect failed - server not responding");
    client.stop();
    return -1; // Connection error
  }
  
  client.print(method + " " + url + " HTTP/1.1\r\n");
  client.print("Host: " + String(serverHost) + "\r\n");
  client.print("CF-Access-Client-Id: " + String(cfClientId) + "\r\n");
  client.print("CF-Access-Client-Secret: " + String(cfClientSecret) + "\r\n");
  if (body.length() > 0) {
    client.print("Content-Type: application/json\r\n");
    client.print("Content-Length: " + String(body.length()) + "\r\n");
  }
  client.print("Connection: close\r\n\r\n");
  if (body.length() > 0) client.print(body);
  client.flush();
  
  // Wait for response with USB task running
  unsigned long t = millis();
  while (!client.available()) {
    Usb.Task(); // Keep USB alive
    if (millis() - t > 5000) { 
      Serial.println("Response timeout");
      client.stop(); 
      return -1; // Connection/timeout error
    }
    delay(10);
  }
  
  // Read status line to check for HTTP 200.
  bool is200 = false;
  bool statusLineRead = false;
  String statusLine = "";
  
  t = millis();
  while (client.connected() || client.available()) {
    if (millis() - t > 3000) break;
    Usb.Task();
    while (client.available()) {
      char c = client.read();
      
      // Read the first line (status line) to get HTTP status code
      if (!statusLineRead) {
        if (c == '\r' || c == '\n') {
          statusLineRead = true;
          // Check for 200 status code
          if (statusLine.indexOf("200") >= 0) is200 = true;
        } else {
          statusLine += c;
        }
      }
      
    }
    delay(1);
  }
  
  client.stop();
  
  // Relocate endpoint succeeds on HTTP 200.
  if (!is200) return 0;
  return 1;
}

// Returns: 1 = success, 0 = server rejected, -1 = connection error
int relocateWithStatus() {
  if (count < 2 || !head || !tail) return 0;

  String json = "{\"AssetBarcodes\":[";
  BarcodeNode* current = head;
  while (current && current != tail) {
    json += "\"" + current->value + "\"";
    if (current->next && current->next != tail) json += ",";
    current = current->next;
  }
  json += "],\"DestinationBarcode\":\"" + tail->value + "\"}";
  return httpRequestWithStatus("POST", "/api/Asset/relocate", json);
}
