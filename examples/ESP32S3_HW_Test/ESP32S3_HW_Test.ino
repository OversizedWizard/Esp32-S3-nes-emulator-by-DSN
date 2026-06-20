/**
 * ESP32-S3 Hardware Test – Inversion Correction & Volume Reset
 * RED = VOL- | BLUE = VOL+ | GREEN = PLAY
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SD.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include "Audio.h" 

// ─── Pin Map ─────────────────────────────────────────────────────────────────
#define BTN_UP     44
#define BTN_DOWN    1
#define BTN_LEFT   43
#define BTN_RIGHT    2
#define BTN_START  41
#define BTN_SELECT 42
#define BTN_A      40
#define BTN_B      39
#define BTN_X      47
#define BTN_Y      21

#define PIN_MOSI 7
#define PIN_SCK 15
#define PIN_MISO 17
#define TFT_CS 4
#define TFT_DC 6
#define TFT_RST 5
#define LED_BL 16
#define SD_CS 18
#define TOUCH_SCK 11  
#define TOUCH_MISO 20 
#define TOUCH_MOSI 13 
#define T_CS 12  
#define T_IRQ 45 
#define I2S_DOUT 9   
#define I2S_BCLK 10   
#define I2S_LRC 14   

// ─── Objects & Global State ──────────────────────────────────────────────────
Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, TFT_CS, TFT_DC, TFT_RST);
SPIClass touchSpi(HSPI); 
XPT2046_Touchscreen touch(T_CS, T_IRQ);
Audio audio; 

bool sdOk = false;
String sdInfo = "--";
char tchInf[32] = "waiting...";
String touchCoords = "Tap screen to test";
int currentVolume = 12;   
uint32_t tLast = 0;      
uint32_t flashTimer = 0;
bool isFlashing = false;

#define COL_BG tft.color565(0, 0, 0)
#define COL_OK ST77XX_GREEN
#define COL_FAIL ST77XX_RED
#define COL_WAIT ST77XX_YELLOW
#define COL_TXT ST77XX_WHITE

// ─── Functions ───────────────────────────────────────────────────────────────

void initSD() {
  if (SD.begin(SD_CS, SPI, 4000000)) {
    sdOk = true;
    uint32_t tot = (uint32_t)(SD.cardSize() / 1048576ULL);
    sdInfo = String(tot) + " MB Mounted";
  } else {
    sdOk = false;
    sdInfo = "SD FAILED";
  }
}

void updateVolumeDisplay() {
  tft.fillRect(10, 90, 310, 20, COL_BG);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(10, 90);
  tft.print("   VOLUME: ");
  tft.print(currentVolume);
  tft.print(" / 21  (");
  tft.print(sdInfo);
  tft.print(")");
}

void drawUI(bool firstRun = true, bool clearScreen = true) {
  if (firstRun) {
    if (clearScreen) tft.fillRect(0, 0, 320, 240, COL_BG);
    tft.fillRect(0, 0, 320, 26, ST77XX_ORANGE);
    tft.setTextColor(COL_BG); tft.setTextSize(2);
    tft.setCursor(6, 6); tft.print("ESP32-S3 HW Test");
    tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(10, 40); tft.print("DISPLAY: ");
    tft.setTextColor(COL_OK); tft.print("OK (ST7789V)");
    tft.setCursor(10, 70); tft.setTextColor(COL_TXT); tft.print("SD CARD: ");
    tft.setTextColor(sdOk ? COL_OK : COL_FAIL); tft.print(sdOk ? "OK" : "FAIL");
    
    updateVolumeDisplay();

    tft.setCursor(10, 120); tft.setTextColor(COL_TXT); tft.print("BUTTONS: ");
    tft.setCursor(10, 150); tft.print("TOUCH:   ");

    tft.fillRect(30, 190, 100, 40, ST77XX_RED);
    tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2);
    tft.setCursor(45, 203); tft.print("VOL -");

    tft.fillRect(190, 190, 100, 40, ST77XX_BLUE);
    tft.setCursor(205, 203); tft.print("VOL +");

    tft.fillRect(190, 130, 100, 40, ST77XX_GREEN);
    tft.setTextColor(ST77XX_BLACK); tft.setTextSize(1);
    tft.setCursor(205, 145); tft.print("SOUND TEST");
  }
  tft.setTextSize(1);
  tft.setTextColor(COL_WAIT, COL_BG);
  tft.setCursor(80, 150); tft.print(tchInf); tft.print("        ");
  tft.setCursor(10, 170);
  tft.setTextColor(tchInf[0] == 'T' ? COL_OK : COL_WAIT, COL_BG);
  if (tchInf[0] == 'T') tft.print(touchCoords);
}

void setup() {
  Serial.begin(115200);
  delay(400);

  pinMode(TFT_CS, OUTPUT); digitalWrite(TFT_CS, HIGH);
  pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH);
  pinMode(T_CS, OUTPUT); digitalWrite(T_CS, HIGH);
  pinMode(LED_BL, OUTPUT); digitalWrite(LED_BL, HIGH);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  
  // ─── COLOR RE-FIX ───
  tft.init(240, 320);
  uint8_t madctl = 0xF8;
  tft.sendCommand(ST77XX_MADCTL, &madctl, 1);
  tft.invertDisplay(false); // <--- Changed from true to false
  tft.setRotation(1); 
  tft.sendCommand(ST77XX_MADCTL, &madctl, 1);

  initSD();

  touchSpi.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, -1);
  touch.begin(touchSpi);
  touch.setRotation(1);

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(currentVolume);
  
  drawUI(true); 
}

void loop() {
  audio.loop(); 

  if (isFlashing && millis() - flashTimer > 100) {
    drawUI(true, false); 
    isFlashing = false;
  }

  if (touch.touched()) {
    TS_Point p = touch.getPoint();
    if (p.z > 400 && p.z < 4000) {
      if (millis() - tLast > 150) { 
        tLast = millis();
        int16_t sx = map(p.x, 3750, 190, 0, 320);
        int16_t sy = map(p.y, 3750, 219, 0, 240);
        sx = constrain(sx, 0, 320); sy = constrain(sy, 0, 240);

        strlcpy(tchInf, "Tapped!", sizeof(tchInf));
        drawUI(false); 

        if (sx >= 190 && sx <= 290 && sy >= 130 && sy <= 170) {
           tft.fillRect(190, 130, 100, 40, ST77XX_WHITE);
           if (sdOk) audio.connecttoFS(SD, "/test.wav");
           flashTimer = millis(); isFlashing = true;
        }

        if (sx >= 30 && sx <= 130 && sy >= 190 && sy <= 230) {
          tft.fillRect(30, 190, 100, 40, ST77XX_WHITE);
          if (currentVolume > 0) currentVolume--;
          audio.setVolume(currentVolume);
          updateVolumeDisplay();
          flashTimer = millis(); isFlashing = true;
        }

        if (sx >= 190 && sx <= 290 && sy >= 190 && sy <= 230) {
          tft.fillRect(190, 190, 100, 40, ST77XX_WHITE);
          if (currentVolume < 21) currentVolume++;
          audio.setVolume(currentVolume);
          updateVolumeDisplay();
          flashTimer = millis(); isFlashing = true;
        }
      }
    }
  } else {
    if (millis() - tLast > 250 && tchInf[0] == 'T') {
      strlcpy(tchInf, "Released", sizeof(tchInf));
      drawUI(false);
    }
  }
}