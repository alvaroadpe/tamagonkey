#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <sys/time.h>
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/gpio.h"

#include "images.h"

// ============================================================================
// TIME SCALE MULTIPLIER (Affects Stat Decay, Poop, Sleep, Age & Crying)
// 1.0 = Normal, 10.0 = 10x Faster, 0.5 = Half Speed
// ============================================================================
const float TIME_SCALE = 1.0f; 

// ============================================================================
// HARDWARE PIN DEFINITIONS (ESP32-C3 SuperMini Final Map)
// ============================================================================
#define SDA_PIN        5
#define SCL_PIN        6
#define BTN_SCROLL     0
#define BTN_SELECT     1
#define BTN_BACK       2
#define BUZZER_PIN     7  
#define BAT_ADC_PIN    4  
#define CHRG_SENSE_PIN 3  

const bool CHRG_ACTIVE_LOW = false; 

// ============================================================================
// DISPLAY CONFIGURATION
// ============================================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ============================================================================
// OTA, ADMIN & EMERGENCY SAVE VARIABLES
// ============================================================================
WebServer server(80);
Preferences preferences;

bool isOTAMode = false;
int selectPressCount = 0;
unsigned long lastSelectPressTime = 0;
unsigned long backHoldStartOTA = 0;

RTC_DATA_ATTR int adminSelection = 0;
const int ADMIN_ITEMS_COUNT = 3;
const char* adminItems[] = { "Upload OTA", "Edit Stats", "Factory Reset" };

RTC_DATA_ATTR int statEditSelection = 0;
RTC_DATA_ATTR bool isEditingStat = false;

// ============================================================================
// REAL-TIME CLOCK HELPER (Survives Sleep)
// ============================================================================
int64_t getNowUs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

// ============================================================================
// BATTERY & CHARGING HELPERS
// ============================================================================
bool isCharging() {
  int val = digitalRead(CHRG_SENSE_PIN);
  return CHRG_ACTIVE_LOW ? (val == LOW) : (val == HIGH);
}

float getBatteryVoltage() {
  uint32_t total = 0;
  for(int i = 0; i < 5; i++) {
    total += analogReadMilliVolts(BAT_ADC_PIN);
    delay(2);
  }
  return ((total / 5.0f) * 2.0f) / 1000.0f;
}

int getBatteryPercentage() {
  float batVoltage = getBatteryVoltage();
  if (batVoltage >= 4.07f) return 100;
  if (batVoltage <= 3.3f) return 0;
  return (int)(((batVoltage - 3.3f) / (4.07f - 3.3f)) * 100.0f);
}

bool isBatteryLow() {
  return getBatteryPercentage() <= 15;
}

// ============================================================================
// APP STATE & RTC RAM VARIABLES (Maintained in RAM during light sleep)
// ============================================================================
enum Screen { TITLE, PET_HOME, MENU, FEED, PLAY, SLEEP, HIBERNATE_CONFIRM, DEAD, ADMIN_MENU, ADMIN_STATS, OTA_ACTIVE };

RTC_DATA_ATTR bool isInitialized = false;
RTC_DATA_ATTR Screen currentScreen = TITLE;

RTC_DATA_ATTR int hunger = 80;     
RTC_DATA_ATTR int happiness = 80;  

RTC_DATA_ATTR int menuSelection = 0;
const int MENU_ITEMS_COUNT = 4;
const char* menuItems[] = { "Fud", "Bongo", "Klin", "Zzz" };

RTC_DATA_ATTR int foodSelection = 0;

// Hibernation & Death Variables
RTC_DATA_ATTR bool isHibernating = false;
RTC_DATA_ATTR int hibernateChoice = 0; 
RTC_DATA_ATTR bool isDead = false;

// Dual Poop Scheduling System
RTC_DATA_ATTR int poopCount = 0;               
RTC_DATA_ATTR int64_t poopTarget1Us = 0;       
RTC_DATA_ATTR int64_t poopTarget2Us = 0;       

// Age / Day Counter Variables
RTC_DATA_ATTR int ageDays = 0;
RTC_DATA_ATTR int64_t ageProgressUs = 0;
const int64_t ONE_DAY_US = 86400000000LL;

// Zero Stat / Condition Crying & Continuous Death Counter
RTC_DATA_ATTR bool zeroStatTimerActive = false;
RTC_DATA_ATTR int64_t zeroStatStartTimestampUs = 0;
RTC_DATA_ATTR int cryingRepeatCount = 0; 

// Sleep Variables
RTC_DATA_ATTR int64_t sleepStartTimestampUs = 0;
const int64_t BASE_SLEEP_DURATION_US = 36000000000LL;

// Time Tracking
RTC_DATA_ATTR int64_t lastTimestampUs = 0;

// Local Session Variables
unsigned long lastActivityTime = 0;
const unsigned long INACTIVITY_TIMEOUT_MS = 15000;
const unsigned long HOLD_MS = 5000;                

unsigned long backHoldStart = 0;
bool backHoldTriggered = false;

unsigned long scrollHoldStart = 0;
bool scrollHoldTriggered = false;

bool prevScroll = HIGH;
bool prevSelect = HIGH;
bool prevBack   = HIGH;
unsigned long lastPressTime = 0;
const unsigned long DEBOUNCE_DELAY = 200;

