#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include "hw_config.h"
#include "tft_driver.h"
#include "turning.h"

#if ENABLE_SOUND
#include <driver/i2s_std.h>
#endif
#include <Preferences.h>

#include <XPT2046_Touchscreen.h>

#include <string.h>
#include <stdarg.h>

extern "C" {
#include "noftypes.h"
#include "bitmap.h"
#include "osd.h"
#include "nofrendo.h"
#include "nesinput.h"
#include "event.h"
#include "nofconfig.h"
#include "nes.h"
#include "nesstate.h"
}

#define NES_SCREEN_WIDTH 256
#define NES_SCREEN_HEIGHT 240
 
#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 240
 
static int AUDIO_SAMPLE_RATE = 22050;
#define TARGET_FRAME_MICROS 16666   
 
int master_volume = 5; 
Preferences prefs;
bool show_fps = false;
bool select_pressed = false;
static bool runtime_sound_enabled = false;
static int aspect_ratio_mode = 0; // 0: Original, 1: Fullscreen (320)
static int available_rates[] = {16000, 22050, 32000, 44100, 48000};
static int current_rate_idx = 1; // Default to 22050 (lower = less fuzz on ESP32)
static int dma_buf_size = 512;  // Larger buffer reduces underruns
static bool in_ingame_menu = false; // Flag to prevent vid_flush from drawing background

extern "C" void nes_togglepause();
extern "C" void main_quit();
// nes_t is now defined by including nes.h in the extern "C" block above
extern "C" nes_t *nes_getcontextptr(void);

extern TFTDriver tft;
extern "C" int nes_get_gamepad_state();

static uint16_t scale_color_565(uint16_t color, uint8_t alpha) {
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5) & 0x3F;
  uint8_t b = color & 0x1F;

  r = (r * alpha) / 255;
  g = (g * alpha) / 255;
  b = (b * alpha) / 255;

  return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t swap_rb_565(uint16_t color) {
  uint16_t r = (color >> 11) & 0x1F;
  uint16_t g = (color >> 5) & 0x3F;
  uint16_t b = color & 0x1F;
  return (uint16_t)((b << 11) | (g << 5) | r);
}

static void draw_magic(uint8_t alpha) {
  static uint16_t line[hakki_W];

  for (int y = 0; y < hakki_H; y++) {
    int row = y * hakki_W;
    for (int x = 0; x < hakki_W; x++) {
      uint16_t c = pgm_read_word(&hakki[row + x]);
      c = swap_rb_565(c);
      line[x] = scale_color_565(c, alpha);
    }
    tft.pushImage(0, y, hakki_W, 1, line);
  }
}

static void show_magic() {
  static bool shown = false;
  if (shown) {
    return;
  }
  shown = true;

  const int fade_in_ms = 300;
  const int hold_ms = 500;
  const int fade_out_ms = 500;
  const int steps = 30;
  const int step_delay_in = fade_in_ms / steps;
  const int step_delay_out = fade_out_ms / steps;

  tft.fillScreen(0x0000);
  for (int i = 0; i <= steps; i++) {
    uint8_t alpha = (uint8_t)((i * 255) / steps);
    draw_magic(alpha);
    delay(step_delay_in);
  }

  delay(hold_ms);

  for (int i = steps; i >= 0; i--) {
    uint8_t alpha = (uint8_t)((i * 255) / steps);
    draw_magic(alpha);
    delay(step_delay_out);
  }

  tft.fillScreen(0x0000);
}
 
static int16_t stereo_buffer[2048];

#if ENABLE_SOUND
static i2s_chan_handle_t tx_handle = NULL;
#endif
 
static uint16_t myPalette565[256];
static uint16_t myPalette565_swapped[256];
static bitmap_t *game_bitmap = NULL;
static bool video_ready = false;
static uint16_t *frame_buffer = NULL;  
static uint16_t *stretched_frame_buffer = NULL; // Full-frame buffer for stretching
static bool low_mem_video_mode = false;
static unsigned long last_display_update = 0; // Moved here to be accessible by menu
 
unsigned long frame_start_time = 0;
unsigned long frame_count = 0;
unsigned long last_fps_update = 0;
int current_fps = 0;
 
#define INP_JOYPAD0 0x0001
static nesinput_t joypad_p1;

#define HW_MASK_A 0x01
#define HW_MASK_B 0x02
#define HW_MASK_SELECT 0x04
#define HW_MASK_START 0x08
#define HW_MASK_UP 0x10
#define HW_MASK_DOWN 0x20
#define HW_MASK_LEFT 0x40
#define HW_MASK_RIGHT 0x80
#define HW_MASK_X     0x100
#define HW_MASK_Y     0x200

extern "C" int osd_init_sound(void);
static void show_ingame_menu();
static unsigned long last_menu_btn_time = 0;

#define INP_PAD_A 0x01
#define INP_PAD_B 0x02
#define INP_PAD_SELECT 0x04
#define INP_PAD_START 0x08
#define INP_PAD_UP 0x10
#define INP_PAD_DOWN 0x20
#define INP_PAD_LEFT 0x40
#define INP_PAD_RIGHT 0x80
 
esp_timer_handle_t nes_timer_handle = NULL;
void (*emu_timer_callback)(void) = NULL;

void IRAM_ATTR timer_callback_handler(void *arg) {
  if (emu_timer_callback) emu_timer_callback();
}
 
// extern char *global_rom_data; // Removed unused
// extern int global_rom_size;

