// Barcode Scanner MVP - Arduino R4 WiFi + USB Host Shield
#include <WiFiS3.h>
#include <hidboot.h>
#include <usbhub.h>
#include <SPI.h>

// Config
char ssid[] = "prog";
char pass[] = "Alvorlig5And";
const char* serverIP = "10.108.131.103";
const int serverPort = 5000;
const int MAX_BARCODES = 20;
const unsigned long TIMEOUT_MS = 60000;

// Pins
const int buzzerPin = 8;
const int redPin = 7;
const int greenPin = 6;

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
WiFiClient client;

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
  
  if (Usb.Init() == -1) { Serial.println("USB failed"); while(1); }
  delay(200);
  Kbd.SetReportParser(0, &parser);
  
  while (status != WL_CONNECTED) { status = WiFi.begin(ssid, pass); delay(5000); }
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  digitalWrite(greenPin, HIGH); tone(buzzerPin, 2000, 30); delay(50); tone(buzzerPin, 2500, 30); delay(50); digitalWrite(greenPin, LOW);
  
  lastScan = millis();
  printList();
}

void loop() {
  Usb.Task();
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
    if (count < 2) { Serial.println("Need 2+ barcodes"); beepError(); }
    else if (relocate()) { Serial.println("OK"); beepOK(); beepOK(); reset(); }
    else { Serial.println("FAILED"); beepError(); reset(); }
  }
  else if (b == "UNDO") { if (count > 0) { count--; barcodes[count] = ""; beepOK(); } }
  else if (b == "RESET") { reset(); beepOK(); }
  else {
    if (count >= MAX_BARCODES) return;
    for (int i = 0; i < count; i++) if (barcodes[i] == b) { Serial.println("Duplicate"); beepError(); printList(); return; }
    if (validate(b)) { barcodes[count++] = b; Serial.println("Added"); beepOK(); }
    else { Serial.println("Invalid"); beepError(); }
  }
  printList();
}

void reset() { for (int i = 0; i < count; i++) barcodes[i] = ""; count = 0; }

void beepOK() { digitalWrite(greenPin, HIGH); tone(buzzerPin, 2000, 30); delay(50); digitalWrite(greenPin, LOW); }
void beepError() { digitalWrite(redPin, HIGH); tone(buzzerPin, 500, 80); delay(100); digitalWrite(redPin, LOW); }
void beepScan() { tone(buzzerPin, 1500, 15); }

void printList() {
  Serial.print("List["); Serial.print(count); Serial.print("]: ");
  for (int i = 0; i < count; i++) { Serial.print(barcodes[i]); if (i < count-1) Serial.print(","); }
  Serial.println();
}

bool httpRequest(String method, String url, String body = "") {
  if (!client.connect(serverIP, serverPort)) return false;
  
  client.print(method + " " + url + " HTTP/1.1\r\n");
  client.print("Host: " + String(serverIP) + ":" + serverPort + "\r\n");
  if (body.length() > 0) {
    client.print("Content-Type: application/json\r\n");
    client.print("Content-Length: " + String(body.length()) + "\r\n");
  }
  client.print("Connection: close\r\n\r\n");
  if (body.length() > 0) client.print(body);
  
  unsigned long t = millis();
  while (!client.available()) { if (millis() - t > 10000) { client.stop(); return false; } }
  
  String resp = "";
  while (client.available()) resp += (char)client.read();
  client.stop();
  
  int i = resp.indexOf("\r\n\r\n");
  return (i >= 0 && resp.substring(i + 4).indexOf("true") >= 0);
}

bool validate(String b) { return httpRequest("GET", "/api/Asset/validate/" + b); }

bool relocate() {
  String json = "{\"AssetBarcodes\":[";
  for (int i = 0; i < count - 1; i++) {
    json += "\"" + barcodes[i] + "\"";
    if (i < count - 2) json += ",";
  }
  json += "],\"DestinationBarcode\":\"" + barcodes[count - 1] + "\"}";
  return httpRequest("POST", "/api/Asset/relocate", json);
}
