#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ============ WiFi & API CONFIGURATION ============
const char* ssid = "Sabrinasss";
const char* password = "Sabrinaa";
const char* apiBaseUrl = "https://vendingmachineiot-server.onrender.com/api";

// ============ LCD CONFIGURATION ============
int lcdColumns = 16;
int lcdRows = 2;
LiquidCrystal_I2C lcd(0x27, lcdColumns, lcdRows);

// ============ RFID CONFIGURATION ============
#define SS_PIN    5
#define RST_PIN   4

MFRC522 rfid(SS_PIN, RST_PIN);
MFRC522::MIFARE_Key key;

// ============ BUTTON CONFIGURATION ============
const int BUTTON1_PIN = 14;
const int BUTTON2_PIN = 27;

// ============ SERVO CONFIGURATION ============
Servo tissueServo;      // For Tissue (Slot 1)
Servo sanitaryServo;    // For Sanitary Pad (Slot 2)

const int TISSUE_SERVO_PIN = 12;       // Tissue servo (Button 1)
const int SANITARY_SERVO_PIN = 13;     // Sanitary pad servo (Button 2)

// For continuous rotation servos (MG90 360-degree)
// IMPORTANT: 90 = STOP, 0-89 = Rotate CCW, 91-180 = Rotate CW
const int SERVO_STOP = 90;            // Stop position
const int SERVO_LEFT_SPEED = 0;       // Full speed Counter-Clockwise (LEFT)
const int SERVO_RIGHT_SPEED = 180;    // Full speed Clockwise (RIGHT)

// ============ ULTRASONIC SENSOR CONFIGURATION ============
const int TRIG_PIN = 32;     // Trigger pin for HC-SR04
const int ECHO_PIN = 33;     // Echo pin for HC-SR04
const int DETECTION_DISTANCE = 30; // Distance threshold in cm (30cm or below)

// System variables for ultrasonic control
bool objectDetected = false;
bool servoRunning = false;  // false = servos stopped, true = servos running
unsigned long lastDistanceCheck = 0;
const int CHECK_INTERVAL = 200; // Check every 200ms

// ============ SYSTEM VARIABLES ============
struct User {
  String cardUid;
  String name;
  float balance;
  String userId; // MongoDB _id
};

struct Product {
  String id;     // MongoDB _id
  String name;
  float price;
  int slotNumber;  // 1 for tissue (servo1), 2 for sanitary pad (servo2)
  int stock;
};

#define MAX_PRODUCTS 10
Product products[MAX_PRODUCTS];
int numProducts = 0;

User* currentUser = NULL;
String lastScannedUID = "";

// Button states
int button1State;
int lastButton1State = HIGH;
int button2State;
int lastButton2State = HIGH;
unsigned long lastDebounceTime1 = 0;
unsigned long lastDebounceTime2 = 0;
const unsigned long debounceDelay = 50;
int button1PressCount = 0;
int button2PressCount = 0;

// System states
enum SystemState {
  STATE_IDLE,
  STATE_WAITING_CARD,
  STATE_USER_LOGGED_IN,
  STATE_PRODUCT_SELECTION,
  STATE_DISPENSING,
  STATE_INSUFFICIENT_BALANCE,
  STATE_OUT_OF_STOCK,
  STATE_WIFI_CONNECTING,
  STATE_API_ERROR
};

SystemState currentState = STATE_WIFI_CONNECTING;
bool waitingForCard = false;

// WiFi & API variables
bool wifiConnected = false;
bool productsLoaded = false;
unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 10000;

// Continuous dispensing variables
bool continuousDispensing = false;
int continuousSlotNumber = 0;  // 1 for Tissue, 2 for Sanitary

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  while (!Serial);
  
  Serial.println("\n========================");
  Serial.println("   VENDING MACHINE v10.0");
  Serial.println("   Single Servo Control");
  Serial.println("   Ultrasonic Controlled");
  Serial.println("========================");
  
  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  
  // Initialize buttons
  pinMode(BUTTON1_PIN, INPUT_PULLUP);
  pinMode(BUTTON2_PIN, INPUT_PULLUP);
  
  // Initialize ultrasonic sensor - FIXED PIN CONFIGURATION
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);
  
  // Initialize servos
  tissueServo.attach(TISSUE_SERVO_PIN);
  sanitaryServo.attach(SANITARY_SERVO_PIN);
  
  // Set servos to STOP position (90 degrees)
  tissueServo.write(SERVO_STOP);
  sanitaryServo.write(SERVO_STOP);
  delay(1000);
  
  // Show connecting to WiFi message
  lcd.setCursor(0, 0);
  lcd.print("Connecting to");
  lcd.setCursor(0, 1);
  lcd.print("WiFi...");
  
  // Connect to WiFi
  connectToWiFi();
  
  // Initialize SPI for RFID
  SPI.begin();
  
  // Initialize RFID reader
  rfid.PCD_Init();
  delay(4);
  
  // Prepare RFID key
  for (byte i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF;
  }
  
  // Test ultrasonic sensor
  testUltrasonicSensor();
  
  // Fetch products from API
  fetchProductsFromAPI();
  
  Serial.println("\n=== PIN CONFIGURATION ===");
  Serial.println("LCD I2C: SDA=GPIO21, SCL=GPIO22");
  Serial.println("RFID: SS=GPIO5, RST=GPIO4");
  Serial.println("Buttons: B1=GPIO14, B2=GPIO27");
  Serial.println("Servos: Tissue=GPIO12, Sanitary=GPIO13");
  Serial.println("Ultrasonic: Trig=GPIO25, Echo=GPIO26");
  Serial.println("\n=== ULTRASONIC LOGIC ===");
  Serial.println("Object ≤30cm = SERVO STOPS PERMANENTLY");
  Serial.println("Object >30cm = SERVO STARTS MOVING");
  Serial.println("=== SYSTEM READY ===");
}