extern "C" bool vid_preload_rom(const char *path);
extern "C" int osd_rom_open(const char *path);
extern "C" int osd_rom_read(void *dst, int len);
extern "C" void osd_rom_close(void);
 
#define MAX_GAMES 50
struct {
  char names[MAX_GAMES][128];
  int count;
  int selected;
} game_list = {{}, 0, 0};

int menu_volume = 100;
bool menu_brightness = true;
 
void scan_games() {
  File root = SD.open("/");
  game_list.count = 0;
  
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    
    if (!entry.isDirectory() && game_list.count < MAX_GAMES) {
      String name = entry.name();
      if (name.endsWith(".nes") || name.endsWith(".NES")) {
        strncpy(game_list.names[game_list.count], entry.name(), 127);
        game_list.names[game_list.count][127] = '\0';
        game_list.count++;
      }
    }
    entry.close();
  }
  root.close();
}
 
void draw_menu() {
  tft.fillScreen(0x0000);  
  delay(100);
   
  tft.drawFilledRect(0, 0, DISPLAY_WIDTH, 50, 0x001F);  // Blue instead of red for BGR display
  delay(50);
   
  Serial.println("[MENU] Drawing title...");
  tft.drawString(20, 10, "NES EMULATOR", 0xFFFF, 0x001F, 2);
   
  Serial.println("[MENU] Drawing game list...");
  int start_y = 70;
  for (int i = 0; i < game_list.count && i < 4; i++) {
    uint16_t color = (i == game_list.selected) ? 0x001F : 0xFFFF;  // Blue instead of red
    char display_name[128];
    strncpy(display_name, game_list.names[i], 127);
    display_name[127] = '\0';
     
    char *dot = strchr(display_name, '.');
    if (dot) *dot = '\0';
    
    Serial.printf("  [%d] %s\n", i, display_name);
    tft.drawString(20, start_y + (i * 30), display_name, color, 0x0000, 1);
  }
  
  Serial.println("[MENU] Drawing controls...");
  tft.drawString(10, 220, "UP/DOWN:Select  A:Play  SELECT:Settings", 0xFFFF, 0x0000, 1);
  delay(100);
}

void draw_settings() {
  tft.fillScreen(0x0000);
  delay(50);
   
  tft.drawFilledRect(0, 0, DISPLAY_WIDTH, 50, 0xF800); // Red background for settings
  
  tft.drawString(20, 10, "SETTINGS", 0xFFFF, 0xF800, 2);
  
  tft.drawString(20, 80, "Volume: ", 0xFFFF, 0x0000, 1);
  char vol_str[10];
  snprintf(vol_str, 10, "%d%%", menu_volume);
  tft.drawString(100, 80, vol_str, 0x07E0, 0x0000, 1);
  
  tft.drawString(10, 220, "UP/DOWN:Volume  A:Save  B:Back", 0xFFFF, 0x0000, 1);
}

// Touch initialization flag and objects
static bool touch_initialized = false;
static SPIClass *touchSpi = nullptr;
static XPT2046_Touchscreen *ts = nullptr;

