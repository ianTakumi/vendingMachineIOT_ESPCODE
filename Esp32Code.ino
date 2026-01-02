#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>

// ============ LCD CONFIGURATION ============
int lcdColumns = 16;
int lcdRows = 2;
LiquidCrystal_I2C lcd(0x27, lcdColumns, lcdRows);  // LCD on I2C: SDA=21, SCL=22

// ============ RFID CONFIGURATION ============
#define SS_PIN    5    // GPIO 5 for SDA/SS
#define RST_PIN   4    // GPIO 4 (21 is used by LCD I2C)

MFRC522 rfid(SS_PIN, RST_PIN);
MFRC522::MIFARE_Key key;

// ============ BUTTON CONFIGURATION ============
const int BUTTON1_PIN = 14;  // GPIO 14
const int BUTTON2_PIN = 27;  // GPIO 27

// ============ SYSTEM VARIABLES ============
struct User {
  String cardUid;    // RFID Card UID
  String name;
  float balance;
  int id;
};

// RFID Card Database with YOUR ACTUAL CARDS
#define NUM_USERS 2
User users[NUM_USERS] = {
  {"3217A7AB", "Sabrina", 150.00, 1},    // Your TAG card
  {"23293339", "Althea", 200.50, 2}      // Your CARD card
};

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
  STATE_INSUFFICIENT_BALANCE
};

SystemState currentState = STATE_IDLE;
bool waitingForCard = false;

// Product prices
float product1Price = 20.00;  // Tissue - PHP 20
float product2Price = 30.00;  // Alcohol Spray - PHP 30

// Minimum balance to show products
const float MIN_BALANCE = 20.00;

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  while (!Serial);
  
  Serial.println("\n========================");
  Serial.println("   VENDING MACHINE v4.0");
  Serial.println("  RFID ONLY - NO GUEST");
  Serial.println("========================");
  
  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  
  // Initialize buttons
  pinMode(BUTTON1_PIN, INPUT_PULLUP);
  pinMode(BUTTON2_PIN, INPUT_PULLUP);
  
  // Initialize SPI for RFID
  SPI.begin();
  
  // Initialize RFID reader
  rfid.PCD_Init();
  delay(4);
  
  // Prepare RFID key
  for (byte i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF;
  }
  
  // Show welcome message
  lcd.setCursor(0, 0);
  lcd.print("RFID Vending");
  lcd.setCursor(0, 1);
  lcd.print("Tissue & Alcohol");
  
  delay(2000);
  
  // Show system info in Serial
  Serial.println("\n=== PIN CONFIGURATION ===");
  Serial.println("LCD I2C: SDA=GPIO21, SCL=GPIO22");
  Serial.println("RFID: SS=GPIO5, RST=GPIO4");
  Serial.println("Buttons: B1=GPIO14, B2=GPIO27");
  Serial.println("=========================\n");
  
  Serial.println("=== USER DATABASE ===");
  Serial.println("1. Sabrina  - UID: 3217A7AB - PHP 150.00");
  Serial.println("2. Althea   - UID: 23293339 - PHP 200.50");
  Serial.println("==============================\n");
  
  Serial.println("=== PRODUCTS ===");
  Serial.println("1. Tissue        - PHP 20.00");
  Serial.println("2. Alcohol Spray - PHP 30.00");
  Serial.println("==================\n");
  
  // Start in idle state
  displayIdleScreen();
}

// ============ MAIN LOOP ============
void loop() {
  // 1. Check for RFID cards
  checkRFID();
  
  // 2. Read and process buttons
  readButtons();
  
  // 3. Update display every second
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 1000) {
    updateStatusDisplay();
    lastUpdate = millis();
  }
}