// ============ MAIN LOOP ============
void loop() {
  // Check WiFi connection periodically
  if (millis() - lastWiFiCheck > wifiCheckInterval) {
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      productsLoaded = false;
      currentState = STATE_WIFI_CONNECTING;
      connectToWiFi();
    }
    lastWiFiCheck = millis();
  }
  
  // Check ultrasonic sensor and control servo
  controlServoWithUltrasonic();
  
  // Only process RFID and buttons if WiFi is connected
  if (wifiConnected && productsLoaded) {
    // Check for RFID cards
    checkRFID();
    
    // Read and process buttons
    readButtons();
  }
  
  // Update display every second
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 1000) {
    updateStatusDisplay();
    lastUpdate = millis();
  }
}

// ============ ULTRASONIC FUNCTIONS - FIXED ============
float getDistance() {
  // Clear the trig pin
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  
  // Send 10 microsecond pulse
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  // Read the echo pin - increased timeout for better detection
  long duration = pulseIn(ECHO_PIN, HIGH, 60000); // 60ms timeout
  
  // Calculate distance in cm (speed of sound = 343 m/s = 0.0343 cm/µs)
  float distance = duration * 0.0343 / 2;
  
  // Debug output
  Serial.print("[ULTRASONIC] Duration: ");
  Serial.print(duration);
  Serial.print(" µs, Distance: ");
  Serial.print(distance);
  Serial.println(" cm");
  
  // Check for valid readings
  if (duration == 0) {
    // No echo received - sensor not connected or object too far
    Serial.println("[ULTRASONIC] ERROR: No echo received!");
    return -1.0; // Return -1 for error
  }
  
  if (distance > 400 || distance < 2) {
    // Out of range (HC-SR04 range is 2cm to 400cm)
    return 999.0; // Return 999 for out of range
  }
  
  return distance;
}

// ============ TEST ULTRASONIC SENSOR ============
void testUltrasonicSensor() {
  Serial.println("\n=== ULTRASONIC SENSOR TEST ===");
  Serial.println("Testing HC-SR04 sensor...");
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Testing Sensor");
  lcd.setCursor(0, 1);
  lcd.print("Please wait...");
  
  // Take 5 readings to test the sensor
  float totalDistance = 0;
  int validReadings = 0;
  
  for (int i = 0; i < 5; i++) {
    float distance = getDistance();
    
    if (distance > 0 && distance < 400) {
      totalDistance += distance;
      validReadings++;
      Serial.print("Test ");
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(distance);
      Serial.println(" cm");
    } else if (distance == -1) {
      Serial.print("Test ");
      Serial.print(i + 1);
      Serial.println(": NO ECHO - Check wiring!");
    } else {
      Serial.print("Test ");
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(distance);
      Serial.println(" cm (out of range)");
    }
    
    delay(200);
  }
  
  if (validReadings > 0) {
    float avgDistance = totalDistance / validReadings;
    Serial.print("Average distance: ");
    Serial.print(avgDistance);
    Serial.println(" cm");
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Sensor OK!");
    lcd.setCursor(0, 1);
    lcd.print("Avg: ");
    lcd.print(avgDistance, 1);
    lcd.print(" cm");
  } else {
    Serial.println("ERROR: No valid readings! Check ultrasonic sensor wiring.");
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("SENSOR ERROR!");
    lcd.setCursor(0, 1);
    lcd.print("Check Wiring");
  }
  
  delay(2000);
}

// ============ ULTRASONIC CONTROL FUNCTION - IMPROVED ============
void controlServoWithUltrasonic() {
  // Check ultrasonic only every CHECK_INTERVAL milliseconds
  if (millis() - lastDistanceCheck > CHECK_INTERVAL) {
    float distance = getDistance();
    lastDistanceCheck = millis();
    
    // Only apply ultrasonic control when in continuous dispensing mode
    if (continuousDispensing) {
      // Check for valid distance reading (not error -1)
      if (distance >= 0) {
        if (distance <= DETECTION_DISTANCE) {
          // Object is at 30cm or below
          if (servoRunning) {
            // Servo was running, need to STOP it
            servoRunning = false;
            stopCurrentServo();
            Serial.print("[ULTRASONIC] Object detected at ");
            Serial.print(distance);
            Serial.println(" cm - SERVO STOPPED");
            objectDetected = true;
            
            // Update LCD
            lcd.clear();
            lcd.print("Successfully");
            lcd.setCursor(0, 0);
            lcd.print("Dispensed!");
            lcd.setCursor(0, 1);
            
            // ADD THIS LINE - Restart to RFID after 3 seconds
            delay(3000);
            restartToRFIDScanning();  // <- DITO NAG-RESTART
          }
        } else {
          // Object is at 31cm or above (or no object)
          if (!servoRunning && objectDetected) {
            // Object was previously detected and removed, now start servo
            servoRunning = true;
            startCurrentServo();
            Serial.print("[ULTRASONIC] Object removed (");
            Serial.print(distance);
            Serial.println(" cm) - SERVO STARTED");
            objectDetected = false;
            
            // Update LCD
            lcd.clear();
            lcd.setCursor(0, 0);
            if (continuousSlotNumber == 1) {
              lcd.print("Getting Tissue");
            } else {
              lcd.print("Getting Sanitary");
            }
            lcd.setCursor(0, 1);
            lcd.print("Servo: RUNNING");
          } else if (!servoRunning && !objectDetected) {
            // Initial state after reset - start servo if no object
            servoRunning = true;
            startCurrentServo();
            Serial.println("[ULTRASONIC] Initial start - No object detected");
          }
        }
      } else {
        // Distance reading error
        Serial.println("[ULTRASONIC] ERROR: Invalid distance reading!");
      }
    }
  }
}

