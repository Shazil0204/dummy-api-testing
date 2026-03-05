// Barcode Scanner MVP - Arduino R4 WiFi + USB Host Shield
#include <WiFiS3.h>
#include <hidboot.h>
#include <usbhub.h>
#include <SPI.h>

// Config
char ssid[] = "prog";
char pass[] = "Alvorlig5And";
const char* serverHost = "inventorysystem.acceptable.pro";
const int serverPort = 443;
const char* cfClientId = "fd0f2455c8ef15b1f83d6315190ee48b.access";
const char* cfClientSecret = "7e4a0b4cefbd617e5b0a9be60444ea07eafcd7d6c2fe76cddf458ad5885bf7e4";
const int MAX_BARCODES = 200; // Max ~200 for Arduino R4 WiFi RAM limits
const unsigned long TIMEOUT_MS = 60000;

// Pins
const int buzzerPin = 8;
const int redPin = 7;
const int greenPin = 6;
const int bluePin = 5;

// State
String barcodes[MAX_BARCODES];
int count = 0;
int status = WL_IDLE_STATUS;
unsigned long lastScan = 0;
String barcode = "";
bool ready = false;

// USB
USB Usb;
HIDBoot<USB_HID_PROTOCOL_KEYBOARD> Kbd(&Usb);
WiFiSSLClient client; // SSL client for HTTPS

class KbdParser : public KeyboardReportParser {
protected:
  void OnKeyDown(uint8_t mod, uint8_t key) override {
    uint8_t c = OemToAscii(mod, key);
    if (c) {
      if (c == 13 || c == 10) { if (barcode.length() > 0) ready = true; }
      else barcode += (char)c;
    }
  }
};
KbdParser parser;

void setup() {
  Serial.begin(115200);
  while (!Serial);
  
  pinMode(buzzerPin, OUTPUT);
  pinMode(redPin, OUTPUT);
  pinMode(greenPin, OUTPUT);
  pinMode(bluePin, OUTPUT);
  
  if (Usb.Init() == -1) { Serial.println("USB failed"); while(1); }
  delay(200);
  Kbd.SetReportParser(0, &parser);
  
  // WiFi connection with visual/audio feedback
  Serial.println("Connecting to WiFi...");
  while (status != WL_CONNECTED) {
    status = WiFi.begin(ssid, pass);
    if (status != WL_CONNECTED) {
      // Connection failed - red light + error beep
      digitalWrite(greenPin, LOW);
      digitalWrite(redPin, HIGH);
      // Super Mario Bros - Death Sound (WiFi Failed ❌)

      tone(buzzerPin, 523, 150); delay(180);   // C5
      tone(buzzerPin, 494, 150); delay(180);   // B4
      tone(buzzerPin, 466, 150); delay(180);   // A#4
      tone(buzzerPin, 440, 300); delay(320);   // A4

      tone(buzzerPin, 415, 150); delay(180);   // G#4
      tone(buzzerPin, 392, 400); delay(450);   // G4 (sad fall)
      Serial.println("WiFi connection failed, retrying in 5 sec...");
      delay(5000);
      digitalWrite(redPin, LOW);
    }
  }
  // Connected - turn green LED on and keep it on
  digitalWrite(redPin, LOW);
  digitalWrite(greenPin, HIGH);
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  
  // Super Mario Bros - Intro (WiFi Connected Style 🎮)

  tone(buzzerPin, 659, 150); delay(180);  // E5
  tone(buzzerPin, 659, 150); delay(180);  // E5
  delay(150);

  tone(buzzerPin, 659, 150); delay(180);  // E5
  delay(150);

  tone(buzzerPin, 523, 150); delay(180);  // C5
  tone(buzzerPin, 659, 150); delay(180);  // E5
  tone(buzzerPin, 784, 300); delay(350);  // G5
  delay(200);

  tone(buzzerPin, 392, 300); delay(350);  // G4
  
  lastScan = millis();
  printList();
}

void loop() {
  Usb.Task();
  
  // Periodic WiFi check every 30 seconds
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 30000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected, reconnecting...");
      digitalWrite(greenPin, LOW);
      digitalWrite(redPin, HIGH);
      tone(buzzerPin, 500, 200);
      WiFi.disconnect();
      delay(100);
      status = WL_IDLE_STATUS;
      while (status != WL_CONNECTED) {
        status = WiFi.begin(ssid, pass);
        if (status != WL_CONNECTED) {
          tone(buzzerPin, 500, 200);
          delay(5000);
        }
      }
      digitalWrite(redPin, LOW);
      digitalWrite(greenPin, HIGH);
      Serial.print("Reconnected IP: "); Serial.println(WiFi.localIP());
    }
  }
  
  if (count > 0 && millis() - lastScan > TIMEOUT_MS) { beepError(); reset(); printList(); lastScan = millis(); }
  if (ready) {
    process(barcode);
    barcode = "";
    ready = false;
    lastScan = millis();
  }
}

