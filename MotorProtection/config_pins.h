#pragma once

#include <stdint.h>

// ESP32-S3-N16R8 — all GPIO numbers live in this file only.
// GPIO 22-25 do not exist on this chip at all (not physical pins).
// GPIO 26-37 are reserved for flash/octal PSRAM on this N16R8 module.
// GPIO 0/45 remain fully off-limits (boot mode / flash voltage).
// GPIO 3 is used as ENC_SW (strapping-safe with pull-up).
// GPIO 43/44 are UART0 — leave them untouched.
// GPIO 46 is input-only and a strapping pin (ROM extra boot-log text only,
// unrelated to boot mode / flash mode / JTAG) and is used here as ENC_B (DT).
// GPIO 19/20 are native USB D-/D+ — NEVER use as GPIO on this board
// (USB CDC is active).
// GPIO 48 is the onboard WS2812-style RGB LED (not an encoder pin).

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
static const int ENC_A_PIN  = 47;  // CLK
static const int ENC_B_PIN  = 46;  // DT (GPIO 46 is input-only; encoder line only)
// GPIO 3 is a strapping pin (JTAG source). KY-040's pull-up
// holds it HIGH at boot (JTAG from GPIO pins — never used).
// Safe as input after boot. GPIO 19 is USB D- and must never
// be used as GPIO on this board.
static const int ENC_SW_PIN = 3;  // SW (active-LOW when pressed)

// Onboard WS2812-style RGB LED. One-wire, NOT I2C — never take s_i2c_mu.
static const int LED_PIN = 48;

// Local panel: SH1106 128x64 OLED shares the ADS1115 I2C bus (PIN_I2C_SDA /
// PIN_I2C_SCL). Bus access is serialized by s_i2c_mu — never a second bus.
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