// ============ SERVO CONTROL FUNCTIONS ============
void stopCurrentServo() {
  if (continuousSlotNumber == 1) {
    tissueServo.write(SERVO_STOP);
    delay(100); // Give servo time to stop
    Serial.println("[SERVO] Tissue servo STOPPED");
  } else if (continuousSlotNumber == 2) {
    sanitaryServo.write(SERVO_STOP);
    delay(100); // Give servo time to stop
    Serial.println("[SERVO] Sanitary servo STOPPED");
  }
}

void startCurrentServo() {
  if (continuousSlotNumber == 1) {
    tissueServo.write(SERVO_RIGHT_SPEED);   // Tissue - RIGHT rotation
    Serial.println("[SERVO] Tissue servo STARTED (RIGHT rotation)");
  } else if (continuousSlotNumber == 2) {
    sanitaryServo.write(SERVO_LEFT_SPEED);  // Sanitary - LEFT rotation
    Serial.println("[SERVO] Sanitary servo STARTED (LEFT rotation)");
  }
}

void stopAllServos() {
  tissueServo.write(SERVO_STOP);
  sanitaryServo.write(SERVO_STOP);
  delay(100);
  servoRunning = false;
  Serial.println("[SERVO] All servos STOPPED");
}

// ============ MODIFIED SERVO FUNCTIONS ============
void dispenseProduct(int slotNumber) {
  if (slotNumber == 1) {
    // TISSUE - Start continuous dispensing
    Serial.println("[SERVO] Starting CONTINUOUS Tissue dispensing...");
    
    // Set continuous dispensing mode for Tissue only
    continuousDispensing = true;
    continuousSlotNumber = 1;
    
    // Check initial distance
    float initialDistance = getDistance();
    
    if (initialDistance >= 0 && initialDistance <= DETECTION_DISTANCE) {
      // Object already detected at startup - KEEP TISSUE SERVO STOPPED
      servoRunning = false;
      objectDetected = true;
      stopCurrentServo();
      Serial.print("[INITIAL] Object detected at ");
      Serial.print(initialDistance);
      Serial.println(" cm - TISSUE SERVO REMAINS STOPPED");
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Hand Detected!");
      lcd.setCursor(0, 1);
      lcd.print("Place >30cm");
    } else {
      // No object detected or error reading - START TISSUE SERVO
      servoRunning = true;
      objectDetected = false;
      startCurrentServo(); // Starts tissue servo only
      Serial.print("[INITIAL] No object detected (");
      Serial.print(initialDistance);
      Serial.println(" cm) - TISSUE SERVO STARTED");
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Getting Tissue");
      lcd.setCursor(0, 1);
      lcd.print("Servo: RUNNING");
    }
    
  } else if (slotNumber == 2) {
    // SANITARY PAD - Start continuous dispensing
    Serial.println("[SERVO] Starting CONTINUOUS Sanitary Pad dispensing...");
    
    // Set continuous dispensing mode for Sanitary only
    continuousDispensing = true;
    continuousSlotNumber = 2;
    
    // Check initial distance
    float initialDistance = getDistance();
    
    if (initialDistance >= 0 && initialDistance <= DETECTION_DISTANCE) {
      // Object already detected at startup - KEEP SANITARY SERVO STOPPED
      servoRunning = false;
      objectDetected = true;
      stopCurrentServo();
      Serial.print("[INITIAL] Object detected at ");
      Serial.print(initialDistance);
      Serial.println(" cm - SANITARY SERVO REMAINS STOPPED");
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Hand Detected!");
      lcd.setCursor(0, 1);
      lcd.print("Place >30cm");
    } else {
      // No object detected or error reading - START SANITARY SERVO
      servoRunning = true;
      objectDetected = false;
      startCurrentServo(); // Starts sanitary servo only
      Serial.print("[INITIAL] No object detected (");
      Serial.print(initialDistance);
      Serial.println(" cm) - SANITARY SERVO STARTED");
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Getting Sanitary");
      lcd.setCursor(0, 1);
      lcd.print("Servo: RUNNING");
    }
  }
}

void endContinuousDispensing() {
  Serial.println("[CONTINUOUS] Ending dispensing...");
  
  stopAllServos();
  continuousDispensing = false;
  continuousSlotNumber = 0;
  servoRunning = false;
  objectDetected = false;
  
  Serial.println("[CONTINUOUS] Dispensing ended");
  
  if (currentUser != NULL) {
    displayPurchaseSuccess(currentUser->balance);
  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Purchase Done!");
    lcd.setCursor(0, 1);
    lcd.print("Thank you!");
    delay(2000);
  }
  
  // Check if user can still purchase
  if (currentUser != NULL) {
    bool canPurchase = false;
    for (int i = 0; i < numProducts; i++) {
      if (currentUser->balance >= products[i].price && products[i].stock > 0) {
        canPurchase = true;
        break;
      }
    }
    
    if (canPurchase) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Tap RFID Card");
      lcd.setCursor(0, 1);
      lcd.print("to Continue");
      
      currentState = STATE_IDLE;
      Serial.println("[Purchase] Asking for RFID again...");
    } else {
      if (currentUser->balance < getMinProductPrice()) {
        displayInsufficientBalance();
      } else {
        displayOutOfStock();
      }
    }
  } else {
    currentState = STATE_IDLE;
    displayIdleScreen();
  }
}

