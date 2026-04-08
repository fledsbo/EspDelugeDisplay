#pragma once

// ─── Display pins (Waveshare ESP32-S3-AMOLED-1.91) ───
#define LCD_CS    6
#define LCD_SCK   47
#define LCD_D0    18
#define LCD_D1    7
#define LCD_D2    48
#define LCD_D3    5
#define LCD_RST   17

// ─── Display dimensions ───
#define LCD_WIDTH       536
#define LCD_HEIGHT      240

// ─── Deluge OLED geometry ───
#define OLED_WIDTH      128
#define OLED_HEIGHT     48
#define OLED_PAGES      6        // 48 / 8
#define OLED_BUF_SIZE   768      // 128 * 6

// ─── Scaling ───
#define SCALE_FACTOR    4
#define SCALED_WIDTH    (OLED_WIDTH  * SCALE_FACTOR)   // 512
#define SCALED_HEIGHT   (OLED_HEIGHT * SCALE_FACTOR)   // 192
#define OFFSET_X        ((LCD_WIDTH  - SCALED_WIDTH)  / 2)  // 12
#define OFFSET_Y        ((LCD_HEIGHT - SCALED_HEIGHT) / 2)  // 24

// ─── Colors (RGB565) ───
#define COLOR_BG        0x0000   // black — AMOLED pixels off
#define COLOR_FG        0xFFFF   // white
#define COLOR_FG_AMBER  0xFD20   // warm amber

// ─── Timing ───
#define POLL_INTERVAL_MS      50   // 20 Hz
#define CONNECT_RETRY_MS    2000
#define USB_TASK_INTERVAL_MS   1

// ─── SysEx ───
#define SYSEX_MFG_ID    0x7D
#define MIDI_CABLE_NUM  2        // Port 3 confirmed by descriptor: jack 5 (OUT), jack 6 (IN)

// ─── SysEx receive buffer ───
#define SYSEX_RX_BUF_SIZE  4096
#define RLE_DECODE_BUF_SIZE 1024
