#ifndef TFT_DRIVER_H
#define TFT_DRIVER_H

#include <Arduino.h>
#include <SPI.h>
#include "hw_config.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#define DISPLAY_WIDTH  320
#define DISPLAY_HEIGHT 240

class TFTDriver : public Adafruit_ST7789 {
  public:
    TFTDriver() : Adafruit_ST7789(&SPI, HW_TFT_CS, HW_TFT_DC, HW_TFT_RST) {}
    
    void init() {
        Adafruit_ST7789::init(240, 320);
        uint8_t madctl = 0xF0;
#ifdef HW_TFT_MADCTL
        madctl = HW_TFT_MADCTL;
#endif
        sendCommand(ST77XX_MADCTL, &madctl, 1);
        invertDisplay(false);
        setRotation(1);
        sendCommand(ST77XX_MADCTL, &madctl, 1);
        fillScreen(0x0000);
    }
    
    void drawFilledRect(int x, int y, int w, int h, uint16_t color) {
        fillRect(x, y, w, h, color);
    }
    
    void drawString(int x, int y, const char *str, uint16_t fg, uint16_t bg, int scale = 1) {
        setCursor(x, y);
        setTextColor(fg, bg);
        setTextSize(scale);
        print(str);
    }
    
    void pushImage(int x, int y, int w, int h, const uint16_t *data, bool preswapped = false) {
        drawRGBBitmap(x, y, (uint16_t*)data, w, h);
    }
};

#endif