// ============ MODIFIED BUSINESS LOGIC ============
void processPurchase(int productIndex) {
  if (currentUser == NULL) return;
  
  if (productIndex < 0 || productIndex >= numProducts) return;
  
  Product* product = &products[productIndex];
  
  Serial.println("\n=== PURCHASE ATTEMPT ===");
  Serial.print("User: ");
  Serial.println(currentUser->name);
  Serial.print("Product: ");
  Serial.println(product->name);
  Serial.print("Price: PHP ");
  Serial.println(product->price);
  Serial.print("Stock: ");
  Serial.println(product->stock);
  Serial.print("Current Balance: PHP ");
  Serial.println(currentUser->balance);
  
  if (product->stock <= 0) {
    displayOutOfStock();
    Serial.println("[FAILED] Product out of stock!");
    return;
  }
  
  if (currentUser->balance >= product->price) {
    currentState = STATE_DISPENSING;
    
    // Create order via API
    bool orderSuccess = createOrder(currentUser->userId, product->id);
    
    if (orderSuccess) {
      // Start continuous dispensing based on slot number
      dispenseProduct(product->slotNumber);
      
      Serial.println("[SUCCESS] Purchase approved! Starting continuous dispensing...");
    } else {
      Serial.println("[FAILED] API order creation failed!");
      currentState = STATE_USER_LOGGED_IN;
      displayWelcomeAndCheckBalance();
    }
  } else {
    displayInsufficientBalance();
    Serial.println("[FAILED] Insufficient balance!");
  }
  
  Serial.println("=======================\n");
}

// ============ MODIFIED BUTTON FUNCTIONS ============
void button1Pressed() {
  button1PressCount++;
  Serial.print("\n[Button1] Pressed! Total: ");
  Serial.println(button1PressCount);
  
  switch(currentState) {
    case STATE_IDLE:
      currentState = STATE_WAITING_CARD;
      waitingForCard = true;
      displayScanCardScreen();
      Serial.println("[BUTTON] Ready to scan card");
      break;
      
    case STATE_USER_LOGGED_IN:
      displayWelcomeAndCheckBalance();
      break;
      
    case STATE_PRODUCT_SELECTION:
      if (currentUser != NULL) {
        processPurchase(0); // First product (Tissue - index 0)
      }
      break;
      
    case STATE_WAITING_CARD:
      waitingForCard = false;
      currentState = STATE_IDLE;
      displayIdleScreen();
      Serial.println("[BUTTON] Cancelled scan");
      break;
      
    case STATE_DISPENSING:
      // Button 1 during dispensing ends the continuous dispensing
      if (continuousDispensing) {
        endContinuousDispensing();
        Serial.println("[BUTTON] Ended continuous dispensing");
      }
      break;
      
    case STATE_INSUFFICIENT_BALANCE:
    case STATE_OUT_OF_STOCK:
      break;
  }
}

void button2Pressed() {
  button2PressCount++;
  Serial.print("\n[Button2] Pressed! Total: ");
  Serial.println(button2PressCount);
  
  switch(currentState) {
    case STATE_IDLE:
      currentState = STATE_WAITING_CARD;
      waitingForCard = true;
      displayScanCardScreen();
      Serial.println("[BUTTON] Ready to scan card");
      break;
      
    case STATE_USER_LOGGED_IN:
      logoutUser();
      break;
      
    case STATE_PRODUCT_SELECTION:
      if (currentUser != NULL && numProducts > 1) {
        processPurchase(1); // Second product (Sanitary Pad - index 1)
      }
      break;
      
    case STATE_WAITING_CARD:
      waitingForCard = false;
      currentState = STATE_IDLE;
      displayIdleScreen();
      Serial.println("[BUTTON] Cancelled scan");
      break;
      
    case STATE_DISPENSING:
      // Button 2 during dispensing ends the continuous dispensing
      if (continuousDispensing) {
        endContinuousDispensing();
        Serial.println("[BUTTON] Ended continuous dispensing");
      }
      break;
      
    case STATE_INSUFFICIENT_BALANCE:
    case STATE_OUT_OF_STOCK:
      currentState = STATE_IDLE;
      displayIdleScreen();
      break;
  }
}

// ============ DISPLAY FUNCTIONS ============
void displayIdleScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Tap RFID Card");
  lcd.setCursor(0, 1);
  lcd.print("B1/B2: Scan");
  
  Serial.println("[Display] Idle - RFID ONLY");
}

void displayScanCardScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Scan RFID Card");
  lcd.setCursor(0, 1);
  lcd.print("B1: Cancel");
  
  Serial.println("[Display] Waiting for RFID card...");
}

void displayWelcomeAndCheckBalance() {
  if (currentUser == NULL) return;
  
  lcd.clear();
  lcd.setCursor(0, 0);
  
  String displayName = currentUser->name;
  if (displayName.length() > 8) {
    displayName = displayName.substring(0, 8);
  }
  
  lcd.print("Hi, ");
  lcd.print(displayName);
  lcd.print("!");
  
  lcd.setCursor(0, 1);
  lcd.print("Checking balance");
  
  delay(1500);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Balance: PHP");
  lcd.setCursor(0, 1);
  lcd.print(currentUser->balance, 1);
  
  delay(1500);
  
  // Check if user can purchase anything
  bool canPurchase = false;
  for (int i = 0; i < numProducts; i++) {
    if (currentUser->balance >= products[i].price && products[i].stock > 0) {
      canPurchase = true;
      break;
    }
  }
  
  if (canPurchase) {
    currentState = STATE_PRODUCT_SELECTION;
    displayProductSelection();
  } else {
    bool hasBalance = false;
    bool hasStock = false;
    
    for (int i = 0; i < numProducts; i++) {
      if (currentUser->balance >= products[i].price) hasBalance = true;
      if (products[i].stock > 0) hasStock = true;
    }
    
    if (!hasBalance) {
      currentState = STATE_INSUFFICIENT_BALANCE;
      displayInsufficientBalance();
    } else if (!hasStock) {
      currentState = STATE_OUT_OF_STOCK;
      displayOutOfStock();
    }
  }
}

