// Newsheen / Pusheen puck (esp32_base_puck_v2) — pin map for the MAX98357A speaker demo.
//
// J3 "SENSOR INTERFACE" is a 1x07 2.54 mm socket. Netlist-verified pin order
// (J3.1 is the 3V3 end, nearest the board edge; silkscreen reads the other way):
//
//   J3.7 = GPIO35 (SDA)   <- silk "35_SDA"   top of the header
//   J3.6 = GPIO36 (SCL)   <- silk "36_SCL"
//   J3.5 = GPIO37 (I2S WS)<- silk "37_WS"
//   J3.4 = GPIO39 (I2S SD)<- silk "39_SD"
//   J3.3 = GPIO38 (I2S SCK)<- silk "38_SCK"
//   J3.2 = GND
//   J3.1 = +3V3
//
// The generic purple MAX98357A breakout is also 7 pins, in this order:
//   LRC, BCLK, DIN, GAIN, SD, GND, VIN
//
// Lay the two headers side by side in silkscreen order and GND lands on GND and
// VIN lands on 3V3 — no crossed wires. That is NEWSHEEN_WIRING_STRAIGHT, and it
// costs nothing because the ESP32-S3 GPIO matrix will route I2S to any pin.
//
//   35_SDA -- LRC      36_SCL -- BCLK     37_WS -- DIN
//   39_SD  -- GAIN     38_SCK -- SD       GND -- GND      3V3 -- VIN
//
// NEWSHEEN_WIRING_CLASSIC instead uses the pins the silkscreen names, for anyone
// wiring flying leads and reading the labels literally:
//
//   37_WS -- LRC       38_SCK -- BCLK     39_SD -- DIN
//   GND -- GND         3V3 -- VIN        GAIN + SD left to the breakout's defaults
//
// Both are safe on the shipping N16R2 module (16 MB quad flash / 2 MB QUAD PSRAM),
// which leaves GPIO33-37 free. An N16R8 (OCTAL PSRAM) would eat GPIO33-37 and kill
// this header — always `esptool flash-id` before trusting a board.

#pragma once

// ---------------------------------------------------------------- fixed pins
#define PIN_NEOPIXEL   16   // -> U5 SN74LVC1T45 -> 8x WS2812B (needs the DIR bodge)
#define NUM_PIXELS      8
#define PIN_BUTTON     17   // SW3, active low, 10K pull-up + hardware debounce
#define PIN_BOOT_BUTTON 0   // SW2 strapping pin; also driven by USB-CDC DTR
#define PIN_DEBUG_LED  48   // D14, plain LED + 330R, active high

// ------------------------------------------------------------- I2S + amp pins
#if defined(NEWSHEEN_WIRING_CLASSIC)

  #define PIN_I2S_LRC   37   // silk 37_WS  -> LRC
  #define PIN_I2S_BCLK  38   // silk 38_SCK -> BCLK
  #define PIN_I2S_DIN   39   // silk 39_SD  -> DIN
  #define PIN_AMP_SD    -1   // not wired: breakout's own pull-up enables the amp
  #define PIN_AMP_GAIN  -1   // not wired: floating = 9 dB
  #define WIRING_NAME   "CLASSIC (37/38/39)"

#else  // NEWSHEEN_WIRING_STRAIGHT (default)

  #define PIN_I2S_LRC   35   // silk 35_SDA -> LRC
  #define PIN_I2S_BCLK  36   // silk 36_SCL -> BCLK
  #define PIN_I2S_DIN   37   // silk 37_WS  -> DIN
  #define PIN_AMP_SD    38   // silk 38_SCK -> SD   (drive HIGH to enable, LOW to mute)
  #define PIN_AMP_GAIN  39   // silk 39_SD  -> GAIN (see setGain(); GPIO39 is also JTAG MTCK)
  #define WIRING_NAME   "STRAIGHT (35/36/37 + SD/GAIN)"

#endif

// GPIO35 and GPIO36 carry 5.1K pull-ups to 3V3 (they are the I2C pads). Harmless
// for push-pull I2S clock/data outputs — about 0.65 mA each when driven low.
