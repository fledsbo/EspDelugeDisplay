#include "display.h"
#include <string.h>

Display display;

// RM67162 commands
#define RM_SLPOUT     0x11
#define RM_DISPON     0x29
#define RM_CASET      0x2A
#define RM_PASET      0x2B
#define RM_RAMWR      0x2C
#define RM_MADCTL     0x36
#define RM_COLMOD     0x3A
#define RM_BRIGHTNESS 0x51
#define RM_INVON      0x21

#define MADCTL_MY  0x80
#define MADCTL_MV  0x20
#define MADCTL_RGB 0x00

bool Display::begin() {
    pinMode(LCD_CS, OUTPUT);
    digitalWrite(LCD_CS, HIGH);
    reset();

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = LCD_D0;
    buscfg.miso_io_num = LCD_D1;
    buscfg.sclk_io_num = LCD_SCK;
    buscfg.quadwp_io_num = LCD_D2;
    buscfg.quadhd_io_num = LCD_D3;
    buscfg.data4_io_num = -1;
    buscfg.data5_io_num = -1;
    buscfg.data6_io_num = -1;
    buscfg.data7_io_num = -1;
    buscfg.max_transfer_sz = MAX_CHUNK_PX * 2;
    buscfg.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;

    if (spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        Serial.println("SPI bus init failed");
        return false;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.command_bits = 8;
    devcfg.address_bits = 24;
    devcfg.mode = 0;
    devcfg.clock_speed_hz = QSPI_FREQ;
    devcfg.spics_io_num = -1;
    devcfg.flags = SPI_DEVICE_HALFDUPLEX;
    devcfg.queue_size = 1;

    if (spi_bus_add_device(SPI2_HOST, &devcfg, &_handle) != ESP_OK) {
        Serial.println("SPI device add failed");
        return false;
    }

    spi_device_acquire_bus(_handle, portMAX_DELAY);
    memset(&_spi_tran_ext, 0, sizeof(_spi_tran_ext));
    _spi_tran = (spi_transaction_t*)&_spi_tran_ext;

    _dmaBuf = (uint8_t*)heap_caps_aligned_alloc(16, MAX_CHUNK_PX * 2, MALLOC_CAP_DMA);
    if (!_dmaBuf) {
        Serial.println("DMA buffer alloc failed");
        return false;
    }

    writeCommand(RM_SLPOUT);
    delay(120);
    initPanel();

    Serial.println("Display OK");
    return true;
}

void Display::reset() {
    pinMode(LCD_RST, OUTPUT);
    digitalWrite(LCD_RST, LOW);
    delay(20);
    digitalWrite(LCD_RST, HIGH);
    delay(150);
}

void Display::initPanel() {
    writeC8D8(RM_COLMOD, 0x55);  // 16-bit RGB565
    writeC8D8(RM_MADCTL, MADCTL_MY | MADCTL_MV | MADCTL_RGB);  // landscape
    writeC8D8(RM_BRIGHTNESS, 0xD0);
    writeCommand(RM_DISPON);
    writeCommand(RM_INVON);
    delay(20);
}

void Display::setBrightness(uint8_t brightness) {
    writeC8D8(RM_BRIGHTNESS, brightness);
}

void Display::clear(uint16_t color) {
    setWindow(0, 0, LCD_WIDTH, LCD_HEIGHT);
    pushColor(color, LCD_WIDTH * LCD_HEIGHT);
}

// Render decoded 128×48 monochrome framebuffer scaled 4× onto AMOLED
void Display::renderOled(const uint8_t* oledBuf, size_t len, uint16_t fg, uint16_t bg) {
    if (len < OLED_BUF_SIZE) return;

    // Render row-by-row in scanline order to minimize memory
    // Each OLED row becomes SCALE_FACTOR display rows
    static uint16_t lineBuf[SCALED_WIDTH];

    setWindow(OFFSET_X, OFFSET_Y, SCALED_WIDTH, SCALED_HEIGHT);

    for (int oledY = 0; oledY < OLED_HEIGHT; oledY++) {
        int page = oledY / 8;
        int bit  = oledY % 8;
        uint8_t mask = 1 << bit;

        // Build one scaled scanline
        for (int oledX = 0; oledX < OLED_WIDTH; oledX++) {
            uint16_t color = (oledBuf[page * OLED_WIDTH + oledX] & mask) ? fg : bg;
            int baseX = oledX * SCALE_FACTOR;
            for (int sx = 0; sx < SCALE_FACTOR; sx++) {
                lineBuf[baseX + sx] = color;
            }
        }

        // Push this scanline SCALE_FACTOR times (vertical scaling)
        for (int sy = 0; sy < SCALE_FACTOR; sy++) {
            pushPixels(lineBuf, SCALED_WIDTH);
        }
    }
}

// Minimal 5×7 font for status messages
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7F,0x2A,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x08,0x2A,0x1C,0x2A,0x08}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x00,0x08,0x14,0x22,0x41}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x41,0x22,0x14,0x08,0x00}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x01,0x01}, // F
    {0x3E,0x41,0x41,0x51,0x32}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x04,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x7F,0x20,0x18,0x20,0x7F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x03,0x04,0x78,0x04,0x03}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
};

