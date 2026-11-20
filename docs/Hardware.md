# LILYGO T-Embed ESP32-S3 Pinout

**Board:** LILYGO T-Embed ESP32-S3 
**Display:** 1.9 inch 320x170 ST7789 TFT LCD 
**MCU:** ESP32-S3 16MB Flash 8MB Psram

## 1. ST7789 TFT LCD (1.9" 320x170)

| Signal   | GPIO |
| -------- | ---- |
| LCD_BL   | IO15 |
| LCD_DC   | IO13 |
| LCD_CS   | IO10 |
| LCD_CLK  | IO12 |
| LCD_MOSI | IO11 |
| LCD_RES  | IO09 |

## 2. Microphone (ES7210)

| Signal  | GPIO |
| ------- | ---- |
| ES_BCLK | IO47 |
| ES_LRCK | IO21 |
| ES_DIN  | IO14 |
| ES_MCLK | IO48 |

## 3. Speaker (I2S)

| Signal   | GPIO |
| -------- | ---- |
| I2S_BCLK | IO07 |
| I2S_WCLK | IO05 |
| I2S_DOUT | IO06 |

## 4. RGB LED (APA102)

| Signal     | GPIO |
| ---------- | ---- |
| APA102_CLK | IO45 |
| APA102_DI  | IO42 |

## 5. Rotary Encoder

| Signal         | GPIO        |
| -------------- | ----------- |
| Encoder_A      | IO02        |
| Encoder_B      | IO01        |
| Encoder_Button | IO00 (BOOT) |

## 6. TF Card Slot (SD Card)

| Signal  | GPIO |
| ------- | ---- |
| SD_CS   | IO39 |
| SD_SCLK | IO40 |
| SD_MOSI | IO41 |
| SD_MISO | IO38 |

## 7. GPIO Pinout Header

| Pin Label | GPIO / Function |
| --------- | --------------- |
| SDA       | IO16            |
| SCL       | IO17            |
| -         | GND             |
| -         | 3V3             |
| -         | IO40            |
| -         | GND             |
| -         | GND             |
| -         | IO38            |
| -         | IO41            |

## 8. Other Interfaces & Pins

| Name            | GPIO / Notes               |
| --------------- | -------------------------- |
| Power_On        | IO46                       |
| Battery Voltage | IO04                       |
| Grove Connector | Near IO44, IO43 (GND, 3V3) |
| JST-GH 1.25mm   | Expansion connector        |
| Speaker Slot    | JST 1.25mm                 |
| RESET           | Reset button               |
| USB Type-C      | Power & Data               |

---

**Notes:**
- Encoder button shares the BOOT pin (IO00).
- Some SD card pins (IO38/IO40/IO41) are also brought out to the GPIO header.
- Always verify against the latest official schematic before hardware modifications.