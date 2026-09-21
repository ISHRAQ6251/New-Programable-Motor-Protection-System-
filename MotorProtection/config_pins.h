#pragma once

#include <stdint.h>

// ESP32-S3-N16R8 — all GPIO numbers live in this file only.
// Avoided: 0/3/45/46 (strapping), 19/20 (USB-JTAG), 43/44 (UART0 Serial).

// ACS712-30A current-sense inputs — ADC1 only (Wi-Fi makes ADC2 unreliable).
static const int PIN_ISENSE[8] = {1, 2, 4, 5, 6, 7, 8, 9};

// Relay coil drives. Default polarity is active-HIGH; boot LOW = de-energized.
static const int PIN_RELAY[8] = {15, 16, 17, 18, 38, 39, 40, 41};

// RoboticsBD 3.3 V Micro-SD breakout, SPI mode (not SDIO).
static const int PIN_SD_MOSI = 11;
static const int PIN_SD_MISO = 13;
static const int PIN_SD_SCK  = 12;
static const int PIN_SD_CS   = 10;

// Passive buzzer, LEDC PWM.
static const int PIN_BUZZER = 21;

#ifndef PIN_I2C_SDA
#define PIN_I2C_SDA 14
#endif
#ifndef PIN_I2C_SCL
#define PIN_I2C_SCL 42
#endif
#ifndef ADS1115_ADDR_A
#define ADS1115_ADDR_A 0x48
#endif
#ifndef ADS1115_ADDR_B
#define ADS1115_ADDR_B 0x49
#endif

// Local panel: KY-040 rotary encoder. The module carries its own pull-ups on
// all three lines, so plain INPUT (no internal pull-up, no external resistors).
static const int ENC_A_PIN  = 22;  // CLK
static const int ENC_B_PIN  = 23;  // DT
static const int ENC_SW_PIN = 24;  // SW (active-LOW when pressed)

// Local panel: SH1106 128x64 OLED on a SECOND, independent I2C bus (Wire1).
// Physically separate from the ADS1115 bus (GPIO 14/42), so no mutex is shared.
#ifndef OLED_SDA_PIN
#define OLED_SDA_PIN 25
#endif
#ifndef OLED_SCL_PIN
#define OLED_SCL_PIN 47
#endif
#ifndef OLED_I2C_ADDR
#define OLED_I2C_ADDR 0x3C
#endif
#ifndef OLED_I2C_HZ
#define OLED_I2C_HZ 400000
#endif

// Analog path: ACS712-30A 66 mV/A, 10k/15k divider (x0.6) => 39.6 mV/A at ADC.
static const float ACS712_MV_PER_AMP = 66.0f;
static const float DIVIDER_RATIO     = 0.6f;
static const float MV_PER_AMP        = ACS712_MV_PER_AMP * DIVIDER_RATIO;

// DC voltage: R1=150k, R2=10k on the motor terminal (downstream of relay).
// 0-50 V -> ~0-3.13 V at ADS1115 AIN. Scale = (R1+R2)/R2 = 16.
static const float VDIV_R1    = 150000.0f;
static const float VDIV_R2    = 10000.0f;
static const float VDIV_SCALE = (VDIV_R1 + VDIV_R2) / VDIV_R2;