void Display::drawText(uint16_t x, uint16_t y, const char* text, uint16_t fg, uint8_t scale, uint16_t bg) {
    const int charW = 6 * scale;
    const int charH = 8 * scale;
    // Allocate scanline buffer for one scaled character
    static uint16_t charBuf[6 * 4 * 8 * 4]; // max scale=4 → 24×32 = 768 pixels

    if (scale > 4) scale = 4;

    while (*text) {
        char c = *text++;
        int idx = -1;
        if (c >= ' ' && c <= 'Z') idx = c - ' ';
        else if (c >= 'a' && c <= 'z') idx = c - 'a' + ('A' - ' ');

        if (idx < 0 || idx >= (int)(sizeof(font5x7)/sizeof(font5x7[0]))) {
            x += charW;
            continue;
        }

        // Build scaled character bitmap
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 5; col++) {
                uint8_t colData = font5x7[idx][col];
                uint16_t color = (colData & (1 << row)) ? fg : bg;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        charBuf[(row * scale + sy) * charW + col * scale + sx] = color;
                    }
                }
            }
            // Spacing column
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    charBuf[(row * scale + sy) * charW + 5 * scale + sx] = bg;
                }
            }
        }

        setWindow(x, y, charW, charH);
        pushPixels(charBuf, charW * charH);
        x += charW;
    }
}

// ─── Low-level QSPI ───

void Display::writeCommand(uint8_t cmd) {
    csLow();
    _spi_tran_ext.base.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
    _spi_tran_ext.base.cmd = 0x02;
    _spi_tran_ext.base.addr = ((uint32_t)cmd) << 8;
    _spi_tran_ext.base.tx_buffer = NULL;
    _spi_tran_ext.base.length = 0;
    pollStart();
    pollEnd();
    csHigh();
}

void Display::writeC8D8(uint8_t cmd, uint8_t data) {
    csLow();
    _spi_tran_ext.base.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
    _spi_tran_ext.base.cmd = 0x02;
    _spi_tran_ext.base.addr = ((uint32_t)cmd) << 8;
    _spi_tran_ext.base.tx_data[0] = data;
    _spi_tran_ext.base.length = 8;
    pollStart();
    pollEnd();
    csHigh();
}

void Display::setWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint16_t x1 = x + w - 1;
    uint16_t y1 = y + h - 1;

    csLow();
    _spi_tran_ext.base.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
    _spi_tran_ext.base.cmd = 0x02;
    _spi_tran_ext.base.addr = ((uint32_t)RM_CASET) << 8;
    _spi_tran_ext.base.tx_data[0] = x >> 8;
    _spi_tran_ext.base.tx_data[1] = x & 0xFF;
    _spi_tran_ext.base.tx_data[2] = x1 >> 8;
    _spi_tran_ext.base.tx_data[3] = x1 & 0xFF;
    _spi_tran_ext.base.length = 32;
    pollStart(); pollEnd();
    csHigh();

    csLow();
    _spi_tran_ext.base.addr = ((uint32_t)RM_PASET) << 8;
    _spi_tran_ext.base.tx_data[0] = y >> 8;
    _spi_tran_ext.base.tx_data[1] = y & 0xFF;
    _spi_tran_ext.base.tx_data[2] = y1 >> 8;
    _spi_tran_ext.base.tx_data[3] = y1 & 0xFF;
    pollStart(); pollEnd();
    csHigh();

    writeCommand(RM_RAMWR);
}

void Display::pushPixels(const uint16_t* data, uint32_t numPixels) {
    csLow();
    bool first = true;
    uint32_t remaining = numPixels;
    const uint16_t* src = data;

    while (remaining > 0) {
        uint32_t chunk = (remaining > MAX_CHUNK_PX) ? MAX_CHUNK_PX : remaining;
        memcpy(_dmaBuf, src, chunk * 2);

        if (first) {
            _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO;
            _spi_tran_ext.base.cmd = 0x32;
            _spi_tran_ext.base.addr = 0x003C00;
            first = false;
        } else {
            _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO
                | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
        }

        _spi_tran_ext.base.tx_buffer = _dmaBuf;
        _spi_tran_ext.base.length = chunk << 4;
        pollStart(); pollEnd();

        src += chunk;
        remaining -= chunk;
    }
    csHigh();
}

void Display::pushColor(uint16_t color, uint32_t count) {
    uint16_t* buf = (uint16_t*)_dmaBuf;
    uint32_t fillCount = (count > MAX_CHUNK_PX) ? MAX_CHUNK_PX : count;
    for (uint32_t i = 0; i < fillCount; i++) buf[i] = color;

    csLow();
    bool first = true;
    uint32_t remaining = count;

    while (remaining > 0) {
        uint32_t chunk = (remaining > MAX_CHUNK_PX) ? MAX_CHUNK_PX : remaining;

        if (first) {
            _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO;
            _spi_tran_ext.base.cmd = 0x32;
            _spi_tran_ext.base.addr = 0x003C00;
            first = false;
        } else {
            _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO
                | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
        }

        _spi_tran_ext.base.tx_buffer = _dmaBuf;
        _spi_tran_ext.base.length = chunk << 4;
        pollStart(); pollEnd();

        remaining -= chunk;
    }
    csHigh();
}

void Display::csHigh() { digitalWrite(LCD_CS, HIGH); }
void Display::csLow()  { digitalWrite(LCD_CS, LOW); }

void Display::pollStart() {
    esp_err_t err = spi_device_polling_start(_handle, _spi_tran, portMAX_DELAY);
    if (err != ESP_OK) {
        static bool logged = false;
        if (!logged) { Serial.printf("SPI start err: 0x%x\n", err); logged = true; }
    }
}

void Display::pollEnd() {
    esp_err_t err = spi_device_polling_end(_handle, portMAX_DELAY);
    if (err != ESP_OK) {
        static bool logged = false;
        if (!logged) { Serial.printf("SPI end err: 0x%x\n", err); logged = true; }
    }
}
