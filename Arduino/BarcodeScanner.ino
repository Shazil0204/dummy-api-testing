/*
  Barcode Scanner with Asset Relocation System
  Arduino UNO R4 WiFi + USB Host Shield
  
  Features:
  - Connects to WiFi and displays IP
  - Validates barcodes via API endpoint
  - Maintains a list of valid scanned barcodes
  - Special commands: SEND, UNDO, RESET
  - Auto-reset after 60 seconds of inactivity
*/

#include <WiFiS3.h>
#include <hidboot.h>
#include <usbhub.h>
#include <SPI.h>

// --- WiFi Credentials ---
// char ssid[] = "Lab-ZBC";
char ssid[] = "prog";

// char pass[] = "Prestige#PuzzledCASH48!";
char pass[] = "Alvorlig5And";

int status = WL_IDLE_STATUS;

// --- API Server Configuration ---
const char* serverIP = "10.108.131.103";
const int serverPort = 5000;

// --- Barcode Storage ---
const int MAX_BARCODES = 20;
String barcodeList[MAX_BARCODES];
int barcodeCount = 0;

// --- Timeout Configuration ---
const unsigned long TIMEOUT_MS = 60000; // 60 seconds
unsigned long lastScanTime = 0;

// --- Special Command Barcodes ---
const String CMD_SEND = "SEND";
const String CMD_UNDO = "UNDO";
const String CMD_RESET = "RESET";

// --- USB Host Shield ---
USB Usb;
HIDBoot<USB_HID_PROTOCOL_KEYBOARD> HidKeyboard(&Usb);

// --- Barcode Buffer ---
String currentBarcode = "";
bool barcodeReady = false;

// --- Keyboard Parser Class ---
class KbdParser : public KeyboardReportParser {
  protected:
    void OnKeyDown(uint8_t mod, uint8_t key) override;
    void OnKeyPressed(uint8_t key);
};

void KbdParser::OnKeyDown(uint8_t mod, uint8_t key) {
  uint8_t c = OemToAscii(mod, key);
  if (c) {
    OnKeyPressed(c);
  }
}

void KbdParser::OnKeyPressed(uint8_t key) {
  // Enter key signals end of barcode
  if (key == 13 || key == 10) {
    if (currentBarcode.length() > 0) {
      barcodeReady = true;
    }
  } else {
    currentBarcode += (char)key;
  }
}

KbdParser kbdParser;

// --- WiFi Client ---
WiFiClient client;

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    ; // Wait for serial port to connect
  }
  
  Serial.println("=================================");
  Serial.println("Barcode Scanner System Starting");
  Serial.println("=================================");
  
  // Initialize USB Host Shield
  if (Usb.Init() == -1) {
    Serial.println("USB Host Shield initialization failed!");
    while (1); // Halt
  }
  Serial.println("USB Host Shield initialized");
  
  delay(200);
  HidKeyboard.SetReportParser(0, &kbdParser);
  
  // Connect to WiFi
  connectToWiFi();
  
  // Initialize timeout timer
  lastScanTime = millis();
  
  Serial.println("\n=================================");
  Serial.println("System Ready - Scan a barcode");
  Serial.println("Commands: SEND, UNDO, RESET");
  Serial.println("=================================\n");
  printBarcodeList();
}

void loop() {
  // Process USB events
  Usb.Task();
  
  // Check for timeout
  checkTimeout();
  
  // Process barcode if ready
  if (barcodeReady) {
    processBarcode(currentBarcode);
    currentBarcode = "";
    barcodeReady = false;
    lastScanTime = millis(); // Reset timeout
  }
}

// --- WiFi Connection ---
void connectToWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  
  // Check for WiFi module
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("WiFi module not found!");
    while (true);
  }
  
  // Attempt to connect
  while (status != WL_CONNECTED) {
    Serial.print("Attempting connection...");
    status = WiFi.begin(ssid, pass);
    
    // Wait for connection
    delay(5000);
  }
  
  // Connected!
  Serial.println("\n*** WiFi Connected! ***");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Signal Strength (RSSI): ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
}

// --- Timeout Check ---
void checkTimeout() {
  if (barcodeCount > 0 && (millis() - lastScanTime > TIMEOUT_MS)) {
    Serial.println("\n*** TIMEOUT - Resetting list ***");
    resetList();
    lastScanTime = millis();
  }
}

// --- Process Scanned Barcode ---
void processBarcode(String barcode) {
  barcode.trim();
  barcode.toUpperCase();
  
  Serial.println("\n---------------------------------");
  Serial.print("Scanned: ");
  Serial.println(barcode);
  
  // Check for special commands
  if (barcode == CMD_SEND) {
    handleSend();
  } else if (barcode == CMD_UNDO) {
    handleUndo();
  } else if (barcode == CMD_RESET) {
    handleReset();
  } else {
    // Regular barcode - validate via API
    handleRegularBarcode(barcode);
  }
  
  printBarcodeList();
}

// --- Handle Regular Barcode ---
void handleRegularBarcode(String barcode) {
  if (barcodeCount >= MAX_BARCODES) {
    Serial.println("ERROR: Maximum barcode limit reached!");
    return;
  }
  
  // Check if already in list
  for (int i = 0; i < barcodeCount; i++) {
    if (barcodeList[i] == barcode) {
      Serial.println("WARNING: Barcode already in list!");
      return;
    }
  }
  
  // Validate barcode via API
  Serial.print("Validating barcode... ");
  bool isValid = validateBarcode(barcode);
  
  if (isValid) {
    Serial.println("VALID");
    barcodeList[barcodeCount] = barcode;
    barcodeCount++;
    Serial.println("Barcode added to list.");
  } else {
    Serial.println("INVALID");
    Serial.println("Barcode NOT added to list.");
  }
}