// Forward Declarations
void renderPetHomeWithCustomSprite(const unsigned char* customSprite);
void renderPetHome();
void renderTitleScreen();
void prepareAndEnterDeepSleep();
void updateZeroStatState(bool playAnimation = true);
void startOTAServer();
void renderAdminMenu();
void renderAdminStats();
void renderOTAActive();
void run10SecondCrying();
void renderDeadScreen();
void saveGameState();

// ============================================================================
// PERSISTENT SAVE SYSTEM
// ============================================================================
void saveGameState() {
  preferences.begin("onkey", false);
  preferences.putBool("hasSave", true);
  preferences.putInt("hunger", hunger);
  preferences.putInt("happy", happiness);
  preferences.putInt("poopC", poopCount);
  preferences.putLong64("poop1", poopTarget1Us);
  preferences.putLong64("poop2", poopTarget2Us);
  preferences.putInt("ageD", ageDays);
  preferences.putLong64("ageP", ageProgressUs);
  preferences.putLong64("lastT", lastTimestampUs);
  preferences.putBool("isDead", isDead);
  preferences.putBool("isHib", isHibernating);
  preferences.end();
}

// ============================================================================
// SCALED GAME MECHANIC HELPERS
// ============================================================================
inline int64_t getScaledSleepDurationUs() {
  return (int64_t)(BASE_SLEEP_DURATION_US / TIME_SCALE);
}
inline int64_t getScaledOneHourUs() {
  return (int64_t)(3600000000LL / TIME_SCALE);
}
inline int64_t getScaledOneDayUs() {
  return (int64_t)(ONE_DAY_US / TIME_SCALE);
}

// ============================================================================
// BUTTON FLUSH HELPER
// ============================================================================
void flushButtons() {
  delay(50);
  prevScroll = digitalRead(BTN_SCROLL);
  prevSelect = digitalRead(BTN_SELECT);
  prevBack   = digitalRead(BTN_BACK);
  lastPressTime = millis();
}

// ============================================================================
// AUDIO & INTERRUPTIBLE CRYING FUNCTIONS WITH ANIMATION
// ============================================================================
void playTone(int freq, int duration) {
  if (isOTAMode) return; 
  pinMode(BUZZER_PIN, OUTPUT);
  tone(BUZZER_PIN, freq, duration);
  delay(duration);
  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, LOW);
}

void soundBeep()      { playTone(1000, 50); }
void soundSelect()    { playTone(1500, 80); }
void soundHappy()     { playTone(1200, 100); delay(50); playTone(1600, 150); }
void soundAngry()     { playTone(300, 250); delay(50); playTone(200, 250); }
void soundDrumLeft()  { playTone(220, 60); }
void soundDrumRight() { playTone(330, 60); }
void soundMistake()   { playTone(120, 180); }

bool checkButtonPressDuringCry() {
  return (digitalRead(BTN_SCROLL) == LOW || 
          digitalRead(BTN_SELECT) == LOW || 
          digitalRead(BTN_BACK) == LOW);
}

bool playToneInterruptible(int freq, int duration) {
  pinMode(BUZZER_PIN, OUTPUT);
  tone(BUZZER_PIN, freq, duration);
  unsigned long start = millis();
  while (millis() - start < (unsigned long)duration) {
    if (checkButtonPressDuringCry()) {
      noTone(BUZZER_PIN);
      digitalWrite(BUZZER_PIN, LOW);
      return true;
    }
    delay(5);
  }
  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, LOW);
  return false;
}

bool delayInterruptible(int duration) {
  unsigned long start = millis();
  while (millis() - start < (unsigned long)duration) {
    if (checkButtonPressDuringCry()) return true;
    delay(5);
  }
  return false;
}

bool soundCryingInterruptibleFrame(bool frameToggle) {
  renderPetHomeWithCustomSprite(frameToggle ? epd_bitmap_crying_onkey : epd_bitmap_sad_onkey);
  if (playToneInterruptible(700, 150)) return true;
  if (delayInterruptible(40)) return true;
  
  if (playToneInterruptible(500, 180)) return true;
  if (delayInterruptible(40)) return true;
  
  renderPetHomeWithCustomSprite(!frameToggle ? epd_bitmap_crying_onkey : epd_bitmap_sad_onkey);
  if (playToneInterruptible(350, 250)) return true;
  return false;
}

void soundCryingAnimated() {
  renderPetHomeWithCustomSprite(epd_bitmap_crying_onkey);
  playTone(700, 150);
  delay(40);
  playTone(500, 180);
  delay(40);
  renderPetHomeWithCustomSprite(epd_bitmap_sad_onkey);
  playTone(350, 250);
}

void run10SecondCrying() {
  unsigned long start = millis();
  bool interrupted = false;
  bool frameToggle = false;

  while (millis() - start < 10000) {
    if (soundCryingInterruptibleFrame(frameToggle)) {
      interrupted = true;
      break;
    }
    frameToggle = !frameToggle;
    if (delayInterruptible(200)) {
      interrupted = true;
      break;
    }
  }
  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, LOW);

  if (interrupted) {
    unsigned long waitStart = millis();
    while ((digitalRead(BTN_SCROLL) == LOW || digitalRead(BTN_SELECT) == LOW || digitalRead(BTN_BACK) == LOW) && (millis() - waitStart < 500)) delay(10);
    flushButtons();
    currentScreen = PET_HOME;
    renderPetHome();
    lastActivityTime = millis();
  } else {
    prepareAndEnterDeepSleep();
  }
}

