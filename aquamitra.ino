#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <WiFi.h>      // For Wi-Fi connectivity
#include <HTTPClient.h>  // For making HTTP requests
#include <ArduinoJson.h> // For creating the JSON payload
#include "time.h"      // For timestamp generation

const char* ssid = "Nokia"; 
const char* password = "12345678";

const char* serverUrl = "http://192.168.137.179:5000/api/log"; 

// --- Time Configuration (For accurate timestamps) ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800; // +5:30 IST
const int   daylightOffset_sec = 0; 

// ---------------- LCD ----------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------- RFID ----------------
#define RXD2 16     // EM-18 TX → ESP32 GPIO16 (RX2)
#define TXD2 17     // Not used

// Map Card Tag to User ID and Balance Pointer
struct User {
    String rfidTag;
    String userId;
    float balance; // balance in mL
};

User users[] = {
    {"2500D5428A38", "aqm001", 220000}, // User 1
    {"2500D5483088", "aqm002", 165000}  // User 2
};
const int NUM_USERS = sizeof(users) / sizeof(users[0]);

// ---------------- Hardware Pins ----------------
#define RELAY_PIN 0    
#define LED_PIN 18
#define FLOW_ADC_PIN 35 
float maxFlowRate = 100.0;  // mL/s at 3.3V

// ---------------- 4x4 Matrix Keypad ----------------
const byte ROWS = 4;
const byte COLS = 4;
char hexaKeys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS] = {26, 25, 33, 32};
Keypad customKeypad = Keypad(makeKeymap(hexaKeys), rowPins, colPins, ROWS, COLS);

// ---------------- Global Variables ----------------
String enteredAmountStr = "";
User *activeUser = NULL;    // Pointer to the selected user struct

// ---------------- Function Declarations ----------------
void connectToWiFi();
void handleValidCard();
int getWaterAmount();
void startDispensing(int enteredAmount);
bool sendDataToServer(String userId, int amountLiters); // Returns success status
void resetLCD();

// ====================================================================
// ---------------- Setup ----------------
// ====================================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);

  lcd.init();
  lcd.backlight();

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // Relay OFF
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); // LED OFF
  
  // Initialize Wi-Fi and Time
  connectToWiFi();
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  lcd.setCursor(0, 0);
  lcd.print("RFID Water Sys");
  lcd.setCursor(0, 1);
  lcd.print("Place Your Card");
}

// ====================================================================
// ---------------- Main Loop ----------------
// ====================================================================
void loop() {
  if (Serial2.available()) {
    String rfidTag = "";
    while (Serial2.available()) {
      char c = Serial2.read();
      if (c == '\n' || c == '\r') continue;
      rfidTag += c;
      delay(5);
    }

    if (rfidTag.length() > 0) {
      Serial.print("Card: ");
      Serial.println(rfidTag);

      activeUser = NULL;
      for (int i = 0; i < NUM_USERS; i++) {
          if (rfidTag == users[i].rfidTag) {
              activeUser = &users[i]; // Set the pointer to the matched user
              break;
          }
      }

      if (activeUser != NULL) {
        handleValidCard();
      } else {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Access Denied!");
        delay(2000);
        resetLCD();
      }
    }
  }
}

// ====================================================================
// ---------------- Wi-Fi and Helper Functions ----------------
// ====================================================================

void connectToWiFi() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi");
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) { // Increased attempts for stability
    delay(500);
    lcd.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Connected!");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    delay(2000);
  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Failed!");
    lcd.setCursor(0, 1);
    lcd.print("Check PWD/SSID");
    delay(3000);
  }
}

void resetLCD() {
  lcd.clear();
  lcd.setCursor(0, 0);
  if (WiFi.status() == WL_CONNECTED) {
     lcd.print("Place Your Card");
  } else {
     lcd.print("WiFi Disconnected");
  }
}

// ====================================================================
// ---------------- Dispensing and Data Logging Logic ----------------
// ====================================================================

void handleValidCard() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("User: ");
  lcd.print(activeUser->userId); 
  lcd.setCursor(0, 1);
  lcd.print("Bal: ");
  lcd.print(activeUser->balance / 1000.0, 2); 
  lcd.print(" L");
  delay(2500);

  enteredAmountStr = "";
  int enteredAmountL = getWaterAmount(); // Amount in Liters

  if (enteredAmountL * 1000.0 <= activeUser->balance && enteredAmountL > 0) {
    startDispensing(enteredAmountL);
  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Not Enough Bal");
    delay(2000);
    resetLCD();
  }
}