extern "C" int show_menu() {
  Serial.println("\n[MENU] Starting menu system...");

#if ENABLE_TOUCH
  if (!touch_initialized) {
    touchSpi = new SPIClass(HSPI);
    touchSpi->begin(T_CLK, T_DO, T_DIN, T_CS);
    ts = new XPT2046_Touchscreen(T_CS, T_IRQ); 
    if (ts->begin(*touchSpi)) {
      ts->setRotation(1); 
      Serial.println("[MENU] Touch initialized with IRQ");
    } else {
      Serial.println("[MENU] Touch initialization failed");
    }
    touch_initialized = true;
  }
#endif

  show_magic();
  
  scan_games();
  Serial.printf("[MENU] Found %d games\n", game_list.count);
  
  if (game_list.count == 0) {
    Serial.println("[MENU] No games found!");
    tft.fillScreen(0x001F); // Blue screen for error
    delay(3000);
    return -1;
  }
  
  game_list.selected = 0;
  int top_index = 0; 
  bool in_menu = true;
  unsigned long last_input = 0;
  int last_selected = -1;
  
  bool needs_full_redraw = true;
  bool needs_list_redraw = false;

#if ENABLE_TOUCH
  const int ITEMS_PER_PAGE = 5;
#else
  const int ITEMS_PER_PAGE = 4;
#endif

  auto draw_menu_item = [&](int screen_idx, int game_idx, bool selected) {
    int y_pos = 50 + (screen_idx * 40);
    uint16_t bg = 0x0000; 
    uint16_t fg = selected ? 0x07E0 : 0xFFFF;

    
    char display_name[32];
    strncpy(display_name, game_list.names[game_idx], 31);
    display_name[31] = '\0';
    char *dot = strchr(display_name, '.');
    if (dot) *dot = '\0';

    tft.drawFilledRect(0, y_pos - 5, 250, 35, bg); // Draw 250px wide instead of DISPLAY_WIDTH to avoid clipping buttons
    tft.drawString(20, y_pos, display_name, fg, bg, 1);
  };
  
  while (in_menu) {
    if (needs_full_redraw) {
      tft.fillScreen(0x0000); 
      tft.drawString(106, 10, "GAME MENU", 0xFFFF, 0x0000, 2);
      
      for (int i = 0; i < ITEMS_PER_PAGE && (top_index + i) < game_list.count; i++) {
        draw_menu_item(i, top_index + i, (top_index + i) == game_list.selected);
      }
      
#if ENABLE_TOUCH
      // Draw touch scroll and settings buttons on the right side
      // Up Button
      tft.drawFilledRect(260, 50, 40, 40, 0x07E0); // Green box
      tft.drawString(268, 62, "UP", 0x0000, 0x07E0, 2); 
      
      // Down Button
      tft.drawFilledRect(260, 100, 40, 40, 0x07E0); // Green box
      tft.drawString(268, 112, "DN", 0x0000, 0x07E0, 2); 

      // Settings Button
      tft.drawFilledRect(260, 150, 40, 40, 0xF800); // Red box for SET
      tft.drawString(263, 162, "SET", 0xFFFF, 0xF800, 2); 
#else
      // Only show hardware button hints if touch is disabled!
      tft.drawString(10, DISPLAY_HEIGHT - 40, "UP/DN: Select", 0x07E0, 0x0000, 1);
      tft.drawString(10, DISPLAY_HEIGHT - 20, "A: Play  SEL: Settings", 0x07E0, 0x0000, 1);
#endif
      
      last_selected = game_list.selected;
      needs_full_redraw = false;
      needs_list_redraw = false;
    } 
    else if (needs_list_redraw) {
      tft.drawFilledRect(0, 45, 250, 200, 0x0000); 
      
      for (int i = 0; i < ITEMS_PER_PAGE && (top_index + i) < game_list.count; i++) {
        draw_menu_item(i, top_index + i, (top_index + i) == game_list.selected);
      }
      
      last_selected = game_list.selected;
      needs_list_redraw = false;
    } 
    else if (last_selected != game_list.selected) {
      if (last_selected >= top_index && last_selected < top_index + ITEMS_PER_PAGE) {
        draw_menu_item(last_selected - top_index, last_selected, false);
      }
      draw_menu_item(game_list.selected - top_index, game_list.selected, true);
      last_selected = game_list.selected;
    }
    
    int hw = nes_get_gamepad_state();
    
#if ENABLE_TOUCH
    // Handle Touch
    if (ts && ts->touched()) {
      TS_Point p = ts->getPoint();
      
      // Basic scaling to screen coords (inverted both ways by swapping max/min raw bounds)
      int tx = map(p.x, 3750, 190, 0, 320);
      int ty = map(p.y, 3750, 219, 0, 240);
      tx = min(max(tx, 0), 320);
      ty = min(max(ty, 0), 240);
      
      if (millis() - last_input > 250) { 
        if (tx > 240) {
          // Right side screen touched: buttons
          if (ty >= 50 && ty < 100) {
            // Tapped top-right (Up)
            if (top_index > 0) {
              top_index -= 1;
              if (game_list.selected >= top_index + ITEMS_PER_PAGE) game_list.selected = top_index + ITEMS_PER_PAGE - 1;
              needs_list_redraw = true;
            } else if (game_list.selected > 0) {
              game_list.selected--;
              needs_list_redraw = true;
            }
            last_input = millis();
          } else if (ty >= 100 && ty < 150) {
            // Tapped middle-right (Down)
            if (top_index + ITEMS_PER_PAGE < game_list.count) {
              top_index += 1;
              if (game_list.selected < top_index) game_list.selected = top_index;
              needs_list_redraw = true;
            } else if (game_list.selected < game_list.count - 1) {
              game_list.selected++;
              needs_list_redraw = true;
            }
            last_input = millis();
          } else if (ty >= 150 && ty < 200) {
            // Tapped bottom-right (Settings)
            hw |= HW_MASK_SELECT; // Magically trigger the settings menu code below!
            last_input = 0; // Force immediate processing!
          }
        } else {
          // Left side screen touched: game selection
          if (ty >= 45 && ty <= 240) {
            int list_idx = (ty - 45) / 40; 
            if (list_idx >= 0 && list_idx < ITEMS_PER_PAGE) {
               int touched_game_idx = top_index + list_idx;
               if (touched_game_idx < game_list.count) {
                 game_list.selected = touched_game_idx;
                 // Visual feedback of selection before proceeding
                 tft.fillScreen(0x0000);  
                 tft.drawString(60, 110, "LOADING...", 0xFFFF, 0x0000, 2);
                 delay(300);
                 return game_list.selected;
               }
            }
          }
        }
      }
    }
#endif
    
    if ((millis() - last_input) > 150) {
      if (hw & HW_MASK_UP) {
        if (game_list.selected > 0) {
          game_list.selected--;
          if (game_list.selected < top_index) {
            top_index = game_list.selected;
            needs_list_redraw = true; 
          }
          last_input = millis();
        }
      }
      if (hw & HW_MASK_DOWN) {
        if (game_list.selected < game_list.count - 1) {
          game_list.selected++;
          if (game_list.selected >= top_index + ITEMS_PER_PAGE) {
            top_index = game_list.selected - (ITEMS_PER_PAGE - 1);
            needs_list_redraw = true;
          }
          last_input = millis();
        }
      }
      if (hw & HW_MASK_A) {
        tft.fillScreen(0x0000);  
        delay(300);
        return game_list.selected;
      }
      if (hw & HW_MASK_SELECT) {
        bool in_settings = true;
        unsigned long settings_input = 0;
        int last_menu_volume = -1;
        bool settings_full_redraw = true;
        
        while (in_settings) {
          if (settings_full_redraw) {
            tft.fillScreen(0x0000);
            tft.drawString(70, 10, "VOLUME SETTINGS", 0xFFFF, 0x0000, 2);
            
#if ENABLE_TOUCH
            tft.drawFilledRect(120, 180, 80, 40, 0xF800); // Red SAVE button
            tft.drawString(135, 192, "SAVE", 0xFFFF, 0xF800, 2);
#else
            tft.drawString(10, DISPLAY_HEIGHT - 40, "UP/DN: Adjust", 0x07E0, 0x0000, 1);
            tft.drawString(10, DISPLAY_HEIGHT - 20, "A: Save  B: Cancel", 0x07E0, 0x0000, 1);
#endif
            settings_full_redraw = false;
            last_menu_volume = -1;
          }

          if (last_menu_volume != menu_volume) {
            char vol_str[20];
            sprintf(vol_str, "Vol: %3d%%", menu_volume);
            tft.drawFilledRect(100, 60, 120, 20, 0x0000);
            tft.drawString(106, 60, vol_str, 0x07E0, 0x0000, 2);

            int bar_width = (menu_volume * (DISPLAY_WIDTH - 40)) / 200;
            tft.drawFilledRect(20, 120, DISPLAY_WIDTH - 40, 30, 0x4208); 
            tft.drawFilledRect(20, 120, bar_width, 30, 0xF800);

            last_menu_volume = menu_volume;
          }
          
          int hw2 = nes_get_gamepad_state();
          
#if ENABLE_TOUCH
          if (ts && ts->touched()) {
            TS_Point p2 = ts->getPoint();
            int tx2 = map(p2.x, 3750, 190, 0, 320);
            int ty2 = map(p2.y, 3750, 219, 0, 240);
            tx2 = min(max(tx2, 0), 320);
            ty2 = min(max(ty2, 0), 240);
            
            if (millis() - settings_input > 50) {
              // Direct slider manipulation
              if (ty2 >= 100 && ty2 <= 170 && tx2 >= 20 && tx2 <= 300) {
                 int vol = map(tx2, 20, 300, 0, 200);
                 menu_volume = min(max(vol, 0), 200);
                 master_volume = menu_volume;
                 settings_input = millis();
              }
              // Normal UI tap checks below slider
              else if (ty2 >= 180 && ty2 <= 220 && (millis() - settings_input > 200)) {
                 if (tx2 >= 120 && tx2 <= 200) {
                    in_settings = false; // Save/exit
                 }
              }
            }
          }
#endif
          
          if ((millis() - settings_input) > 150) {
            if (hw2 & HW_MASK_UP) {
              menu_volume = min(200, menu_volume + 10);
              master_volume = menu_volume;
              settings_input = millis();
            }
            if (hw2 & HW_MASK_DOWN) {
              menu_volume = max(0, menu_volume - 10);
              master_volume = menu_volume;
              settings_input = millis();
            }
            if (hw2 & HW_MASK_A || hw2 & HW_MASK_B) {
              in_settings = false;
            }
          }
          delay(50);
        }
        last_input = millis();
        needs_full_redraw = true; 
      }
    }
    delay(20);
  }
  return -1;
}