// ============================================================================
// ZERO-STAT & CONDITION EVALUATOR
// ============================================================================
void updateZeroStatState(bool playAnimation) {
  if (isDead) return;

  int64_t nowUs = getNowUs();

  if (hunger <= 0 || happiness <= 0 || poopCount >= 2) {
    if (!zeroStatTimerActive) {
      zeroStatTimerActive = true;
      zeroStatStartTimestampUs = nowUs;
      cryingRepeatCount = 0;
      if (playAnimation) soundCryingAnimated(); // Only triggers if true
    }
  } else {
    zeroStatTimerActive = false;
    cryingRepeatCount = 0;
  }
}

uint32_t getRandomRange(uint32_t minVal, uint32_t maxVal) {
  return minVal + (esp_random() % (maxVal - minVal + 1));
}

// ============================================================================
// WELL-BEING & SPRITE ROUTING
// ============================================================================
int getOverallWellBeing() {
  return (hunger + happiness) / 2;
}

const unsigned char* getPetSprite() {
  if (isDead) return epd_bitmap_critical_onkey;
  
  int score = getOverallWellBeing();
  if (score >= 70) return epd_bitmap_happy_onkey;
  else if (score >= 40) return epd_bitmap_normal_onkey;
  else if (score >= 20) return epd_bitmap_sad_onkey;
  else return epd_bitmap_critical_onkey;
}

// ============================================================================
// DYNAMIC SPRITE DRAWING HELPERS
// ============================================================================
void drawSprite(int x, int y, const unsigned char* bitmap, uint8_t w, uint8_t h) {
  display.drawBitmap(x, y, bitmap, w, h, SSD1306_WHITE);
}

void drawSpriteCentered(int y, const unsigned char* bitmap, uint8_t w, uint8_t h) {
  int centeredX = (SCREEN_WIDTH - w) / 2;
  display.drawBitmap(centeredX, y, bitmap, w, h, SSD1306_WHITE);
}

// ============================================================================
// RETROACTIVE TIME, STAT DECAY & AGE PROCESSOR
// ============================================================================
void processPassageOfTime() {
  if (isHibernating || isDead || isOTAMode) return;

  int64_t nowUs = getNowUs();

  if (lastTimestampUs == 0 || nowUs < lastTimestampUs) {
    lastTimestampUs = nowUs; 
    return;
  }

  int64_t elapsedUs = nowUs - lastTimestampUs;
  int64_t scaledTenMinIntervalUs = (int64_t)(600000000LL / TIME_SCALE);

  ageProgressUs += elapsedUs;
  int64_t scaledDayUs = getScaledOneDayUs();
  while (ageProgressUs >= scaledDayUs) {
    ageProgressUs -= scaledDayUs;
    ageDays++;
  }

  int tenMinIntervalsPassed = elapsedUs / scaledTenMinIntervalUs;

  if (tenMinIntervalsPassed > 0) {
    hunger = max(0, hunger - tenMinIntervalsPassed);
    happiness = max(0, happiness - tenMinIntervalsPassed);
    lastTimestampUs += (int64_t)tenMinIntervalsPassed * scaledTenMinIntervalUs;
  }

  if (poopTarget1Us > 0 && nowUs >= poopTarget1Us) {
    if (poopCount < 2) poopCount++;
    poopTarget1Us = 0; 
  }

  if (poopTarget2Us > 0 && nowUs >= poopTarget2Us) {
    if (poopCount < 2) poopCount++;
    poopTarget2Us = 0; 
  }

  if (poopCount >= 2) {
    poopTarget1Us = 0;
    poopTarget2Us = 0;
  }

  updateZeroStatState(true);
}

// ============================================================================
// OTA SERVER LOGIC (EMERGENCY SAVE & UPDATE)
// ============================================================================
void startOTAServer() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ONKEY_UPDATE");

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", 
      "<h2>Onkey OTA Update</h2>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update'><br><br>"
      "<input type='submit' value='Update Firmware'>"
      "</form>");
  });

  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "UPDATE FAILED" : "UPDATE SUCCESS! RESTARTING...");
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      
      saveGameState(); 
      
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        // Success
      } else {
        Update.printError(Serial);
      }
    }
  });
  server.begin();
}

