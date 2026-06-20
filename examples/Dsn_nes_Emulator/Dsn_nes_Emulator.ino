#include "hw_config.h"
#include "tft_driver.h"
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <Adafruit_NeoPixel.h>
TFTDriver tft;

extern void setup_controller();
extern "C" int nofrendo_main(int argc, char *argv[]);
extern "C" bool vid_preload_rom(const char *path);
extern "C" int show_menu();
extern "C" const char *get_selected_game();
Adafruit_NeoPixel pixel(1, 48, NEO_GRB + NEO_KHZ800);

void setup() {
  // Turn on the backlight
  pinMode(LED_BL, OUTPUT);
  digitalWrite(LED_BL, HIGH);

  pixel.begin();  
  pixel.setBrightness(20);
  pixel.setPixelColor(0, pixel.Color(0, 10, 0)); 
  pixel.show();
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n====================================");
  Serial.println("Starting DSN NES Emulator Setup");
  Serial.println("====================================\n");

  setup_controller();
  Serial.println("[1/4] Controller initialized.");

  Serial.println("[2/4] Mounting SD card...");

  // Initialize shared SPI bus
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, -1);

  if (!SD.begin(SD_CS, SPI, 20000000)) {
    Serial.println("     SD Mount failed - ROM won't be accessible");
  } else {
    Serial.println("     ✓ SD Card mounted");
    File root = SD.open("/");
    if (root) {
      Serial.println("     Files on SD:");
      File file = root.openNextFile();
      while (file && file.name()[0] != 0) {
        Serial.printf("       - %s (%d bytes)\n", file.name(), file.size());
        file = root.openNextFile();
      }
      root.close();
    }
  }
  
  if (!SD.exists("/Saved")) {
     SD.mkdir("/Saved");
     Serial.println("     ✓ Created /Saved directory");
  }
  
  delay(200);

  Serial.println("[3/4] Initializing TFT display...");
  delay(100);

  tft.init();
  delay(100);
  tft.fillScreen(tft.color565(0, 0, 0));
  delay(100);
  Serial.printf("     ✓ TFT ready: %dx%d\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);

  Serial.println("[4/4] ROM Loading Configuration");

  delay(100);
  Serial.printf("     Free heap: %u bytes\n", esp_get_free_heap_size());
  Serial.printf("     Free SPIRAM: %u bytes\n",
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("     Total SPIRAM: %u bytes\n",
                heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
    Serial.println("     WARNING: PSRAM not initialized.");
    Serial.println(
        "     Set correct board/PSRAM mode in Arduino IDE (Tools menu).");
  }

  Serial.println(
      "\n     ROM preloading DISABLED - emulator will load on demand");
  Serial.println("     (If emulator crashes, ROM loading is the issue)");

  Serial.println("\n====================================");
  Serial.println("Setup complete - Entering main loop");
  Serial.println("====================================\n");
  delay(100);
  Serial.flush();

  while (1) {
    int selected = show_menu();
    if (selected < 0) {
      Serial.println("No game selected, waiting...");
      delay(1000);
      continue;
    }

    const char *rom_file = get_selected_game();
    Serial.printf("Starting game: %s\n", rom_file);

#if ENABLE_SOUND
    char *argv[] = {"nes",     "-sound", "-volume",       "100",
                    "-sample", "22050",  (char *)rom_file};
    nofrendo_main(7, argv);
#else
    char *argv[] = {"nes", "-nosound", (char *)rom_file};
    nofrendo_main(3, argv);
#endif
    
    Serial.println("Game finished, returning to menu...");
    delay(500);
  }
}

void loop() { delay(1000); }