extern "C" const char *get_selected_game() {
  static char game_path[256];
  if (game_list.selected < game_list.count) {
    snprintf(game_path, 256, "/%s", game_list.names[game_list.selected]);
    return game_path;
  }
  return "/mario.nes";  // Default fallback
}

static void show_ingame_menu() {
  nes_t *nes = nes_getcontextptr();
  if (!nes) return;
  
  bool was_paused = nes->pause;
  if (!was_paused) nes_togglepause(); // Pause emulation

#if ENABLE_SOUND
  if (tx_handle) i2s_channel_disable(tx_handle);
#endif

  if (nes_timer_handle) esp_timer_stop(nes_timer_handle);
  
  int selected_item = 0;
  int item_count = 9;
  bool in_menu = true;
  unsigned long last_input = millis();
  
  in_ingame_menu = true;
  bool needs_redraw = true;
  while (in_menu) {
    if (needs_redraw) {
      tft.drawFilledRect(40, 30, 241, 181, 0x0000);
      tft.drawRect(40, 30, 241, 181, 0x07E0); // Green border
      tft.drawString(85, 35, "IN-GAME MENU", 0xFFFF, 0x0000, 2);
       const char* items[] = {"Volume", "Sound", "Sample Rate", "Aspect Ratio", "Save Game", "Delete Save", "Resume", "Quit Game", "DMA Buf (X/Y)"};
       for (int i = 0; i < item_count; i++) {
           uint16_t color = (i == selected_item) ? 0x07E0 : 0xFFFF;
           tft.drawString(60, 65 + (i * 18), items[i], color, 0x0000, 1);
           
           if (i == 0) {
               char buf[16]; sprintf(buf, "< %d%% >", master_volume);
               tft.drawString(165, 65, buf, 0x07E0, 0x0000, 1);
           } else if (i == 1) {
               tft.drawString(185, 83, runtime_sound_enabled ? "ON" : "OFF", 0x07E0, 0x0000, 1);
           } else if (i == 2) {
               char buf[16]; sprintf(buf, "< %d >", available_rates[current_rate_idx]);
               tft.drawString(165, 101, buf, 0x07E0, 0x0000, 1);
           } else if (i == 3) {
               tft.drawString(185, 119, aspect_ratio_mode == 0 ? "Original" : "Full", 0x07E0, 0x0000, 1);
           } else if (i == 8) {
               char buf[16]; sprintf(buf, "%d", dma_buf_size);
               tft.drawString(185, 65 + (8 * 18), buf, 0x07E0, 0x0000, 1);
           }
       }
      needs_redraw = false;
    }
    
    int hw = nes_get_gamepad_state();
    if (millis() - last_input > 150) {
        if (hw & HW_MASK_UP) {
            selected_item = (selected_item + item_count - 1) % item_count;
            needs_redraw = true;
            last_input = millis();
        } else if (hw & HW_MASK_DOWN) {
            selected_item = (selected_item + 1) % item_count;
            needs_redraw = true;
            last_input = millis();
        } else if (hw & HW_MASK_LEFT) {
            if (selected_item == 0) { // Volume Down
                master_volume -= 1;
                if (master_volume < 0) master_volume = 0;
                menu_volume = master_volume; 
                prefs.putInt("volume", master_volume);
                // Brief burst for dynamic volume feedback
                if (nes->pause) {
#if ENABLE_SOUND
                    if (tx_handle) i2s_channel_enable(tx_handle);
#endif
                    nes_togglepause(); delay(100); nes_togglepause();
#if ENABLE_SOUND
                    if (tx_handle) i2s_channel_disable(tx_handle);
#endif
                }
            } else if (selected_item == 2) { // Sample Rate Down
                current_rate_idx = (current_rate_idx + 4) % 5;
                AUDIO_SAMPLE_RATE = available_rates[current_rate_idx];
                
                nes_t *machine = nes_getcontextptr();
                if (machine) {
                   apu_setparams(0, AUDIO_SAMPLE_RATE, 60, 16);
                }
#if ENABLE_SOUND
                if (tx_handle) {
                    i2s_channel_disable(tx_handle);
                    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE);
                    i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
                    i2s_channel_enable(tx_handle);
                }
#endif
            }
            needs_redraw = true;
            last_input = millis();
        } else if (hw & HW_MASK_RIGHT) {
            if (selected_item == 0) { // Volume Up
                master_volume += 1;
                if (master_volume > 200) master_volume = 200;
                menu_volume = master_volume; 
                prefs.putInt("volume", master_volume);
                // Brief burst for dynamic volume feedback
                if (nes->pause) {
#if ENABLE_SOUND
                    if (tx_handle) i2s_channel_enable(tx_handle);
#endif
                    nes_togglepause(); delay(100); nes_togglepause();
#if ENABLE_SOUND
                    if (tx_handle) i2s_channel_disable(tx_handle);
#endif
                }
            } else if (selected_item == 2) { // Sample Rate Up
                current_rate_idx = (current_rate_idx + 1) % 5;
                AUDIO_SAMPLE_RATE = available_rates[current_rate_idx];
                
                nes_t *machine = nes_getcontextptr();
                if (machine) {
                   apu_setparams(0, AUDIO_SAMPLE_RATE, 60, 16);
                }
#if ENABLE_SOUND
                if (tx_handle) {
                    i2s_channel_disable(tx_handle);
                    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE);
                    i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
                    i2s_channel_enable(tx_handle);
                }
#endif
            }
            needs_redraw = true;
            last_input = millis();
        } else if (hw & HW_MASK_A) {
            if (selected_item == 1) { // Sound Toggle
                runtime_sound_enabled = !runtime_sound_enabled;
                needs_redraw = true;
            } else if (selected_item == 3) { // Aspect Ratio
                aspect_ratio_mode = (aspect_ratio_mode + 1) % 2;
                tft.fillScreen(0x0000); 
                needs_redraw = true;
            } else if (selected_item == 4) { // Save Game
                state_save();
                tft.drawString(60, 220, "Game Saved!", 0x07E0, 0x0000, 1);
                delay(500);
                needs_redraw = true;
            } else if (selected_item == 5) { // Delete Save
                nes_t *machine = nes_getcontextptr();
                if (machine && machine->rominfo) {
                   char fn[256];
                   const char *rom_path = machine->rominfo->filename;
                   const char *p = strrchr(rom_path, '/');
                   if (p) p++; else p = rom_path;
                   
                   sprintf(fn, "/sd/Saved/%s", p);
                   char *dot = strrchr(fn, '.');
                   if (dot) strcpy(dot, ".ss0");
                   else strcat(fn, ".ss0");

                   if (SD.remove(fn) || SD.remove(fn + 3)) { 
                      tft.drawString(60, 220, "Save Deleted!", 0xF800, 0x0000, 1);
                   } else {
                      tft.drawString(60, 220, "No Save Found", 0xFFFF, 0x0000, 1);
                   }
                   delay(500);
                   needs_redraw = true;
                }
            } else if (selected_item == 6) { // Resume
                in_menu = false;
            } else if (selected_item == 7) { // Quit
                main_quit();
                in_menu = false;
            }
        } else if (hw & HW_MASK_X) {
            Serial.println("X Pressed - Increasing Buffer");
            dma_buf_size += 32;
            if (dma_buf_size > 1024) dma_buf_size = 1024;
            osd_init_sound(); 
            needs_redraw = true;
            last_input = millis();
        } else if (hw & HW_MASK_Y) {
            Serial.println("Y Pressed - Decreasing Buffer");
            dma_buf_size -= 32;
            if (dma_buf_size < 32) dma_buf_size = 32;
            osd_init_sound(); 
            needs_redraw = true;
            last_input = millis();
        } else if (hw & (HW_MASK_START | HW_MASK_B)) {
            in_menu = false;
        }
        last_input = millis();
    }
    delay(20);
  }
  
  // Wait for all buttons to be released before returning
  // This prevents START from immediately re-triggering the menu
  while (nes_get_gamepad_state() & (HW_MASK_START | HW_MASK_SELECT | HW_MASK_B)) {
      delay(20);
  }
  delay(100); // Extra debounce
  
  if (!was_paused) nes_togglepause(); // Resume emulation
  