// ============================================================================
// LIGHT SLEEP ENTRY & POST-WAKEUP ROUTING
// ============================================================================
void prepareAndEnterDeepSleep() {
  if (isCharging() || isOTAMode) {
    lastActivityTime = millis();
    return; 
  }

  if (!isHibernating && !isDead) {
    processPassageOfTime();
  }

  noTone(BUZZER_PIN);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  gpio_wakeup_enable((gpio_num_t)BTN_SCROLL, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)BTN_SELECT, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)BTN_BACK,   GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)CHRG_SENSE_PIN, GPIO_INTR_HIGH_LEVEL);
  
  esp_sleep_enable_gpio_wakeup();

  if (!isHibernating && !isDead) {
    uint64_t sleepUs = (uint64_t)(600000000ULL / TIME_SCALE);
    int64_t nowUs = getNowUs();

    if (currentScreen == SLEEP) {
      int64_t elapsedSleepUs = nowUs - sleepStartTimestampUs;
      int64_t scaledSleepDurationUs = getScaledSleepDurationUs();

      if (elapsedSleepUs < scaledSleepDurationUs) {
        sleepUs = (uint64_t)(scaledSleepDurationUs - elapsedSleepUs);
      } else {
        sleepUs = 1000000ULL;
      }
    } else if (zeroStatTimerActive) {
      sleepUs = (uint64_t)getScaledOneHourUs();
    }
    esp_sleep_enable_timer_wakeup(sleepUs);
  }

  display.ssd1306_command(SSD1306_DISPLAYOFF);
  
  saveGameState(); 
  
  esp_light_sleep_start();

  display.ssd1306_command(SSD1306_DISPLAYON);
  lastActivityTime = millis();
  
  esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
  
  if (wakeup_cause == ESP_SLEEP_WAKEUP_GPIO) {
    if (isHibernating) {
      unsigned long holdStart = millis();
      bool heldFor5Seconds = true;
      while (millis() - holdStart < HOLD_MS) {
        if (digitalRead(BTN_SCROLL) == HIGH) { heldFor5Seconds = false; break; }
        delay(10);
      }
      if (!heldFor5Seconds) {
        prepareAndEnterDeepSleep();
        return;
      }
      isHibernating = false;
      lastTimestampUs = getNowUs();
      updateZeroStatState(true);
      currentScreen = PET_HOME;
    } else {
      unsigned long waitStart = millis();
      while ((digitalRead(BTN_SCROLL) == LOW || digitalRead(BTN_SELECT) == LOW || digitalRead(BTN_BACK) == LOW) && (millis() - waitStart < 500)) delay(10);
      processPassageOfTime();
      
      if (currentScreen == SLEEP || currentScreen == HIBERNATE_CONFIRM || currentScreen == ADMIN_MENU || currentScreen == ADMIN_STATS) {
        currentScreen = PET_HOME;
      }
    }
  } else if (wakeup_cause == ESP_SLEEP_WAKEUP_TIMER) {
    processPassageOfTime();
    int64_t nowUs = getNowUs();

    if (currentScreen == SLEEP && (nowUs - sleepStartTimestampUs >= getScaledSleepDurationUs())) {
      currentScreen = PET_HOME;
      soundHappy();
    } else if (zeroStatTimerActive && !isDead) {
      cryingRepeatCount++;
      if (cryingRepeatCount >= 72) {
        isDead = true; 
        currentScreen = DEAD; 
        renderDeadScreen(); 
        soundAngry();
        saveGameState(); 
      } else {
        currentScreen = PET_HOME; 
        renderPetHome(); 
        run10SecondCrying();
      }
    } else {
      prepareAndEnterDeepSleep();
      return;
    }
  }

  flushButtons();
  if (currentScreen == PET_HOME) renderPetHome();
  else if (currentScreen == DEAD) renderDeadScreen();
}

// ============================================================================
// UI DRAWING FUNCTIONS
// ============================================================================
void renderTitleScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(30, 8);
  display.print("ONKEY");
  drawSpriteCentered(20, epd_bitmap_happy_onkey, 57, 32);
  display.display();
}

void renderDeadScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(30, 8);
  display.print("ONKEY DIED");
  drawSpriteCentered(22, epd_bitmap_critical_onkey, 57, 32);
  display.display();
}

void renderPetHomeWithCustomSprite(const unsigned char* customSprite) {
  if (isDead) {
    renderDeadScreen();
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int statsX = 94;

  drawSprite(statsX, 6, icon_heart, 8, 8);
  display.setCursor(statsX + 12, 6);
  display.print(hunger);

  drawSprite(statsX, 26, icon_smiley, 8, 8);
  display.setCursor(statsX + 12, 26);
  display.print(happiness);

  drawSprite(statsX, 46, icon_clock, 8, 8);
  display.setCursor(statsX + 12, 46);
  display.print(ageDays);

  float batVoltage = getBatteryVoltage();
  display.setCursor(0, 0);
  
  if (isCharging()) {
    if (batVoltage >= 4.00f) display.print("FULL BAT");
    else display.print("CHARGING");
  } else {
    if (batVoltage >= 4.05f) display.print("FULL BAT");
    else if (isBatteryLow()) display.print("LOW BAT");
  }

  int petX = 18;
  int petY = 16;
  drawSprite(petX, petY, customSprite, 57, 32);

  if (poopCount >= 1) drawSprite(petX - 17, 15, poop, 16, 16);
  if (poopCount == 2) drawSprite(petX + 45, 5, poop, 16, 16);

  display.display();
}

void renderPetHome() {
  renderPetHomeWithCustomSprite(getPetSprite());
}

void renderMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  for (int i = 0; i < MENU_ITEMS_COUNT; i++) {
    int yPos = 4 + (i * 15);
    if (i == menuSelection) {
      display.fillRect(20, yPos - 2, 88, 13, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(30, yPos);
    display.print(menuItems[i]);
  }
  display.display();
}

void renderFeedScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(32, 0);
  display.print("SELECT FOOD");

  drawSprite(16, 16, banana, 32, 32);
  display.setCursor(12, 52);
  display.print("Banana");

  drawSprite(80, 16, coco, 32, 32);
  display.setCursor(78, 52);
  display.print("Coco");

  if (foodSelection == 0) display.drawRect(10, 12, 44, 50, SSD1306_WHITE);
  else display.drawRect(74, 12, 48, 50, SSD1306_WHITE);

  display.display();
}

void renderPlayScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);   display.print("BONGO");
  display.setCursor(0, 10);  display.print("BEAT!");
  display.setCursor(98, 0);  display.print("press");
  display.setCursor(92, 10); display.print("select");

  drawSpriteCentered(2, epd_bitmap_bongos_neutral, 80, 60);
  display.display();
}