void displayProductSelection() {
  if (currentUser == NULL) return;
  
  lcd.clear();
  
  if (numProducts >= 1) {
    lcd.setCursor(0, 0);
    String line1 = "1:";
    if (products[0].name.length() > 6) {
      line1 += products[0].name.substring(0, 6);
    } else {
      line1 += products[0].name;
    }
    line1 += " PHP";
    line1 += String(products[0].price, 0);
    lcd.print(line1);
  }
  
  if (numProducts >= 2) {
    lcd.setCursor(0, 1);
    String line2 = "2:";
    if (products[1].name.length() > 6) {
      line2 += products[1].name.substring(0, 6);
    } else {
      line2 += products[1].name;
    }
    line2 += " PHP";
    line2 += String(products[1].price, 0);
    lcd.print(line2);
  }
  
  Serial.println("[Display] Product Selection");
}

void displayPurchaseSuccess(float newBalance) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Purchase Done!");
  lcd.setCursor(0, 1);
  lcd.print("Bal: PHP");
  lcd.print(newBalance, 1);
  
  delay(2000);
  
  stopAllServos();
  
  Serial.println("[Purchase] Dispensing completed successfully!");
}

void displayInsufficientBalance() {
  currentState = STATE_INSUFFICIENT_BALANCE;
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Insufficient");
  lcd.setCursor(0, 1);
  
  if (currentUser != NULL) {
    lcd.print("Bal: PHP");
    lcd.print(currentUser->balance, 1);
  } else {
    lcd.print("Balance!");
  }
  
  Serial.println("[Display] ERROR: Insufficient balance");
  
  delay(2000);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Scan RFID Card");
  lcd.setCursor(0, 1);
  lcd.print("to Continue");
}

void displayOutOfStock() {
  currentState = STATE_OUT_OF_STOCK;
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Out of Stock!");
  lcd.setCursor(0, 1);
  lcd.print("Try again later");
  
  Serial.println("[Display] ERROR: Out of stock");
  
  delay(2000);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Scan RFID Card");
  lcd.setCursor(0, 1);
  lcd.print("to Continue");
}

// ============ UPDATE STATUS DISPLAY WITH ULTRASONIC INFO ============
void updateStatusDisplay() {
  Serial.println("---------------");
  Serial.print("State: ");
  switch(currentState) {
    case STATE_IDLE: 
      Serial.println("IDLE"); 
      break;
    case STATE_WAITING_CARD: 
      Serial.println("WAITING FOR CARD"); 
      break;
    case STATE_USER_LOGGED_IN: 
      Serial.println("USER LOGGED IN"); 
      break;
    case STATE_PRODUCT_SELECTION: 
      Serial.println("PRODUCT SELECTION"); 
      break;
    case STATE_DISPENSING: 
      Serial.println("DISPENSING"); 
      break;
    case STATE_INSUFFICIENT_BALANCE: 
      Serial.println("INSUFFICIENT BALANCE"); 
      break;
    case STATE_OUT_OF_STOCK:
      Serial.println("OUT OF STOCK");
      break;
    case STATE_WIFI_CONNECTING:
      Serial.println("WIFI CONNECTING");
      break;
    case STATE_API_ERROR:
      Serial.println("API ERROR");
      break;
  }
  
  if (currentUser != NULL) {
    Serial.print("User: ");
    Serial.print(currentUser->name);
    Serial.print(" | Card: ");
    Serial.print(currentUser->cardUid);
    Serial.print(" | Balance: PHP ");
    Serial.println(currentUser->balance);
  } else {
    Serial.println("No user logged in");
  }
  
  Serial.print("WiFi: ");
  Serial.println(wifiConnected ? "CONNECTED" : "DISCONNECTED");
  if (wifiConnected) {
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }
  
  Serial.print("Products Loaded: ");
  Serial.print(numProducts);
  Serial.println(" items");
  for (int i = 0; i < numProducts; i++) {
    Serial.print("  ");
    Serial.print(i+1);
    Serial.print(". ");
    Serial.print(products[i].name);
    Serial.print(" (Slot ");
    Serial.print(products[i].slotNumber);
    Serial.print(") - PHP ");
    Serial.print(products[i].price);
    Serial.print(" - Stock: ");
    Serial.println(products[i].stock);
  }
  
  // Ultrasonic status - show raw reading
  float currentDistance = getDistance();
  Serial.print("Ultrasonic Reading: ");
  Serial.print(currentDistance);
  Serial.println(" cm");
  
  Serial.print("Object Detected (<30cm): ");
  Serial.println(objectDetected ? "YES" : "NO");
  Serial.print("Servo Running: ");
  Serial.println(servoRunning ? "YES" : "NO");
  
  Serial.print("Continuous Dispensing: ");
  Serial.println(continuousDispensing ? "ACTIVE" : "INACTIVE");
  if (continuousDispensing) {
    Serial.print("  Active Slot: ");
    Serial.println(continuousSlotNumber);
    Serial.print("  Active Servo: ");
    Serial.println(continuousSlotNumber == 1 ? "TISSUE" : "SANITARY");
  }
  
  Serial.print("Last UID: ");
  Serial.println(lastScannedUID);
  Serial.print("Button Counts: B1=");
  Serial.print(button1PressCount);
  Serial.print(", B2=");
  Serial.println(button2PressCount);
  
  Serial.println("---------------");
}

