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

// Analog path: ACS712-30A 66 mV/A, 10k/15k divider (x0.6) => 39.6 mV/A at ADC.
static const float ACS712_MV_PER_AMP = 66.0f;
static const float DIVIDER_RATIO     = 0.6f;
static const float MV_PER_AMP        = ACS712_MV_PER_AMP * DIVIDER_RATIO;

// DC voltage: R1=150k, R2=10k on the motor terminal (downstream of relay).
// 0-50 V -> ~0-3.13 V at ADS1115 AIN. Scale = (R1+R2)/R2 = 16.
static const float VDIV_R1    = 150000.0f;
static const float VDIV_R2    = 10000.0f;
static const float VDIV_SCALE = (VDIV_R1 + VDIV_R2) / VDIV_R2;