void process(String b) {
  b.trim(); b.toUpperCase();
  beepScan();
  Serial.print("> "); Serial.println(b);
  
  if (b == "SEND") {
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
  
  if (b == "UNDO") { if (count > 0) { count--; barcodes[count] = ""; beepOK(); } }
  else if (b == "RESET") { reset(); beepOK(); }
  else {
    if (count >= MAX_BARCODES) { beepError(); printList(); digitalWrite(bluePin, LOW); digitalWrite(greenPin, HIGH); return; }
    for (int i = 0; i < count; i++) if (barcodes[i] == b) { Serial.println("Duplicate"); beepError(); printList(); digitalWrite(bluePin, LOW); digitalWrite(greenPin, HIGH); return; }
    int result = validateWithStatus(b);
    if (result == 1) { 
      barcodes[count++] = b; 
      Serial.println("Added"); 
      beepOK(); 
    }
    else if (result == -1) {
      // Connection error - distinct feedback
      Serial.println("Cannot validate - server not responding");
      beepConnectionError();
    }
    else { 
      Serial.println("Invalid"); 
      beepError(); 
    }
  }
  // Back to green after processing
  digitalWrite(bluePin, LOW);
  digitalWrite(greenPin, HIGH);
  printList();
}

void reset() { for (int i = 0; i < count; i++) barcodes[i] = ""; count = 0; }

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
  for (int i = 0; i < count; i++) { Serial.print(barcodes[i]); if (i < count-1) Serial.print(","); }
  Serial.println();
}

// Return codes: 1 = success, 0 = server said no/invalid, -1 = connection error
int httpRequestWithStatus(String method, String url, String body = "") {
  // Check WiFi and reconnect if needed
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    digitalWrite(greenPin, LOW);
    digitalWrite(bluePin, LOW);
    digitalWrite(redPin, HIGH);
    tone(buzzerPin, 500, 200);
    WiFi.disconnect();
    delay(100);
    status = WL_IDLE_STATUS;
    int retryCount = 0;
    while (status != WL_CONNECTED && retryCount < 3) {
      Usb.Task(); // Keep USB alive
      status = WiFi.begin(ssid, pass);
      if (status != WL_CONNECTED) {
        tone(buzzerPin, 500, 200);
        retryCount++;
        for (int i = 0; i < 50; i++) { delay(100); Usb.Task(); } // 5 sec with USB task
      }
    }
    if (status != WL_CONNECTED) {
      digitalWrite(redPin, LOW);
      digitalWrite(greenPin, HIGH);
      return -1; // Connection error
    }
    digitalWrite(redPin, LOW);
    digitalWrite(greenPin, HIGH);
    Serial.print("Reconnected IP: "); Serial.println(WiFi.localIP());
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
  
  // Read status line to check for 200 OK and body content
  bool is200 = false;
  bool bodyTrue = false;
  bool bodyFalse = false;
  bool statusLineRead = false;
  bool inBody = false;
  int crlfCount = 0;
  String statusLine = "";
  String bodyContent = "";
  
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
      
      if (!inBody) {
        if (c == '\r' || c == '\n') crlfCount++;
        else crlfCount = 0;
        if (crlfCount >= 4) inBody = true;
      } else {
        // Collect body content (limit to prevent memory issues)
        if (bodyContent.length() < 50) {
          bodyContent += c;
        }
      }
    }
    delay(1);
  }
  
  // Check body content for true/false
  bodyContent.toLowerCase();
  if (bodyContent.indexOf("true") >= 0) bodyTrue = true;
  if (bodyContent.indexOf("false") >= 0) bodyFalse = true;
  
  client.stop();
  
  // Success if: HTTP 200 AND (body says "true" OR body doesn't say "false")
  // This handles both validate (returns true/false) and relocate (returns 200 on success)
  if (!is200) return 0; // Server returned error status
  if (bodyFalse) return 0; // Body explicitly says false
  return 1; // 200 with "true" or no true/false in body = success
}

// Legacy wrapper for backwards compatibility
bool httpRequest(String method, String url, String body = "") {
  return httpRequestWithStatus(method, url, body) == 1;
}

// Returns: 1 = valid, 0 = invalid, -1 = connection error
int validateWithStatus(String b) { return httpRequestWithStatus("GET", "/api/Asset/validate/" + b); }

// Returns: 1 = success, 0 = server rejected, -1 = connection error
int relocateWithStatus() {
  String json = "{\"AssetBarcodes\":[";
  for (int i = 0; i < count - 1; i++) {
    json += "\"" + barcodes[i] + "\"";
    if (i < count - 2) json += ",";
  }
  json += "],\"DestinationBarcode\":\"" + barcodes[count - 1] + "\"}";
  return httpRequestWithStatus("POST", "/api/Asset/relocate", json);
}