// ============ RETAINED FUNCTIONS ============
void connectToWiFi() {
  Serial.print("\n[WiFi] Connecting to: ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
    
    // Update LCD
    lcd.setCursor(0, 1);
    lcd.print("WiFi");
    for (int i = 0; i < (attempts % 4); i++) {
      lcd.print(".");
    }
    for (int i = (attempts % 4); i < 4; i++) {
      lcd.print(" ");
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    currentState = STATE_IDLE;
    
    Serial.println("\n[WiFi] CONNECTED!");
    Serial.print("[WiFi] IP Address: ");
    Serial.println(WiFi.localIP());
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Connected!");
    lcd.setCursor(0, 1);
    lcd.print("IP: ");
    lcd.print(WiFi.localIP());
    
    delay(2000);
    displayIdleScreen();
  } else {
    Serial.println("\n[WiFi] FAILED to connect!");
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Failed!");
    lcd.setCursor(0, 1);
    lcd.print("Check SSID/PW");
    
    delay(2000);
    currentState = STATE_WIFI_CONNECTING;
    displayIdleScreen();
  }
}

void fetchProductsFromAPI() {
  if (!wifiConnected) {
    Serial.println("[API] Cannot fetch products - WiFi not connected");
    return;
  }
  
  Serial.println("\n[API] Fetching products from server...");
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Loading");
  lcd.setCursor(0, 1);
  lcd.print("products...");
  
  HTTPClient http;
  String url = String(apiBaseUrl) + "/products";
  
  Serial.print("[API] URL: ");
  Serial.println(url);
  
  http.begin(url);
  int httpCode = http.GET();
  
  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      String response = http.getString();
      Serial.println("[API] Products fetched successfully!");
      
      // Parse JSON response
      StaticJsonDocument<2048> doc;
      DeserializationError error = deserializeJson(doc, response);
      
      if (error) {
        Serial.print("[API] JSON parsing error: ");
        Serial.println(error.c_str());
        http.end();
        return;
      }
      
      // Check response structure
      if (doc.containsKey("success") && doc["success"] == true && 
          doc.containsKey("data")) {
        
        JsonArray productsArray = doc["data"].as<JsonArray>();
        numProducts = 0;
        
        for (JsonObject product : productsArray) {
          if (numProducts >= MAX_PRODUCTS) break;
          
          products[numProducts].id = product["_id"].as<String>();
          products[numProducts].name = product["name"].as<String>();
          products[numProducts].price = product["price"].as<float>();
          products[numProducts].slotNumber = product["slotNumber"].as<int>();
          products[numProducts].stock = product["stock"].as<int>();
          
          Serial.print("[API] Product ");
          Serial.print(numProducts + 1);
          Serial.print(": ");
          Serial.print(products[numProducts].name);
          Serial.print(" (Slot ");
          Serial.print(products[numProducts].slotNumber);
          Serial.print(") - PHP ");
          Serial.print(products[numProducts].price);
          Serial.print(" - Stock: ");
          Serial.println(products[numProducts].stock);
          
          numProducts++;
        }
        
        productsLoaded = true;
        Serial.print("[API] Loaded ");
        Serial.print(numProducts);
        Serial.println(" products");
        
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Products loaded!");
        lcd.setCursor(0, 1);
        lcd.print(numProducts);
        lcd.print(" items ready");
        
        delay(2000);
        displayIdleScreen();
        
      } else {
        Serial.println("[API] Invalid response format");
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Invalid API");
        lcd.setCursor(0, 1);
        lcd.print("response");
        delay(2000);
      }
    } else {
      Serial.print("[API] Error fetching products. Code: ");
      Serial.println(httpCode);
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("API Error:");
      lcd.setCursor(0, 1);
      lcd.print("Code ");
      lcd.print(httpCode);
      delay(2000);
    }
  } else {
    Serial.print("[API] Failed to connect: ");
    Serial.println(http.errorToString(httpCode).c_str());
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("API Connection");
    lcd.setCursor(0, 1);
    lcd.print("Failed!");
    delay(2000);
    currentState = STATE_API_ERROR;
  }
  
  http.end();
}