int getWaterAmount() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Enter Amt (L):");

  while (true) {
    char key = customKeypad.getKey();

    if (key) {
      if (key >= '0' && key <= '9') {
        if (enteredAmountStr.length() < 3) { 
           enteredAmountStr += key;
        }
      } else if (key == 'A') { // Clear input
        enteredAmountStr = "";
      } else if (key == '*') { // Cancel
        resetLCD();
        return 0;
      } else if (key == '#') { // Confirm
        if (enteredAmountStr.length() > 0) {
          return enteredAmountStr.toInt();
        }
      }
      
      lcd.setCursor(0, 1);
      lcd.print("Amt: ");
      lcd.print(enteredAmountStr);
      lcd.print(" L  "); 
      delay(200);
    }
  }
}

void startDispensing(int enteredAmountL) {
  int enteredAmount_mL = enteredAmountL * 1000; 
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Dispensing...");

  digitalWrite(RELAY_PIN, LOW); // ON
  digitalWrite(LED_PIN, HIGH);
  
  float dispensed = 0;
  unsigned long lastMillis = millis();
  float voltage;
  float flowRate;
  
  while (dispensed < enteredAmount_mL) {
    int adcValue = analogRead(FLOW_ADC_PIN);
    voltage = (adcValue / 4095.0) * 5.0; 
    flowRate = (voltage / 5.0) * maxFlowRate;  

    unsigned long now = millis();
    float deltaTime = (now - lastMillis) / 1000.0;
    lastMillis = now;

    dispensed += flowRate * deltaTime;

    lcd.setCursor(0, 1);
    lcd.print((int)dispensed);
    lcd.print("/");
    lcd.print(enteredAmount_mL);
    lcd.print(" mL    ");

    delay(200);
  }

  digitalWrite(RELAY_PIN, HIGH); // OFF
  digitalWrite(LED_PIN, LOW);
  
  activeUser->balance -= dispensed; 

  int dispensedL = (int)round(dispensed / 1000.0);
  
  // Call the function to send data to the server
  bool isSent = sendDataToServer(activeUser->userId, dispensedL); 

  if (isSent) {
      Serial.println("Data successfully logged on server.");
  } else {
      Serial.println("WARNING: Data failed to log on server.");
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Done!");
  lcd.setCursor(0, 1);
  lcd.print("Bal: ");
  lcd.print(activeUser->balance / 1000.0, 2); 
  lcd.print(" L");
  delay(3000);

  resetLCD();
}


// ---------------- Send Data to Server (Your provided logic, integrated with NTP time) ----------------
bool sendDataToServer(String userId, int amountLiters) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Error: WiFi not connected. Cannot send data.");
    return false;
  }

  HTTPClient http;
  
  // 1. Get current timestamp from NTP
  struct tm timeinfo;
  char timestamp_buffer[30];
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time. Using placeholder.");
    // Fallback to a fixed placeholder if time sync fails
    sprintf(timestamp_buffer, "01-01-2000 00:00:00"); 
  } else {
    // Format: "DD-MM-YYYY HH:MM:SS" (Your required format)
    strftime(timestamp_buffer, sizeof(timestamp_buffer), "%d-%m-%Y %H:%M:%S", &timeinfo); 
  }
  
  // 2. Create JSON object
  StaticJsonDocument<100> doc;
  doc["userID"] = userId;
  doc["amount"] = amountLiters; // Amount in Liters
  doc["timestamp"] = timestamp_buffer;
  
  String jsonPayload;
  serializeJson(doc, jsonPayload);
  
  Serial.print("Sending JSON: ");
  Serial.println(jsonPayload);
  
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");

  // 3. Send POST request and check response code
  int httpResponseCode = http.POST(jsonPayload);
  bool success = false;

  if (httpResponseCode > 0) {
    if (httpResponseCode >= 200 && httpResponseCode < 300) {
      Serial.print("Data transmission SUCCESS! HTTP Code: ");
      Serial.println(httpResponseCode);
      String response = http.getString();
      Serial.print("Server Response: ");
      Serial.println(response);
      success = true;
    } else {
      Serial.print("Data transmission FAILED! HTTP Code: ");
      Serial.println(httpResponseCode);
      Serial.print("Server Error: ");
      Serial.println(http.getString());
    }
  } else {
    // Connection failed (e.g., server down or firewall blocking)
    Serial.print("HTTP Connection Error: ");
    Serial.println(http.errorToString(httpResponseCode));
  }
  
  http.end();
  return success;
}