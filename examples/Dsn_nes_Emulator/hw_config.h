#ifndef HW_CONFIG_H
#define HW_CONFIG_H

// Set to 1 to enable sound, 0 to disable
#define ENABLE_SOUND 1

// Set to 1 to enable touchscreen in the menu, 0 to disable
#define ENABLE_TOUCH 1

// --- SD Card Pins (Shared SPI) ---
#define SD_CS        18
#define SD_SCK       15
#define SD_MOSI       7
#define SD_MISO      17

// --- I2S Audio Pins (Your New Layout) ---
#define I2S_DO        9  // DIN
#define I2S_BCK      10  // BCLK
#define I2S_WS       14  // LRC / Word Select
#define I2S_GAIN      3  
#define I2S_SD        8  // Shutdown

// --- TFT Display Pins (Shared SPI) ---
#define HW_TFT_MOSI   7
#define HW_TFT_SCK   15
#define HW_TFT_CS     4
#define HW_TFT_DC     6
#define HW_TFT_RST    5
#define LED_BL       16

// ST7789 display orientation (MADCTL)
#define HW_TFT_MADCTL 0xF0 // 180-degree rotation (0x30 ^ 0xC0)
#define HW_TFT_COL_OFFSET 0
#define HW_TFT_ROW_OFFSET 0

// --- Controller Pins (Safe & Stable) ---
#define BTN_UP       44
#define BTN_DOWN      1
#define BTN_LEFT     43
#define BTN_RIGHT     2
#define BTN_START    41
#define BTN_SELECT   42
#define BTN_A        40
#define BTN_B        39
#define BTN_X        47
#define BTN_y        21  

// --- Touch Controller (Shared SPI) ---
#define T_CLK        11
#define T_CS         12
#define T_DIN        13
#define T_DO         20
#define T_IRQ        45


#endif