User* findUserByCardUID(String cardUid) {
  if (!wifiConnected) {
    Serial.println("[API] Cannot find user - WiFi not connected");
    return NULL;
  }
  
  Serial.println("\n[API] Finding user by RFID...");
  Serial.print("[API] Card UID: ");
  Serial.println(cardUid);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Checking user");
  lcd.setCursor(0, 1);
  lcd.print("in database...");
  
  HTTPClient http;
  String url = String(apiBaseUrl) + "/users";
  
  http.begin(url);
  int httpCode = http.GET();
  
  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      String response = http.getString();
      Serial.println("[API] Users fetched successfully!");
      
      // Parse JSON response
      StaticJsonDocument<1024> doc;
      DeserializationError error = deserializeJson(doc, response);
      
      if (error) {
        Serial.print("[API] JSON parsing error: ");
        Serial.println(error.c_str());
        http.end();
        return NULL;
      }
      
      // Check response structure
      if (doc.containsKey("success") && doc["success"] == true && 
          doc.containsKey("data")) {
        
        JsonArray usersArray = doc["data"].as<JsonArray>();
        
        for (JsonObject user : usersArray) {
          if (user.containsKey("rfid_tag")) {
            String apiCardUid = user["rfid_tag"].as<String>();
            if (apiCardUid.equals(cardUid)) {
              // Found user
              static User foundUser;
              foundUser.cardUid = apiCardUid;
              foundUser.name = user["name"].as<String>();
              foundUser.balance = user["credits"].as<float>();
              foundUser.userId = user["_id"].as<String>();
              
              Serial.print("[API] Found user: ");
              Serial.println(foundUser.name);
              Serial.print("[API] User ID: ");
              Serial.println(foundUser.userId);
              Serial.print("[API] Balance: PHP ");
              Serial.println(foundUser.balance);
              
              http.end();
              return &foundUser;
            }
          }
        }
        
        Serial.println("[API] User not found in database");
      } else {
        Serial.println("[API] Invalid response format");
      }
    } else {
      Serial.print("[API] Error fetching users. Code: ");
      Serial.println(httpCode);
    }
  } else {
    Serial.print("[API] Failed to connect: ");
    Serial.println(http.errorToString(httpCode).c_str());
  }
  
  http.end();
  return NULL;
}

bool createOrder(String userId, String productId) {
  if (!wifiConnected) {
    Serial.println("[API] Cannot create order - WiFi not connected");
    return false;
  }
  
  Serial.println("\n[API] Creating order...");
  Serial.print("User ID: ");
  Serial.println(userId);
  Serial.print("Product ID: ");
  Serial.println(productId);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Creating order");
  lcd.setCursor(0, 1);
  lcd.print("Please wait...");
  
  HTTPClient http;
  String url = String(apiBaseUrl) + "/orders";
  
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  
  // Create JSON payload
  StaticJsonDocument<256> doc;
  doc["userId"] = userId;
  doc["productId"] = productId;
  
  String requestBody;
  serializeJson(doc, requestBody);
  
  Serial.print("[API] Request body: ");
  Serial.println(requestBody);
  
  int httpCode = http.POST(requestBody);
  
  bool success = false;
  
  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
      String response = http.getString();
      Serial.print("[API] Order created successfully! Response: ");
      Serial.println(response);
      
      // Parse response to get updated user info
      StaticJsonDocument<512> responseDoc;
      DeserializationError error = deserializeJson(responseDoc, response);
      
      if (!error) {
        if (responseDoc.containsKey("success") && responseDoc["success"] == true &&
            responseDoc.containsKey("data")) {
          
          JsonObject data = responseDoc["data"].as<JsonObject>();
          
          if (currentUser != NULL && data.containsKey("user")) {
            JsonObject user = data["user"];
            if (user.containsKey("credits")) {
              currentUser->balance = user["credits"].as<float>();
              Serial.print("[API] Updated balance: PHP ");
              Serial.println(currentUser->balance);
            }
          }
          
          // Update product stock
          if (data.containsKey("product")) {
            JsonObject product = data["product"];
            String updatedProductId = product["_id"].as<String>();
            int updatedStock = product["stock"].as<int>();
            
            // Find and update product stock
            for (int i = 0; i < numProducts; i++) {
              if (products[i].id == updatedProductId) {
                products[i].stock = updatedStock;
                Serial.print("[API] Updated product stock: ");
                Serial.print(products[i].name);
                Serial.print(" - Stock: ");
                Serial.println(products[i].stock);
                break;
              }
            }
          }
        }
      }
      
      success = true;
    } else {
      Serial.print("[API] Error creating order. Code: ");
      Serial.println(httpCode);
      String response = http.getString();
      Serial.print("[API] Response: ");
      Serial.println(response);
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Order Failed!");
      lcd.setCursor(0, 1);
      lcd.print("Code: ");
      lcd.print(httpCode);
      delay(2000);
    }
  } else {
    Serial.print("[API] Failed to connect: ");
    Serial.println(http.errorToString(httpCode).c_str());
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("API Connection");
    lcd.setCursor(0, 1);
    lcd.print("Failed!");
    delay(2000);
  }
  
  http.end();
  return success;
}

void checkRFID() {
  if (!rfid.PICC_IsNewCardPresent()) {
    return;
  }
  
  if (!rfid.PICC_ReadCardSerial()) {
    return;
  }
  
  String cardUid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    cardUid += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
    cardUid += String(rfid.uid.uidByte[i], HEX);
  }
  cardUid.toUpperCase();
  
  lastScannedUID = cardUid;
  
  Serial.println("\n[RFID] Card Detected!");
  Serial.print("[RFID] UID: ");
  Serial.println(cardUid);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Card Detected:");
  lcd.setCursor(0, 1);
  lcd.print(cardUid.substring(0, 16));
  
  delay(1000);
  
  switch(currentState) {
    case STATE_IDLE:
    case STATE_WAITING_CARD:
      processRFIDLogin(cardUid);
      break;
      
    case STATE_USER_LOGGED_IN:
    case STATE_PRODUCT_SELECTION:
    case STATE_INSUFFICIENT_BALANCE:
    case STATE_OUT_OF_STOCK:
      checkUserBalance(cardUid);
      break;
      
    case STATE_DISPENSING:
      break;
  }
  
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void processRFIDLogin(String cardUid) {
  // Find user from API
  User* foundUser = findUserByCardUID(cardUid);
  
  if (foundUser != NULL) {
    currentUser = foundUser;
    waitingForCard = false;
    
    logLogin();
    
    // Check if user can purchase anything
    bool canPurchase = false;
    for (int i = 0; i < numProducts; i++) {
      if (currentUser->balance >= products[i].price && products[i].stock > 0) {
        canPurchase = true;
        break;
      }
    }
    
    if (canPurchase) {
      currentState = STATE_USER_LOGGED_IN;
      displayWelcomeAndCheckBalance();
    } else {
      // Check why user can't purchase
      bool hasBalance = false;
      bool hasStock = false;
      
      for (int i = 0; i < numProducts; i++) {
        if (currentUser->balance >= products[i].price) hasBalance = true;
        if (products[i].stock > 0) hasStock = true;
      }
      
      if (!hasBalance) {
        currentState = STATE_INSUFFICIENT_BALANCE;
        displayInsufficientBalance();
      } else if (!hasStock) {
        currentState = STATE_OUT_OF_STOCK;
        displayOutOfStock();
      }
    }
    
    Serial.println("[RFID] BEEP! Login successful!");
  } else {
    // Unknown card
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Unknown Card!");
    lcd.setCursor(0, 1);
    lcd.print("Not Registered");
    
    Serial.println("[RFID] ERROR: Unknown card!");
    
    delay(2000);
    displayIdleScreen();
  }
}