void renderHibernateConfirm() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(14, 2);
  display.print("HIBERNATE ONKEY?");
  display.setCursor(8, 18);
  display.print("Freeze stats & pause");
  display.setCursor(8, 28);
  display.print("battery drain?");

  if (hibernateChoice == 0) { 
    display.fillRect(18, 44, 40, 16, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    display.setCursor(28, 48); display.print("YES");
    display.setTextColor(SSD1306_WHITE);
    display.drawRect(70, 44, 40, 16, SSD1306_WHITE);
    display.setCursor(82, 48); display.print("NO");
  } else { 
    display.drawRect(18, 44, 40, 16, SSD1306_WHITE);
    display.setCursor(28, 48); display.print("YES");
    display.fillRect(70, 44, 40, 16, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    display.setCursor(82, 48); display.print("NO");
  }
  display.display();
}

// ============================================================================
// ADMIN MENU SCREENS
// ============================================================================
void renderAdminMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(34, 0); 
  display.print("ADMIN MENU");

  for (int i = 0; i < ADMIN_ITEMS_COUNT; i++) {
    int yPos = 16 + (i * 15);
    if (i == adminSelection) {
      display.fillRect(10, yPos - 2, 108, 13, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(14, yPos); 
    display.print(adminItems[i]);
  }
  display.display();
}

void renderAdminStats() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(34, 0); 
  display.print("EDIT STATS");

  const char* labels[] = {"Hunger: ", "Happy:  ", "Poops:  ", "Age:    "};
  int values[] = {hunger, happiness, poopCount, ageDays};

  for (int i = 0; i < 4; i++) {
    int yPos = 14 + (i * 12);
    if (i == statEditSelection) {
      if (isEditingStat) {
        display.setTextColor(SSD1306_WHITE);
      } else {
        display.fillRect(10, yPos - 2, 108, 11, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
      }
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    
    display.setCursor(14, yPos); 
    if (i == statEditSelection && isEditingStat) display.print("> ");
    else display.print("  ");
    
    display.print(labels[i]); 
    display.print(values[i]);
  }
  display.display();
}

void renderOTAActive() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0); display.print("OTA MODE ACTIVE");
  display.setCursor(0, 16); display.print("Connect Wi-Fi to:");
  display.setCursor(0, 26); display.print("ONKEY_UPDATE");
  display.setCursor(0, 42); display.print("Browser IP:");
  display.setCursor(0, 52); display.print("192.168.4.1");
  display.display();
}

// ============================================================================
// ANIMATIONS & MINI-GAMES
// ============================================================================
void runEatingAnimation() {
  for (int i = 0; i < 3; i++) {
    display.clearDisplay();
    drawSprite(10, 18, getPetSprite(), 57, 32);
    if (i % 2 == 0) drawSprite(75, 18, (foodSelection == 0) ? banana : coco, 32, 32);
    display.display();
    playTone(600 + (i * 200), 100);
    delay(150);
  }
  soundHappy();
  flushButtons();
  lastActivityTime = millis();
}

void runHappyAnimation() {
  display.clearDisplay();
  drawSpriteCentered(18, epd_bitmap_happy_onkey, 57, 32);
  display.display();
  soundHappy();
  delay(500); 
  flushButtons();
  lastActivityTime = millis();
}

void runAngryAnimation() {
  display.clearDisplay();
  drawSpriteCentered(18, epd_bitmap_angry_onkey, 57, 32);
  display.display();
  soundAngry();
  delay(500); 
  flushButtons();
  lastActivityTime = millis();
}

void runCleaningAnimation() {
  for (int i = 0; i < 4; i++) {
    display.clearDisplay();
    drawSprite(12, 18, getPetSprite(), 57, 32);
    int broomX = 78 + ((i % 2 == 0) ? 0 : 6);
    drawSprite(broomX, 18, epd_bitmap_broom, 32, 32);
    display.display();
    playTone(700 + (i * 100), 80);
    delay(150);
  }
  flushButtons();
  lastActivityTime = millis();
}

void runSleepAnimation() {
  int petX = (SCREEN_WIDTH - 57) / 2;
  int petY = 24;

  for (int i = 0; i < 5; i++) {
    display.clearDisplay();
    drawSprite(petX, petY, epd_bitmap_asleep_onkey, 57, 32);

    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    int zx = petX + 28 + (i * 3);
    int zy = petY - 6 - (i * 4);

    display.setCursor(zx, zy);
    if (i % 3 == 0)      display.print("z");
    else if (i % 3 == 1) display.print("Zz");
    else                 display.print("Zzz");

    display.display();
    playTone(500 - (i * 50), 80);
    delay(220);
  }
  flushButtons();
}

void runBongoGame() {
  int score = 0;
  const int bongoX = (SCREEN_WIDTH - 80) / 2;
  const int bongoY = (SCREEN_HEIGHT - 60) / 2;

  for (int round = 0; round < 10; round++) {
    int targetButton = getRandomRange(0, 1);
    display.clearDisplay();
    drawSprite(bongoX, bongoY, epd_bitmap_bongos_neutral, 80, 60);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    if (targetButton == 0) { display.setCursor(0, 28); display.print("LEFT"); } 
    else { display.setCursor(96, 28); display.print("RIGHT"); }
    display.display();

    bool pScroll = digitalRead(BTN_SCROLL);
    bool pSelect = digitalRead(BTN_SELECT);
    unsigned long startTime = millis();
    bool answered = false;
    bool correct  = false;

    while (millis() - startTime < 500) {
      bool cScroll = digitalRead(BTN_SCROLL);
      bool cSelect = digitalRead(BTN_SELECT);
      bool scrollHit = (pScroll == HIGH && cScroll == LOW);
      bool selectHit = (pSelect == HIGH && cSelect == LOW);
      pScroll = cScroll;
      pSelect = cSelect;

      if (scrollHit || selectHit) {
        answered = true;
        if (targetButton == 0 && scrollHit && !selectHit) correct = true;
        else if (targetButton == 1 && selectHit && !scrollHit) correct = true;
        else correct = false;
        break;
      }
      delay(10);
    }

    display.clearDisplay();
    if (answered && correct) {
      score++;
      if (targetButton == 0) {
        drawSprite(bongoX, bongoY, epd_bitmap_bongos_left, 80, 60);
        display.display();
        soundDrumLeft();
      } else {
        drawSprite(bongoX, bongoY, epd_bitmap_bongos_right, 80, 60);
        display.display();
        soundDrumRight();
      }
      delay(200);
    } else {
      drawSprite(bongoX, bongoY, epd_bitmap_bongos_angry, 80, 60);
      display.display();
      soundMistake();
      delay(200);
    }
  }

  if (score >= 8) {
    for (int dance = 0; dance < 4; dance++) {
      display.clearDisplay(); drawSprite(bongoX, bongoY, epd_bitmap_bongos_left, 80, 60); display.display(); soundDrumLeft(); delay(120);
      display.clearDisplay(); drawSprite(bongoX, bongoY, epd_bitmap_bongos_right, 80, 60); display.display(); soundDrumRight(); delay(120);
    }
    happiness = min(100, happiness + 30);
    soundHappy();
  } else {
    happiness = max(0, happiness - 15);
    display.clearDisplay(); drawSprite(bongoX, bongoY, epd_bitmap_bongos_angry, 80, 60); display.display(); soundAngry(); delay(1500);
  }
  hunger = max(0, hunger - 10);
  updateZeroStatState(true);
  flushButtons();
  lastActivityTime = millis();
}

// ============================================================================
// SETUP & MAIN LOOP
// ============================================================================
void setup() {
  WiFi.mode(WIFI_OFF);
  btStop();
  gpio_hold_dis((gpio_num_t)BUZZER_PIN);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BTN_SCROLL, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BTN_BACK,   INPUT_PULLUP);
  pinMode(BAT_ADC_PIN, INPUT);
  pinMode(CHRG_SENSE_PIN, INPUT);

  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
  soundBeep();

  Wire.begin(SDA_PIN, SCL_PIN, 100000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    display.begin(SSD1306_SWITCHCAPVCC, 0x3D);
  }
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(1);

  esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();

  if (wakeup_cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    preferences.begin("onkey", false);
    if (preferences.getBool("hasSave", false)) {
      hunger = 0;       // BATTERY DEATH PENALTY 
      happiness = 0;    // BATTERY DEATH PENALTY
      
      poopCount = preferences.getInt("poopC", 0);
      poopTarget1Us = preferences.getLong64("poop1", 0);
      poopTarget2Us = preferences.getLong64("poop2", 0);
      ageDays = preferences.getInt("ageD", 0);
      ageProgressUs = preferences.getLong64("ageP", 0);
      lastTimestampUs = preferences.getLong64("lastT", getNowUs());
      isDead = preferences.getBool("isDead", false);
      isHibernating = preferences.getBool("isHib", false);
      
      isInitialized = true;
      currentScreen = isDead ? DEAD : PET_HOME;
      
      display.clearDisplay();
      if (isDead) renderDeadScreen();
      else renderPetHome();
      
      display.ssd1306_command(SSD1306_DISPLAYON);
      soundBeep();
      flushButtons();
      lastActivityTime = millis();
    }
    preferences.end();
  }

  if (!isInitialized) {
    isInitialized = true;
    hunger = 80; happiness = 80; poopCount = 0; poopTarget1Us = 0; poopTarget2Us = 0;
    ageDays = 0; ageProgressUs = 0; isHibernating = false; isDead = false;
    cryingRepeatCount = 0; zeroStatTimerActive = false; currentScreen = TITLE;
    lastTimestampUs = getNowUs();

    saveGameState(); 

    display.clearDisplay();
    renderTitleScreen();
    display.ssd1306_command(SSD1306_DISPLAYON);

    soundSelect();
    flushButtons();
    lastActivityTime = millis();
  }
}

