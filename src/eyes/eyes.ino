// Open EvE - animated robot eyes for the front 128x64 OLED.
//
// Hardware:
//   - MCU:     ESP32-S3-DevKitC-1
//   - Display: 128x64 monochrome OLED (SSD1306) over I2C @ 0x3C
//
// Default I2C pins on ESP32-S3-DevKitC-1 (Arduino-ESP32 core):
//   SDA -> GPIO 8
//   SCL -> GPIO 9
// If you wired the OLED to different pins, change the defines below.
//
// Animation engine: FluxGarage RoboEyes
//   https://github.com/FluxGarage/RoboEyes

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>

// --- Display geometry --------------------------------------------------------
static constexpr uint8_t SCREEN_WIDTH   = 128;
static constexpr uint8_t SCREEN_HEIGHT  = 64;
static constexpr int8_t  OLED_RESET     = -1;     // shared with MCU reset
static constexpr uint8_t SCREEN_ADDRESS = 0x3C;   // common SSD1306 I2C address

// --- I2C pins (ESP32-S3 defaults; override if your wiring differs) -----------
static constexpr int8_t I2C_SDA_PIN = 8;
static constexpr int8_t I2C_SCL_PIN = 9;
static constexpr uint32_t I2C_CLOCK_HZ = 400000;  // 400 kHz fast-mode I2C

// --- Driver objects ----------------------------------------------------------
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
RoboEyes<Adafruit_SSD1306> roboEyes(display);

// --- Mood demo cycle ---------------------------------------------------------
// Cycle through every supported mood so you can verify the animation is alive.
// Remove this scheduler and call setMood()/anim_*() from your own logic later.
struct MoodStep {
  uint8_t mood;            // DEFAULT / HAPPY / TIRED / ANGRY
  uint16_t hold_ms;        // how long to stay in this mood
  void (*onEnter)();       // optional one-shot effect on entering this mood
};

static void triggerLaugh()    { roboEyes.anim_laugh(); }
static void triggerConfused() { roboEyes.anim_confused(); }

static const MoodStep kMoodCycle[] = {
  { DEFAULT, 4000, nullptr },
  { HAPPY,   4000, triggerLaugh },
  { TIRED,   4000, nullptr },
  { ANGRY,   4000, triggerConfused },
};
static constexpr size_t kMoodCycleLen = sizeof(kMoodCycle) / sizeof(kMoodCycle[0]);

static size_t moodIndex = 0;
static unsigned long moodChangeAt = 0;

static void applyMood(size_t i) {
  const MoodStep &step = kMoodCycle[i];
  roboEyes.setMood(step.mood);
  if (step.onEnter) step.onEnter();
}

// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(F("[open-eve] booting eye animation..."));

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_CLOCK_HZ);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("[open-eve] SSD1306 init failed - check wiring/address"));
    while (true) { delay(1000); }
  }
  display.clearDisplay();
  display.display();

  // Bring the eyes to life.
  roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, /*max fps*/ 60);
  roboEyes.setDisplayColors(BLACK, WHITE);   // monochrome: 0 = bg, 1 = drawings

  // Eye geometry - tweak to taste.
  roboEyes.setWidth(36, 36);
  roboEyes.setHeight(36, 36);
  roboEyes.setBorderradius(8, 8);
  roboEyes.setSpacebetween(10);

  // Background behaviours that make the face feel alive.
  roboEyes.setAutoblinker(ON, /*every*/ 3, /*+random*/ 2);  // seconds
  roboEyes.setIdleMode(ON, /*every*/ 2, /*+random*/ 2);     // seconds
  roboEyes.setCuriosity(ON);

  roboEyes.open();
  applyMood(moodIndex);
  moodChangeAt = millis() + kMoodCycle[moodIndex].hold_ms;

  Serial.println(F("[open-eve] eyes online"));
}

void loop() {
  roboEyes.update();  // handles its own framerate cap (60 fps configured above)

  const unsigned long now = millis();
  if ((long)(now - moodChangeAt) >= 0) {
    moodIndex = (moodIndex + 1) % kMoodCycleLen;
    applyMood(moodIndex);
    moodChangeAt = now + kMoodCycle[moodIndex].hold_ms;
  }
}