#if ENABLE_SOUND
  if (tx_handle && (!nes || !nes->poweroff)) {
      i2s_channel_enable(tx_handle);
  }
#endif

  if (nes_timer_handle) esp_timer_start_periodic(nes_timer_handle, 1000000 / 60);

  in_ingame_menu = false;
  tft.fillScreen(0x0000); 
  last_display_update = 0; // Force immediate frame refresh
  last_menu_btn_time = millis();
}

/* Removed unused ROM preload logic to save memory */

extern "C" int osd_init_sound(void) {
  if (!runtime_sound_enabled) {
    return 0;
  }
#if ENABLE_SOUND
  if (tx_handle) {
      i2s_channel_disable(tx_handle);
      i2s_del_channel(tx_handle);
      tx_handle = NULL;
  }


  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 6;     // More DMA buffers = smoother playback
  chan_cfg.dma_frame_num = dma_buf_size; 
  i2s_new_channel(&chan_cfg, &tx_handle, NULL);

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = (gpio_num_t)I2S_BCK,
          .ws   = (gpio_num_t)I2S_WS,
          .dout = (gpio_num_t)I2S_DO,
          .din  = I2S_GPIO_UNUSED,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv   = false,
          },
      },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  i2s_channel_init_std_mode(tx_handle, &std_cfg);
  i2s_channel_enable(tx_handle);

  Serial.printf("I2S init: rate=%d dma_bufs=6 frame_size=%d\n", AUDIO_SAMPLE_RATE, dma_buf_size);
  return 0;