void loop() {
  if (isOTAMode) {
    server.handleClient();
    if (digitalRead(BTN_BACK) == LOW) {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      isOTAMode = false;
      soundBeep();
      currentScreen = ADMIN_MENU;
      renderAdminMenu();
      while(digitalRead(BTN_BACK) == LOW) delay(10); 
      flushButtons();
      lastActivityTime = millis();
    }
    return;
  }

  int64_t nowUs = getNowUs();
  if (nowUs - lastTimestampUs >= (int64_t)(600000000LL / TIME_SCALE)) {
    processPassageOfTime();
    if (currentScreen == PET_HOME) renderPetHome();
  }

  static unsigned long lastChargeRefresh = 0;
  if (isCharging() && currentScreen == PET_HOME && millis() - lastChargeRefresh > 3000) {
    renderPetHome();
    lastChargeRefresh = millis();
  }

  // Check 5-second Hold on SCROLL Button to trigger Hibernation
  if (digitalRead(BTN_SCROLL) == LOW && digitalRead(BTN_BACK) == HIGH && currentScreen != HIBERNATE_CONFIRM && currentScreen != SLEEP && !isDead && currentScreen != ADMIN_MENU && currentScreen != ADMIN_STATS) {
    if (scrollHoldStart == 0) {
      scrollHoldStart = millis();
    } else if (millis() - scrollHoldStart >= HOLD_MS && !scrollHoldTriggered) {
      scrollHoldTriggered = true; currentScreen = HIBERNATE_CONFIRM; hibernateChoice = 0;
      soundSelect(); renderHibernateConfirm(); lastActivityTime = millis();
    }
  } else if (digitalRead(BTN_SCROLL) == HIGH) {
    scrollHoldStart = 0; scrollHoldTriggered = false;
  }

  // Check 5-second Hold on BACK Button to Reset when Dead
  if (digitalRead(BTN_BACK) == LOW && digitalRead(BTN_SCROLL) == HIGH && isDead) {
    if (backHoldStart == 0) {
      backHoldStart = millis();
    } else if (millis() - backHoldStart >= HOLD_MS && !backHoldTriggered) {
      backHoldTriggered = true; isDead = false; hunger = 80; happiness = 80; poopCount = 0;
      poopTarget1Us = 0; poopTarget2Us = 0; zeroStatTimerActive = false; cryingRepeatCount = 0;
      currentScreen = PET_HOME; soundHappy(); renderPetHome(); saveGameState(); lastActivityTime = millis();
    }
  } else {
    backHoldStart = 0; backHoldTriggered = false;
  }

  bool currentScroll = digitalRead(BTN_SCROLL);
  bool currentSelect = digitalRead(BTN_SELECT);
  bool currentBack   = digitalRead(BTN_BACK);
  bool scrollPressed = false, selectPressed = false, backPressed = false;

  if (millis() - lastPressTime > DEBOUNCE_DELAY) {
    if (prevScroll == HIGH && currentScroll == LOW && !scrollHoldTriggered) { scrollPressed = true; lastPressTime = millis(); } 
    else if (prevSelect == HIGH && currentSelect == LOW) { selectPressed = true; lastPressTime = millis(); } 
    else if (prevBack == HIGH && currentBack == LOW && !backHoldTriggered) { backPressed = true; lastPressTime = millis(); }
  }

  if (selectPressed) {
    if (millis() - lastSelectPressTime < 2000) selectPressCount++;
    else selectPressCount = 1;
    lastSelectPressTime = millis();
  }

  if (currentBack == LOW && selectPressCount >= 3 && currentScreen != ADMIN_MENU && currentScreen != ADMIN_STATS && currentScreen != OTA_ACTIVE && !isDead) {
    if (backHoldStartOTA == 0) {
      backHoldStartOTA = millis();
    } else if (millis() - backHoldStartOTA >= 10000) {
      currentScreen = ADMIN_MENU;
      adminSelection = 0;
      soundSelect();
      renderAdminMenu();
      selectPressCount = 0; 
      backHoldStartOTA = 0;
      lastActivityTime = millis();
    }
  } else {
    backHoldStartOTA = 0;
  }

  prevScroll = currentScroll; prevSelect = currentSelect; prevBack = currentBack;

  if (scrollPressed || selectPressed || backPressed) {
    lastActivityTime = millis();

    switch (currentScreen) {
      case TITLE:
        if (selectPressed) { soundSelect(); currentScreen = PET_HOME; renderPetHome(); }
        break;

      case PET_HOME:
        if (scrollPressed) { soundBeep(); currentScreen = MENU; renderMenu(); }
        break;

      case MENU:
        if (scrollPressed) { soundBeep(); menuSelection = (menuSelection + 1) % MENU_ITEMS_COUNT; renderMenu(); } 
        else if (selectPressed) {
          soundSelect();
          if (menuSelection == 0) { currentScreen = FEED; renderFeedScreen(); } 
          else if (menuSelection == 1) { currentScreen = PLAY; renderPlayScreen(); } 
          else if (menuSelection == 2) { 
            if (poopCount > 0) { poopCount = 0; happiness = min(100, happiness + 10); runCleaningAnimation(); runHappyAnimation(); } 
            else { happiness = max(0, happiness - 15); runCleaningAnimation(); runAngryAnimation(); }
            updateZeroStatState(true); currentScreen = PET_HOME; renderPetHome();
          } 
          else if (menuSelection == 3) { runSleepAnimation(); currentScreen = SLEEP; sleepStartTimestampUs = getNowUs(); prepareAndEnterDeepSleep(); }
        } else if (backPressed) { soundBeep(); currentScreen = PET_HOME; renderPetHome(); }
        break;

      case FEED:
        if (scrollPressed) { soundBeep(); foodSelection = (foodSelection + 1) % 2; renderFeedScreen(); } 
        else if (selectPressed) {
          runEatingAnimation();
          if (foodSelection == 0) hunger = min(100, hunger + 30);
          else { hunger = min(100, hunger + 25); happiness = min(100, happiness + 5); }
          int totalPendingPoops = poopCount + (poopTarget1Us > 0 ? 1 : 0) + (poopTarget2Us > 0 ? 1 : 0);
          if (totalPendingPoops < 2) {
            int64_t basePoopDelayUs = (int64_t)getRandomRange(3600, 14400) * 1000000LL;
            int64_t newTargetUs = getNowUs() + (int64_t)(basePoopDelayUs / TIME_SCALE);
            if (poopTarget1Us == 0) poopTarget1Us = newTargetUs;
            else if (poopTarget2Us == 0) poopTarget2Us = newTargetUs;
          }
          updateZeroStatState(true); currentScreen = PET_HOME; renderPetHome();
        } else if (backPressed) { soundBeep(); currentScreen = MENU; renderMenu(); }
        break;

      case PLAY:
        if (selectPressed) { runBongoGame(); currentScreen = PET_HOME; renderPetHome(); } 
        else if (backPressed) { soundBeep(); currentScreen = MENU; renderMenu(); }
        break;

      case HIBERNATE_CONFIRM:
        if (scrollPressed) { soundBeep(); hibernateChoice = (hibernateChoice == 0) ? 1 : 0; renderHibernateConfirm(); } 
        else if (selectPressed) {
          if (hibernateChoice == 0) { 
            soundHappy(); display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(20, 28); display.print("Zzz... Hibernating"); display.display(); delay(1000);
            isHibernating = true; prepareAndEnterDeepSleep();
          } else { soundBeep(); currentScreen = PET_HOME; renderPetHome(); }
        } else if (backPressed) { soundBeep(); currentScreen = PET_HOME; renderPetHome(); }
        break;
        
      case ADMIN_MENU:
        if (scrollPressed) {
          soundBeep();
          adminSelection = (adminSelection + 1) % ADMIN_ITEMS_COUNT;
          renderAdminMenu();
        } else if (selectPressed) {
          soundSelect();
          if (adminSelection == 0) { 
            isOTAMode = true;
            currentScreen = OTA_ACTIVE;
            renderOTAActive();
            startOTAServer();
          } else if (adminSelection == 1) { 
            currentScreen = ADMIN_STATS;
            statEditSelection = 0;
            isEditingStat = false;
            renderAdminStats();
          } else if (adminSelection == 2) { 
            hunger = 80; happiness = 80; poopCount = 0; poopTarget1Us = 0; poopTarget2Us = 0;
            ageDays = 0; ageProgressUs = 0; zeroStatTimerActive = false; cryingRepeatCount = 0;
            isHibernating = false; isDead = false; menuSelection = 0; foodSelection = 0;
            currentScreen = TITLE;
            
            preferences.begin("onkey", false);
            preferences.clear();
            preferences.end();
            saveGameState(); 
            
            soundHappy(); renderTitleScreen();
          }
        } else if (backPressed) {
          soundBeep();
          currentScreen = PET_HOME;
          renderPetHome();
        }
        break;

      case ADMIN_STATS:
        if (scrollPressed) {
          soundBeep();
          if (!isEditingStat) {
            statEditSelection = (statEditSelection + 1) % 4;
          } else {
            if (statEditSelection == 0) hunger = (hunger + 10) % 110;          
            else if (statEditSelection == 1) happiness = (happiness + 10) % 110; 
            else if (statEditSelection == 2) poopCount = (poopCount + 1) % 3;    
            else if (statEditSelection == 3) ageDays = (ageDays + 1) % 100;    
            updateZeroStatState(false); 
          }
          renderAdminStats();
        } else if (backPressed) {
          soundBeep();
          if (isEditingStat) {
            if (statEditSelection == 0) hunger = (hunger - 10 < 0) ? 100 : hunger - 10;
            else if (statEditSelection == 1) happiness = (happiness - 10 < 0) ? 100 : happiness - 10;
            else if (statEditSelection == 2) poopCount = (poopCount - 1 < 0) ? 2 : poopCount - 1;
            else if (statEditSelection == 3) ageDays = (ageDays - 1 < 0) ? 99 : ageDays - 1;
            updateZeroStatState(false); 
            renderAdminStats();
          } else {
            currentScreen = ADMIN_MENU;
            updateZeroStatState(true); // Verifies logic cleanly upon exiting editor mode
            renderAdminMenu();
          }
        } else if (selectPressed) {
          soundSelect();
          isEditingStat = !isEditingStat; 
          renderAdminStats();
        }
        break;

      case OTA_ACTIVE:
      case SLEEP:
      case DEAD:
        break;
    }
  }

  if ((millis() - lastActivityTime >= INACTIVITY_TIMEOUT_MS) && !isCharging()) {
    prepareAndEnterDeepSleep();
  }
}