// --- Handle SEND Command ---
void handleSend() {
  Serial.println("Command: SEND");
  
  // Validation: Need at least 2 barcodes
  if (barcodeCount < 2) {
    Serial.println("ERROR: Need at least 2 barcodes!");
    Serial.print("Current count: ");
    Serial.println(barcodeCount);
    Serial.println("(Assets + 1 Destination Location)");
    return;
  }
  
  // Call relocation endpoint
  bool success = callRelocationEndpoint();
  
  if (success) {
    Serial.println("*** RELOCATION SUCCESSFUL ***");
    resetList();
  } else {
    Serial.println("*** RELOCATION FAILED ***");
  }
}

// --- Handle UNDO Command ---
void handleUndo() {
  Serial.println("Command: UNDO");
  
  if (barcodeCount > 0) {
    barcodeCount--;
    Serial.print("Removed: ");
    Serial.println(barcodeList[barcodeCount]);
    barcodeList[barcodeCount] = "";
  } else {
    Serial.println("List is already empty!");
  }
}

// --- Handle RESET Command ---
void handleReset() {
  Serial.println("Command: RESET");
  resetList();
}

// --- Reset the Barcode List ---
void resetList() {
  for (int i = 0; i < barcodeCount; i++) {
    barcodeList[i] = "";
  }
  barcodeCount = 0;
  Serial.println("List cleared.");
}

// --- Print Current Barcode List ---
void printBarcodeList() {
  Serial.println("\n=== Current Barcode List ===");
  
  if (barcodeCount == 0) {
    Serial.println("(empty)");
  } else {
    for (int i = 0; i < barcodeCount; i++) {
      Serial.print(i + 1);
      Serial.print(". ");
      Serial.print(barcodeList[i]);
      
      // Label the last one as destination
      if (i == barcodeCount - 1 && barcodeCount >= 2) {
        Serial.print(" <- Destination Location");
      } else if (barcodeCount >= 2) {
        Serial.print(" <- Asset");
      }
      Serial.println();
    }
  }
  
  Serial.print("Total: ");
  Serial.print(barcodeCount);
  Serial.println(" barcode(s)");
  Serial.println("============================\n");
}

// --- API: Validate Barcode ---
bool validateBarcode(String barcode) {
  if (!client.connect(serverIP, serverPort)) {
    Serial.println("Connection to server failed!");
    return false;
  }
  
  // Build GET request
  String url = "/api/Asset/validate/" + barcode;
  
  Serial.print("Requesting: ");
  Serial.println(url);
  
  client.print("GET ");
  client.print(url);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.print(serverIP);
  client.print(":");
  client.println(serverPort);
  client.println("Connection: close");
  client.println();
  
  // Wait for response with longer timeout
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 10000) {
      Serial.println("Request timeout!");
      client.stop();
      return false;
    }
  }
  
  // Read entire response
  String fullResponse = "";
  while (client.available()) {
    char c = client.read();
    fullResponse += c;
  }
  client.stop();
  
  // Debug: Print full response
  Serial.println("--- Raw Response ---");
  Serial.println(fullResponse);
  Serial.println("--- End Response ---");
  
  // Find the body (after double CRLF)
  int bodyStart = fullResponse.indexOf("\r\n\r\n");
  if (bodyStart == -1) {
    Serial.println("Could not find response body!");
    return false;
  }
  
  String body = fullResponse.substring(bodyStart + 4);
  body.trim();
  body.toLowerCase();
  
  Serial.print("Parsed body: '");
  Serial.print(body);
  Serial.println("'");
  
  // Check for true in various formats
  return (body == "true" || body.indexOf("true") >= 0);
}

// --- API: Call Relocation Endpoint ---
bool callRelocationEndpoint() {
  if (!client.connect(serverIP, serverPort)) {
    Serial.println("Connection to server failed!");
    return false;
  }
  
  // Build JSON payload
  // All barcodes except the last one are AssetBarcodes
  // The last barcode is the DestinationBarcode
  String jsonPayload = "{\"AssetBarcodes\":[";
  
  // Add all barcodes except the last one as assets
  for (int i = 0; i < barcodeCount - 1; i++) {
    jsonPayload += "\"" + barcodeList[i] + "\"";
    if (i < barcodeCount - 2) {
      jsonPayload += ",";
    }
  }
  
  jsonPayload += "],\"DestinationBarcode\":\"";
  jsonPayload += barcodeList[barcodeCount - 1]; // Last one is destination
  jsonPayload += "\"}";
  
  Serial.println("JSON Payload:");
  Serial.println(jsonPayload);
  
  // Build POST request
  client.println("POST /api/Asset/relocate HTTP/1.1");
  client.print("Host: ");
  client.print(serverIP);
  client.print(":");
  client.println(serverPort);
  client.println("Content-Type: application/json");
  client.print("Content-Length: ");
  client.println(jsonPayload.length());
  client.println("Connection: close");
  client.println();
  client.println(jsonPayload);
  
  // Wait for response with longer timeout
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 10000) {
      Serial.println("Request timeout!");
      client.stop();
      return false;
    }
  }
  
  // Read entire response
  String fullResponse = "";
  while (client.available()) {
    char c = client.read();
    fullResponse += c;
  }
  client.stop();
  
  // Find the body (after double CRLF)
  int bodyStart = fullResponse.indexOf("\r\n\r\n");
  if (bodyStart == -1) {
    Serial.println("Could not find response body!");
    return false;
  }
  
  String body = fullResponse.substring(bodyStart + 4);
  body.trim();
  body.toLowerCase();
  
  Serial.print("Server Response: ");
  Serial.println(body);
  
  // Check for true in various formats (handles chunked encoding)
  return (body == "true" || body.indexOf("true") >= 0);
}
