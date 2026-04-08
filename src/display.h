#pragma once

#include <Arduino.h>
#include <driver/spi_master.h>
#include "config.h"

// RM67162 AMOLED driver via QSPI.
// Based on Waveshare ESP32-S3-AMOLED-1.91 reference code.
class Display {
public:
    bool begin();
    void clear(uint16_t color = COLOR_BG);
    void renderOled(const uint8_t* oledBuf, size_t len, uint16_t fg = COLOR_FG, uint16_t bg = COLOR_BG);
    void drawText(uint16_t x, uint16_t y, const char* text, uint16_t fg = COLOR_FG, uint8_t scale = 1, uint16_t bg = COLOR_BG);
    void setBrightness(uint8_t brightness);

private:
    spi_device_handle_t _handle = nullptr;
    spi_transaction_ext_t _spi_tran_ext;
    spi_transaction_t* _spi_tran = nullptr;
    uint8_t* _dmaBuf = nullptr;

    static constexpr int QSPI_FREQ = 80000000;
    static constexpr int MAX_CHUNK_PX = 8192;

    void reset();
    void initPanel();
    void writeCommand(uint8_t cmd);
    void writeC8D8(uint8_t cmd, uint8_t data);
    void setWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void pushPixels(const uint16_t* data, uint32_t numPixels);
    void pushColor(uint16_t color, uint32_t count);

    void csHigh();
    void csLow();
    void pollStart();
    void pollEnd();
};

extern Display display;
