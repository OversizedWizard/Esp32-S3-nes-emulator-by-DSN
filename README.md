# ESP32-S3 NES Emulator (Optimised for china 2.8 320x240 ST7789 driver)

This repo based on esp nofrendo. But I really too much work and upgraded it
Original Repo : https://github.com/espressif/esp32-nesemu/tree/master/components/nofrendo

[![Platform: ESP32](https://img.shields.io/badge/Platform-ESP32--S3-blue)](https://www.espressif.com/)

A high-performance, DIY handheld NES (Nintendo Entertainment System) emulator powered by the ESP32-S3 microcontroller. This project features high-quality audio via I2S, smooth rendering on an ST7789 display, and games loaded directly from an SD card.

## 📺 Video Tutorial

Build your own following the step-by-step guide! https://youtu.be/wruJ-BESnX8

[![Watch the tutorial](https://img.youtube.com/vi/wruJ-BESnX8/0.jpg)](https://www.youtube.com/watch?v=wruJ-BESnX8)

> **Click the image above to watch the full tutorial on YouTube.**

## ✨ Features

* **Full Speed Emulation:** Runs NES games smoothly thanks to the ESP32-S3's dual-core power.
* **High-Quality Audio:** Crystal clear game sounds using the MAX98357A I2S amplifier.
* **Storage:** Load hundreds of ROMs from a Micro SD Card.
* **Display:** Vibrant colors using the ST7789 IPS TFT display.
* **Controls:** Full 8-button support (D-Pad + A/B + Start/Select) plus optional X/Y.

## 🛠️ Hardware Requirements

To build this project, you will need the following components:

* **Microcontroller:** ESP32-S3 (DevKit or bare module)
* **Display:** ST7789 SPI TFT Module (e.g., 2.8")
* **Audio:** MAX98357A I2S Amplifier Module + 3W 4Ω Speaker
* **Storage:** Micro SD Card Reader Module + Micro SD Card (formatted FAT32)
* **Controls:** Tactile Push Buttons (6x6mm)
* **Power:** LiPo Battery & Charging Circuit (Optional/TP4056)
* **Wires & Perfboard:** For connections.

## 🔌 Wiring & Pinout

Below is the connection diagram for the components. 

> **⚠️ NOTE:** These pins have been updated for this specific custom layout. Display and SD Card share the SPI SCK and MOSI lines.

### 1. ST7789 Display (SPI)
| Display Pin | ESP32-S3 GPIO | Function |
| :--- | :--- | :--- |
| VCC | 3.3V | Power |
| GND | GND | Ground |
| SCK | GPIO **[15]** | SPI Clock (Shared) |
| MOSI | GPIO **[7]** | SPI MOSI (Shared) |
| CS | GPIO **[4]** | Chip Select |
| DC | GPIO **[6]** | Data/Command |
| RST | GPIO **[5]** | Reset |
| BL | GPIO **[16]** | Backlight |

### 2. Micro SD Card Module (SPI)
| SD Module Pin | ESP32-S3 GPIO | Function |
| :--- | :--- | :--- |
| CS | GPIO **[18]** | Chip Select |
| SCK | GPIO **[15]** | Shared with Display |
| MOSI | GPIO **[7]** | Shared with Display |
| MISO | GPIO **[17]** | SPI MISO |
| VCC | 3.3V | Power |
| GND | GND | Ground |

### 3. MAX98357A Audio (I2S)
| Amp Pin | ESP32-S3 GPIO | Function |
| :--- | :--- | :--- |
| LRC (WS) | GPIO **[14]** | Left/Right Clock / Word Select |
| BCLK | GPIO **[10]** | Bit Clock |
| DIN (DO) | GPIO **[9]** | Data In |
| GAIN | GPIO **[3]** | Gain Control |
| SD | GPIO **[8]** | Shutdown |
| VCC | 5V / 3.3V | Power |
| GND | GND | Ground |

### 4. Controller Buttons
| Button | ESP32-S3 GPIO |
| :--- | :--- |
| UP | GPIO **[44]** |
| DOWN | GPIO **[1]** |
| LEFT | GPIO **[43]** |
| RIGHT | GPIO **[2]** |
| A | GPIO **[40]** |
| B | GPIO **[39]** |
| X | GPIO **[47]** |
| Y | GPIO **[21]** |
| START | GPIO **[41]** |
| SELECT | GPIO **[42]** |

*(Connect the other side of all buttons to GND)*

### 5. Touch Controller (SPI)
| Touch Pin | ESP32-S3 GPIO |
| :--- | :--- |
| CLK | GPIO **[11]** |
| CS | GPIO **[12]** |
| DIN | GPIO **[13]** |
| DO | GPIO **[20]** |
| IRQ | GPIO **[45]** |

![Wiring](img/wiring.png)

## 💾 SD Card Setup

1.  Format your Micro SD card to **FAT32**.
2.  Copy your `.nes` game files into root folder.
3.  Insert the SD card into the module.

## 🚀 Installation & Setup

1.  **Clone the Repository:**
    ```bash
    git clone [https://github.com/derdacavga/Esp32-S3-nes-emulator-by-DSN.git](https://github.com/derdacavga/Esp32-S3-nes-emulator-by-DSN.git)
    ```
2.  **Open in IDE:**
    * Open the project using **Arduino IDE**.
3.  **Install Libraries:**
    * Ensure you have installed the necessary libraries (e.g., `Audio`, etc.) `library.properties`.
4.  **Configure:**
    * Check the pin definitions in the code to match your wiring.
5.  **Flash:**
    * Connect your ESP32-S3 via USB.
    * Select the correct Board and Port.
    * Upload the code.

## 🎮 Controls

| Button | Action |
| :--- | :--- |
| **D-Pad** | Navigate Menu / Move Character |
| **A** | Confirm / Jump |
| **B** | Back / Attack |
| **Start** | Pause Game |
| **Select** | Game Mode / Menu |

---

## 🤝 Support

If you found this project helpful, please consider:
* **Subscribing** to the YouTube Channel.
* Giving the video a **Like**.
* Starring this GitHub Repository!

* **YouTube:** https://www.youtube.com/@DsnIndustries/videos
* **Patreon:** https://www.patreon.com/c/dsnIndustries

Happy Making!

## Games
* **Maze Escape:** https://play.google.com/store/apps/details?id=com.DsnMechanics.MazeEscape
* **Air Hockey:** https://play.google.com/store/apps/details?id=com.DsnMechanics.AirHockey
* **Click Challenge:** https://play.google.com/store/apps/details?id=com.DsNMechanics.ClickChallenge
* **Flying Triangels:** https://play.google.com/store/apps/details?id=com.DsnMechanics.Triangle
* **SkyScrapper:** https://play.google.com/store/apps/details?id=com.DsnMechanics.SkyScraper