#else
  Serial.println("Sound disabled in configuration");
  return 0;
#endif
}

extern "C" void osd_getsoundinfo(sndinfo_t *info) {
  info->sample_rate = AUDIO_SAMPLE_RATE;
  info->bps = 16;
}

extern "C" void osd_pushaudio(int16_t *buffer, int samples) {
  if (!runtime_sound_enabled || !buffer) return;

#if ENABLE_SOUND
  if (tx_handle) {
    // master_volume is 0-100. Translate to fraction.
    float volume = master_volume * 0.01f;
    if (samples > 1024) samples = 1024;

    for (int i = 0; i < samples; i++) {
        // buffer comes in interleaved stereo (L, R)
        int16_t left = (int16_t)(buffer[i * 2] * volume);
        int16_t right = (int16_t)(buffer[i * 2 + 1] * volume);
        
        stereo_buffer[i * 2] = left;
        stereo_buffer[i * 2 + 1] = right;
    }

    size_t written;
    // Bounded timeout prevents emulator hang if I2S queue is full
    i2s_channel_write(tx_handle, stereo_buffer, samples * 4, &written, pdMS_TO_TICKS(12));
  }
#endif
}

extern "C" void osd_stopsound(void) {
  if (!runtime_sound_enabled) return;
#if ENABLE_SOUND
  if (tx_handle) {
      size_t written;
      int16_t silence[64] = {0};
      i2s_channel_write(tx_handle, silence, sizeof(silence), &written, 0);
      i2s_channel_disable(tx_handle);
  }
#endif
}

extern "C" void osd_writesound(void *stream, int len) {}

extern "C" void osd_initvideo(int *lines) {
  *lines = NES_SCREEN_HEIGHT;
  Serial.printf("osd_initvideo called, lines=%d\n", *lines);
}

extern "C" void osd_shutdownvideo() {}

extern "C" void osd_setscreen(int x, int y, int width, int height) {}

extern "C" void osd_setpalette(rgb_t *pal) {
  if (!pal) return;
  for (int i = 0; i < 256; i++) {
    uint16_t c = tft.color565(pal[i].b, pal[i].g, pal[i].r);  // BGR order for BGR display
    myPalette565[i] = c;
    myPalette565_swapped[i] = c;
  }
}

extern "C" int nes_get_gamepad_state();

extern "C" void osd_getinput(void) {
  int hw = nes_get_gamepad_state();
  int nes_data = 0;

  if (hw & HW_MASK_A) nes_data |= INP_PAD_A;
  if (hw & HW_MASK_B) nes_data |= INP_PAD_B;
  if (hw & HW_MASK_SELECT) nes_data |= INP_PAD_SELECT;
  if (hw & HW_MASK_START) nes_data |= INP_PAD_START;
  if (hw & HW_MASK_UP) nes_data |= INP_PAD_UP;
  if (hw & HW_MASK_DOWN) nes_data |= INP_PAD_DOWN;
  if (hw & HW_MASK_LEFT) nes_data |= INP_PAD_LEFT;
  if (hw & HW_MASK_RIGHT) nes_data |= INP_PAD_RIGHT;

  static uint8_t start_history = 0;
  static uint8_t select_history = 0;

  int current_start = (hw & HW_MASK_START) ? 1 : 0;
  int current_select = (hw & HW_MASK_SELECT) ? 1 : 0;

  // Detect Start + Select combo BEFORE submitting to the NES emulator
  if (current_start && current_select) {
      start_history = 0;
      select_history = 0;
      if (millis() - last_menu_btn_time > 1000) {
          show_ingame_menu();
      }
  } else {
      // Shift history left and add current state (keep 4 bits)
      start_history = ((start_history << 1) | current_start) & 0x0F;
      select_history = ((select_history << 1) | current_select) & 0x0F;
      
      // Delay applying the button to the game by 4 frames (approx 66ms).
      // This gives the user time to press the second button for the menu combo
      // without the NES game accidentally seeing a lone START press and pausing itself!
      if (start_history & 0x08) nes_data |= INP_PAD_START;
      if (select_history & 0x08) nes_data |= INP_PAD_SELECT;
  }

  joypad_p1.data = nes_data;
}