// ============ RFID FUNCTIONS ============
void checkRFID() {
  // Look for new cards
  if (!rfid.PICC_IsNewCardPresent()) {
    return;
  }
  
  // Verify if the NUID has been read
  if (!rfid.PICC_ReadCardSerial()) {
    return;
  }
  
  // Get card UID
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
  
  // Show card detection on LCD
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Card Detected:");
  lcd.setCursor(0, 1);
  lcd.print(cardUid.substring(0, 16));
  
  delay(1000);
  
  // Process the card based on current state
  switch(currentState) {
    case STATE_IDLE:
    case STATE_WAITING_CARD:
      processRFIDLogin(cardUid);
      break;
      
    case STATE_USER_LOGGED_IN:
    case STATE_PRODUCT_SELECTION:
    case STATE_INSUFFICIENT_BALANCE:
      // If in any state, card tap goes to check balance
      checkUserBalance(cardUid);
      break;
      
    case STATE_DISPENSING:
      // Ignore during dispensing
      break;
  }
  
  // Halt PICC
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void processRFIDLogin(String cardUid) {
  // Search for user with this card UID
  for (int i = 0; i < NUM_USERS; i++) {
    if (users[i].cardUid.equals(cardUid)) {
      // User found!
      currentUser = &users[i];
      waitingForCard = false;
      
      logLogin();
      
      // Check balance and decide next state
      if (currentUser->balance >= MIN_BALANCE) {
        // Balance is enough for at least Tissue
        currentState = STATE_USER_LOGGED_IN;
        displayWelcomeAndCheckBalance();
      } else {
        // Insufficient balance
        currentState = STATE_INSUFFICIENT_BALANCE;
        displayInsufficientBalance();
      }
      
      Serial.println("[RFID] BEEP! Login successful!");
      return;
    }
  }
  
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

void checkUserBalance(String cardUid) {
  // Check if this card belongs to current user
  if (currentUser != NULL && cardUid.equals(currentUser->cardUid)) {
    // Same user - check balance
    if (currentUser->balance >= MIN_BALANCE) {
      // Balance is enough
      currentState = STATE_USER_LOGGED_IN;
      displayWelcomeAndCheckBalance();
    } else {
      // Still insufficient
      currentState = STATE_INSUFFICIENT_BALANCE;
      displayInsufficientBalance();
    }
  } else {
    // Different user - treat as new login
    processRFIDLogin(cardUid);
  }
}

// ============ BUTTON FUNCTIONS ============
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

void button1Pressed() {
  button1PressCount++;
  Serial.print("\n[Button1] Pressed! Total: ");
  Serial.println(button1PressCount);
  
  switch(currentState) {
    case STATE_IDLE:
      // NO GUEST MODE - Start RFID scan mode
      currentState = STATE_WAITING_CARD;
      waitingForCard = true;
      displayScanCardScreen();
      Serial.println("[BUTTON] Ready to scan card");
      break;
      
    case STATE_USER_LOGGED_IN:
      // Show balance and auto-go to product selection
      displayWelcomeAndCheckBalance();
      break;
      
    case STATE_PRODUCT_SELECTION:
      // Buy Product 1 (Tissue)
      if (currentUser != NULL) {
        processPurchase(1);
      }
      break;
      
    case STATE_WAITING_CARD:
      // Cancel card scan
      waitingForCard = false;
      currentState = STATE_IDLE;
      displayIdleScreen();
      Serial.println("[BUTTON] Cancelled scan");
      break;
      
    case STATE_DISPENSING:
    case STATE_INSUFFICIENT_BALANCE:
      // Ignore during these states
      break;
  }
}

void button2Pressed() {
  button2PressCount++;
  Serial.print("\n[Button2] Pressed! Total: ");
  Serial.println(button2PressCount);
  
  switch(currentState) {
    case STATE_IDLE:
      // Start RFID card scan mode
      currentState = STATE_WAITING_CARD;
      waitingForCard = true;
      displayScanCardScreen();
      Serial.println("[BUTTON] Ready to scan card");
      break;
      
    case STATE_USER_LOGGED_IN:
      // Logout
      logoutUser();
      break;
      
    case STATE_PRODUCT_SELECTION:
      // Buy Product 2 (Alcohol Spray)
      if (currentUser != NULL) {
        processPurchase(2);
      }
      break;
      
    case STATE_WAITING_CARD:
      // Cancel scan and go back
      waitingForCard = false;
      currentState = STATE_IDLE;
      displayIdleScreen();
      Serial.println("[BUTTON] Cancelled scan");
      break;
      
    case STATE_INSUFFICIENT_BALANCE:
      // Go back to idle from insufficient balance
      currentState = STATE_IDLE;
      displayIdleScreen();
      break;
      
    case STATE_DISPENSING:
      // Ignore during dispensing
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
  
  // Show welcome with name
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
  
  // Show balance
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Balance: PHP");
  lcd.setCursor(0, 1);
  lcd.print(currentUser->balance, 1);
  
  delay(1500);
  
  // AUTOMATIC: Check if balance is sufficient
  if (currentUser->balance >= MIN_BALANCE) {
    // Balance is enough - go directly to product selection
    currentState = STATE_PRODUCT_SELECTION;
    displayProductSelection();
  } else {
    // Insufficient balance
    currentState = STATE_INSUFFICIENT_BALANCE;
    displayInsufficientBalance();
  }
}

void displayProductSelection() {
  if (currentUser == NULL) return;
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("1:Tissue PHP20");
  
  lcd.setCursor(0, 1);
  lcd.print("2:Alcohol PHP30");
  
  Serial.println("[Display] Product Selection");
}

void displayDispensingProduct(int product) {
  lcd.clear();
  lcd.setCursor(0, 0);
  
  if (product == 1) {
    lcd.print("Dispensing...");
    lcd.setCursor(0, 1);
    lcd.print("Tissue");
  } else {
    lcd.print("Dispensing...");
    lcd.setCursor(0, 1);
    lcd.print("Alcohol Spray");
  }
  
  // Simulate dispensing time
  for (int i = 0; i < 3; i++) {
    lcd.print(".");
    delay(500);
  }
}

void displayPurchaseSuccess(float newBalance) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Purchase Done!");
  lcd.setCursor(0, 1);
  lcd.print("Bal: PHP");
  lcd.print(newBalance, 1);
  
  delay(2000);
  
  // Check if balance is still enough for more purchases
  if (newBalance >= MIN_BALANCE) {
    // Ask for RFID again
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Tap RFID Card");
    lcd.setCursor(0, 1);
    lcd.print("to Continue");
    
    currentState = STATE_IDLE;
    Serial.println("[Purchase] Asking for RFID again...");
  } else {
    // Insufficient balance after purchase
    displayInsufficientBalance();
  }
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
  
  // Ask to scan RFID again
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Scan RFID Card");
  lcd.setCursor(0, 1);
  lcd.print("to Continue");
}

// ============ BUSINESS LOGIC ============
void processPurchase(int productNum) {
  if (currentUser == NULL) return;
  
  float price = (productNum == 1) ? product1Price : product2Price;
  String productName = (productNum == 1) ? "Tissue" : "Alcohol Spray";
  
  Serial.println("\n=== PURCHASE ATTEMPT ===");
  Serial.print("User: ");
  Serial.println(currentUser->name);
  Serial.print("Product: ");
  Serial.println(productName);
  Serial.print("Price: PHP ");
  Serial.println(price);
  Serial.print("Current Balance: PHP ");
  Serial.println(currentUser->balance);
  
  if (currentUser->balance >= price) {
    // Successful purchase
    currentState = STATE_DISPENSING;
    
    // Deduct balance
    currentUser->balance -= price;
    
    // Show dispensing animation
    displayDispensingProduct(productNum);
    
    // Show success message
    displayPurchaseSuccess(currentUser->balance);
    
    // Log transaction
    Serial.println("[SUCCESS] Purchase completed!");
    Serial.print("[SUCCESS] New balance: PHP ");
    Serial.println(currentUser->balance);
    
  } else {
    // Insufficient balance (should not happen if checks are correct)
    displayInsufficientBalance();
    Serial.println("[FAILED] Insufficient balance!");
  }
  
  Serial.println("=======================\n");
}

void logoutUser() {
  if (currentUser != NULL) {
    Serial.print("\n[Logout] ");
    Serial.print(currentUser->name);
    Serial.print(" logged out. Final balance: PHP ");
    Serial.println(currentUser->balance);
    
    // Show logout message
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
  
  displayIdleScreen();
}

void logLogin() {
  if (currentUser == NULL) return;
  
  Serial.println("\n=== USER LOGGED IN ===");
  Serial.print("Name: ");
  Serial.println(currentUser->name);
  Serial.print("Card UID: ");
  Serial.println(currentUser->cardUid);
  Serial.print("Balance: PHP ");
  Serial.println(currentUser->balance);
  Serial.println("=====================\n");
}

void updateStatusDisplay() {
  // Update Serial Monitor with status
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
  
  Serial.print("Last UID: ");
  Serial.println(lastScannedUID);
  Serial.print("Button Counts: B1=");
  Serial.print(button1PressCount);
  Serial.print(", B2=");
  Serial.println(button2PressCount);
  Serial.println("---------------");
}