void checkUserBalance(String cardUid) {
  // Check if this card belongs to current user
  if (currentUser != NULL && cardUid.equals(currentUser->cardUid)) {
    // Same user - check if can purchase
    bool canPurchase = false;
    for (int i = 0; i < numProducts; i++) {
      if (currentUser->balance >= products[i].price && products[i].stock > 0) {
        canPurchase = true;
        break;
      }
    }
    
    if (canPurchase) {
      currentState = STATE_USER_LOGGED_IN;
      displayWelcomeAndCheckBalance();
    } else {
      bool hasBalance = false;
      bool hasStock = false;
      
      for (int i = 0; i < numProducts; i++) {
        if (currentUser->balance >= products[i].price) hasBalance = true;
        if (products[i].stock > 0) hasStock = true;
      }
      
      if (!hasBalance) {
        currentState = STATE_INSUFFICIENT_BALANCE;
        displayInsufficientBalance();
      } else if (!hasStock) {
        currentState = STATE_OUT_OF_STOCK;
        displayOutOfStock();
      }
    }
  } else {
    // Different user - treat as new login
    processRFIDLogin(cardUid);
  }
}

void readButtons() {
  int reading1 = digitalRead(BUTTON1_PIN);
  int reading2 = digitalRead(BUTTON2_PIN);
  
  // Button 1 Debouncing
  if (reading1 != lastButton1State) {
    lastDebounceTime1 = millis();
  }
  if ((millis() - lastDebounceTime1) > debounceDelay) {
    if (reading1 != button1State) {
      button1State = reading1;
      if (button1State == LOW) {
        button1Pressed();
      }
    }
  }
  
  // Button 2 Debouncing
  if (reading2 != lastButton2State) {
    lastDebounceTime2 = millis();
  }
  if ((millis() - lastDebounceTime2) > debounceDelay) {
    if (reading2 != button2State) {
      button2State = reading2;
      if (button2State == LOW) {
        button2Pressed();
      }
    }
  }
  
  lastButton1State = reading1;
  lastButton2State = reading2;
}

float getMinProductPrice() {
  if (numProducts == 0) return 999999.0;
  
  float minPrice = products[0].price;
  for (int i = 1; i < numProducts; i++) {
    if (products[i].price < minPrice) {
      minPrice = products[i].price;
    }
  }
  return minPrice;
}

void logoutUser() {
  if (currentUser != NULL) {
    Serial.print("\n[Logout] ");
    Serial.print(currentUser->name);
    Serial.print(" logged out. Final balance: PHP ");
    Serial.println(currentUser->balance);
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Thank you");
    lcd.setCursor(0, 1);
    lcd.print(currentUser->name);
    
    delay(1500);
  }
  
  currentUser = NULL;
  currentState = STATE_IDLE;
  lastScannedUID = "";
  
  stopAllServos();
  
  displayIdleScreen();
}

void logLogin() {
  if (currentUser == NULL) return;
  
  Serial.println("\n=== USER LOGGED IN ===");
  Serial.print("Name: ");
  Serial.println(currentUser->name);
  Serial.print("Card UID: ");
  Serial.println(currentUser->cardUid);
  Serial.print("User ID: ");
  Serial.println(currentUser->userId);
  Serial.print("Balance: PHP ");
  Serial.println(currentUser->balance);
  Serial.println("=====================\n");
}

// ============ NEW RESTART FUNCTION ============
void restartToRFIDScanning() {
  Serial.println("\n=========================================");
  Serial.println("OBJECT DETECTED - RESTARTING SYSTEM");
  Serial.println("Back to RFID Scanning...");
  Serial.println("=========================================\n");
  
  // Stop all servos
  stopAllServos();
  
  // End continuous dispensing
  continuousDispensing = false;
  continuousSlotNumber = 0;
  servoRunning = false;
  objectDetected = false;
  
  // Reset current user
  if (currentUser != NULL) {
    Serial.print("[RESTART] Logging out user: ");
    Serial.println(currentUser->name);
    currentUser = NULL;
  }
  
  // Set state back to idle for RFID scanning
  currentState = STATE_IDLE;
  waitingForCard = false;
  
  // Display restart message
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Thank you");
  delay(2000);
  
  // Show RFID scanning screen
  displayIdleScreen();
  
  Serial.println("[RESTART] System ready for next RFID scan.");
  Serial.println("=========================================\n");
}