extern "C" int osd_init() {
  Serial.println("[OSD] Initializing...");
  Serial.printf("[OSD] Heap=%u, SPIRAM=%u\n", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  
  Serial.println("[OSD] Initializing joypad...");
  joypad_p1.type = INP_JOYPAD0;
  joypad_p1.data = 0;
  
  input_register(&joypad_p1);
  Serial.println("[OSD] Input registered successfully");
  
  // Load saved settings
  prefs.begin("nes_emu", false);
  master_volume = prefs.getInt("volume", 50);
  menu_volume = master_volume;
  Serial.printf("[OSD] Loaded volume: %d\n", master_volume);

  // Enable APU lowpass filter to reduce fuzz/alias noise
  extern void apu_setfilter(int filter_type);
  apu_setfilter(2); // APU_FILTER_WEIGHTED = 2 (smoothest)

  if (osd_init_sound() != 0) { 
    Serial.println("[OSD] Sound Init FAILED"); 
  }
  
  Serial.printf("[OSD] Post-init Heap=%u, SPIRAM=%u\n", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.println("[OSD] Initialization complete");
  return 0;
}

extern "C" void osd_shutdown() {
  if (nes_timer_handle) {
    esp_timer_stop(nes_timer_handle);
    esp_timer_delete(nes_timer_handle);
    nes_timer_handle = NULL;
  }
  
  // Free video buffers to prevent memory leaks on game reload
  if (frame_buffer) {
    free(frame_buffer);
    frame_buffer = NULL;
  }
  if (stretched_frame_buffer) {
    free(stretched_frame_buffer);
    stretched_frame_buffer = NULL;
  }
  
  // Reset other state
  video_ready = false;
  low_mem_video_mode = false;
}

extern "C" int osd_installtimer(int freq, void *func, int func_param, void *func2, int func2_param) {
  emu_timer_callback = (void (*)(void))func;
  if (nes_timer_handle) {
      esp_timer_stop(nes_timer_handle);
      esp_timer_delete(nes_timer_handle);
      nes_timer_handle = NULL;
  }
  const esp_timer_create_args_t timer_args = { .callback = &timer_callback_handler, .name = "nes_timer" };
  esp_timer_create(&timer_args, &nes_timer_handle);
  esp_timer_start_periodic(nes_timer_handle, 1000000 / freq);
  return 0;
}

extern "C" int osd_gettime(void) {
  return millis();
}

extern "C" int osd_makesnapname(char *buf, int len) {
  return 0;
}

extern "C" void osd_getmouse(int *x, int *y, int *button) {
  *x = 0;
  *y = 0;
  *button = 0;
}

/* Removed unused vid_preload_rom */
static File rom_stream_file;

extern "C" void IRAM_ATTR osd_blit(bitmap_t *bmp) {
  if (!video_ready || !bmp || !bmp->line || !bmp->line[0] || !frame_buffer) {
    return;
  }
  
  for (int y = 0; y < NES_SCREEN_HEIGHT; y++) {
    uint8_t *src = bmp->line[y];
    uint16_t *dst = &frame_buffer[y * NES_SCREEN_WIDTH];
    
    for (int x = 0; x < NES_SCREEN_WIDTH; x++) {
      dst[x] = myPalette565_swapped[src[x]];
    }
  }
}

extern "C" void vid_flush() {
  if (in_ingame_menu) return; // DON'T draw NES screen while menu is open

  static bool tried_frame_buffer_alloc[2] = {false, false}; 
  static uint16_t line_buffer[NES_SCREEN_WIDTH];
  
  frame_start_time = micros();

  if (!frame_buffer) {
    frame_buffer = (uint16_t *)heap_caps_malloc(NES_SCREEN_WIDTH * NES_SCREEN_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame_buffer) {
      frame_buffer = (uint16_t *)malloc(NES_SCREEN_WIDTH * NES_SCREEN_HEIGHT * sizeof(uint16_t));
    }
    if (!frame_buffer) {
      low_mem_video_mode = true;
      Serial.println("Low-memory video mode enabled (line-by-line rendering)");
    }
  }

  if (aspect_ratio_mode == 1 && !stretched_frame_buffer) {
    stretched_frame_buffer = (uint16_t *)heap_caps_malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }

  if (frame_buffer) {
    osd_blit(game_bitmap);
  }

  unsigned long now = micros();
  if (now - last_display_update > 16666) {
    unsigned long t1 = micros();
    
    if (aspect_ratio_mode == 1 && stretched_frame_buffer && frame_buffer) {
       // Fullscreen Stretched (320x240) - Optimized with single pushImage
       for (int y = 0; y < NES_SCREEN_HEIGHT; y++) {
          uint16_t *src = &frame_buffer[y * NES_SCREEN_WIDTH];
          uint16_t *dst = &stretched_frame_buffer[y * DISPLAY_WIDTH];
          for (int i = 0; i < 64; i++) {
             dst[i * 5 + 0] = src[i * 4 + 0];
             dst[i * 5 + 1] = src[i * 4 + 1];
             dst[i * 5 + 2] = src[i * 4 + 1]; // Duplicate
             dst[i * 5 + 3] = src[i * 4 + 2];
             dst[i * 5 + 4] = src[i * 4 + 3];
          }
       }
       tft.pushImage(0, 0, 320, 240, stretched_frame_buffer, true);
    } else {
       // Original Centered (256x240)
       int x_offset = (DISPLAY_WIDTH - NES_SCREEN_WIDTH) / 2;
       if (frame_buffer) {
         tft.pushImage(x_offset, 0, NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, frame_buffer, true);
       } else if (low_mem_video_mode && game_bitmap && game_bitmap->line && game_bitmap->line[0]) {
         for (int y = 0; y < NES_SCREEN_HEIGHT; y++) {
           uint8_t *src = game_bitmap->line[y];
           for (int x = 0; x < NES_SCREEN_WIDTH; x++) {
             line_buffer[x] = myPalette565_swapped[src[x]];
           }
           tft.pushImage(x_offset, y, NES_SCREEN_WIDTH, 1, line_buffer, true);
         }
       }
    }
    
    unsigned long push_t = micros() - t1;
    last_display_update = now;
    
    static unsigned long last_print = 0;
    if (millis() - last_print > 3000) {
      Serial.printf("push=%luus emul_fps=%d\n", push_t, current_fps);
      last_print = millis();
    }

  }

  // Audio is now generated and submitted completely independently of vid_flush
  // in nes_emulate() via osd_pushaudio()

  frame_count++;
  if (millis() - last_fps_update >= 1000) {
    current_fps = frame_count;
    frame_count = 0;
    last_fps_update = millis();
  }
}

extern "C" int vid_init(int width, int height, viddriver_t *osd_driver) {
  if (!game_bitmap) game_bitmap = bmp_create(NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, 8); 
  if (game_bitmap && game_bitmap->data) {
     memset(game_bitmap->data, 0, NES_SCREEN_WIDTH * NES_SCREEN_HEIGHT);
  }
  // DO NOT set frame_buffer = NULL here, as it may already be allocated or handled in vid_flush
  video_ready = (game_bitmap && game_bitmap->data);
  Serial.printf("vid_init heap snapshot: heap=%u spiram=%u\n", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("vid_init: bitmap %p, frame_buffer %p, ready %d, deferred_fb_alloc=1\n", game_bitmap, frame_buffer, video_ready);
  return video_ready ? 0 : -1;
}

extern "C" int osd_rom_open(const char *path) {
  if (rom_stream_file) {
    rom_stream_file.close();
  }

  Serial.printf("[ROM] Streaming open: %s\n", path ? path : "(null)");
  rom_stream_file = SD.open(path, FILE_READ);
  if (!rom_stream_file) {
    Serial.println("[ROM] Streaming open failed");
    return -1;
  }
  return 0;
}

extern "C" int osd_rom_read(void *dst, int len) {
  if (!rom_stream_file || !dst || len <= 0) {
    return -1;
  }
  int n = rom_stream_file.read((uint8_t *)dst, len);
  return n;
}

extern "C" void osd_rom_close(void) {
  if (rom_stream_file) {
    rom_stream_file.close();
  }
}

extern "C" void vid_shutdown() {
  video_ready = false;
}

extern "C" int vid_setmode(int width, int height) {
  return 0;
}

extern "C" void vid_setpalette(rgb_t *pal) {
  osd_setpalette(pal);
}

extern "C" bitmap_t *vid_getbuffer() {
  if (!game_bitmap) game_bitmap = bmp_create(NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, 0);
  video_ready = (game_bitmap && game_bitmap->data);
  return game_bitmap;
}

extern "C" void osd_getvideoinfo(vidinfo_t *info) {
  info->default_width = NES_SCREEN_WIDTH;
  info->default_height = NES_SCREEN_HEIGHT;
  info->driver = 0;
}

extern "C" void osd_togglefullscreen(int code) {}

extern "C" char *osd_newextension(char *string, char *ext) {
  char *p = strrchr(string, '.');
  if (p) {
    strcpy(p, ext);
  } else {
    strcat(string, ext);
  }
  return string;
}

extern "C" void osd_fullname(char *fullname, const char *shortname) {
  strcpy(fullname, shortname);
}

extern "C" int osd_main(int argc, char *argv[]) {
  runtime_sound_enabled = false;
  for (int i = 1; i < argc; i++) {
    if (argv[i] && strcmp(argv[i], "-sound") == 0) {
      runtime_sound_enabled = true;
    }
    if (argv[i] && strcmp(argv[i], "-nosound") == 0) {
      runtime_sound_enabled = false;
    }
  }

  char *rom_name = "rom";
  if (argc > 0) rom_name = argv[argc - 1];
  return main_loop(rom_name, system_nes);
}

extern "C" int nofrendo_log_init(void) {
  return 0;
}

extern "C" void nofrendo_log_shutdown(void) {}

extern "C" int nofrendo_log_print(const char *s) {
  if (s) {
    Serial.print(s);
  }
  return 0;
}

extern "C" int nofrendo_log_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  int written = Serial.vprintf(format, args);
  va_end(args);
  return written;
}

extern "C" void nofrendo_log_assert(int expr, int line, const char *file, char *msg) {
  if (!expr) {
    Serial.printf("ASSERT FAILED: %s:%d %s\n", file ? file : "?", line, msg ? msg : "");
  }
}
