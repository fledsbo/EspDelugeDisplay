#include <unity.h>
#include <string.h>
#include <stdarg.h>

// Prevent debug_server.h from being included (it requires Arduino/WiFi)
#define DEBUG_SERVER_H
void debug_log(const char* fmt, ...) { (void)fmt; }
void debug_server_set_framebuffer(const uint8_t* buf, size_t len) { (void)buf; (void)len; }

#include "config.h"
#include "deluge_sysex.h"
#include "deluge_sysex.cpp"

// ═══════════════════════════════════════════════════════════════
// Command builders
// ═══════════════════════════════════════════════════════════════

void test_build_ping(void) {
    uint8_t buf[8];
    size_t len;
    sysex_build_ping(buf, &len);
    TEST_ASSERT_EQUAL(4, len);
    TEST_ASSERT_EQUAL_HEX8(0xF0, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x7D, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0xF7, buf[3]);
}

void test_build_request_oled(void) {
    uint8_t buf[8];
    size_t len;
    sysex_build_request_oled(buf, &len);
    TEST_ASSERT_EQUAL(6, len);
    uint8_t expected[] = {0xF0, 0x7D, 0x02, 0x00, 0x01, 0xF7};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, 6);
}

void test_build_request_7seg(void) {
    uint8_t buf[8];
    size_t len;
    sysex_build_request_7seg(buf, &len);
    TEST_ASSERT_EQUAL(6, len);
    uint8_t expected[] = {0xF0, 0x7D, 0x02, 0x01, 0x00, 0xF7};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, 6);
}

void test_build_toggle_display(void) {
    uint8_t buf[8];
    size_t len;
    sysex_build_toggle_display(buf, &len);
    TEST_ASSERT_EQUAL(6, len);
    TEST_ASSERT_EQUAL_HEX8(0x04, buf[4]);
}

// ═══════════════════════════════════════════════════════════════
// RLE decode
// ═══════════════════════════════════════════════════════════════

void test_rle_all_zeros(void) {
    // RLE: run of 128 zeros = first=64+(128<<1 would overflow, so split)
    // Simpler: run of 31+97=128, value 0x00
    // first = 64 + (0 high bit) + (31 << 1) = 64 + 62 = 126
    // extended: next byte = 97
    // value byte: 0x00
    uint8_t packed[] = {126, 97, 0x00};
    uint8_t dst[256];
    int n = unpack_7to8_rle(dst, sizeof(dst), packed, sizeof(packed));
    TEST_ASSERT_EQUAL(128, n);
    for (int i = 0; i < 128; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00, dst[i]);
    }
}

void test_rle_short_run(void) {
    // Run of 5 of value 0x42: first = 64 + (0 high) + (5<<1) = 74, value=0x42
    uint8_t packed[] = {74, 0x42};
    uint8_t dst[16];
    int n = unpack_7to8_rle(dst, sizeof(dst), packed, sizeof(packed));
    TEST_ASSERT_EQUAL(5, n);
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x42, dst[i]);
    }
}

void test_rle_run_with_high_bit(void) {
    // Run of 3 of value 0xFF: high=1, so first = 64 + 1 + (3<<1) = 71, value=0x7F
    uint8_t packed[] = {71, 0x7F};
    uint8_t dst[16];
    int n = unpack_7to8_rle(dst, sizeof(dst), packed, sizeof(packed));
    TEST_ASSERT_EQUAL(3, n);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, dst[i]);
    }
}

void test_rle_dense_2_bytes(void) {
    // Dense block of 2 bytes [0x12, 0x34]: no high bits
    // first = 0 + 0 (highbits) = 0, then 0x12, 0x34
    uint8_t packed[] = {0, 0x12, 0x34};
    uint8_t dst[16];
    int n = unpack_7to8_rle(dst, sizeof(dst), packed, sizeof(packed));
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX8(0x12, dst[0]);
    TEST_ASSERT_EQUAL_HEX8(0x34, dst[1]);
}

void test_rle_dense_2_bytes_with_high(void) {
    // Dense block of 2 bytes [0x92, 0x34]: first byte has high bit
    // highbits = 1 (bit 0 set), first = 0 + 1 = 1
    // data: 0x12 (0x92 & 0x7F), 0x34
    uint8_t packed[] = {1, 0x12, 0x34};
    uint8_t dst[16];
    int n = unpack_7to8_rle(dst, sizeof(dst), packed, sizeof(packed));
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX8(0x92, dst[0]);
    TEST_ASSERT_EQUAL_HEX8(0x34, dst[1]);
}

void test_rle_dense_3_bytes(void) {
    // Dense block of 3 bytes [0x11, 0x22, 0x33]: no high bits
    // off=4 for size=3, first = 4 + 0 = 4
    uint8_t packed[] = {4, 0x11, 0x22, 0x33};
    uint8_t dst[16];
    int n = unpack_7to8_rle(dst, sizeof(dst), packed, sizeof(packed));
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x11, dst[0]);
    TEST_ASSERT_EQUAL_HEX8(0x22, dst[1]);
    TEST_ASSERT_EQUAL_HEX8(0x33, dst[2]);
}

void test_rle_dst_overflow(void) {
    // Run of 10 into a 5-byte buffer
    uint8_t packed[] = {64 + (10 << 1), 0x42};  // run of 10
    uint8_t dst[5];
    int n = unpack_7to8_rle(dst, sizeof(dst), packed, sizeof(packed));
    TEST_ASSERT_LESS_THAN(0, n);  // Should return negative error
}

void test_rle_empty_input(void) {
    uint8_t dst[16];
    int n = unpack_7to8_rle(dst, sizeof(dst), nullptr, 0);
    TEST_ASSERT_EQUAL(0, n);
}

// ═══════════════════════════════════════════════════════════════
// SysEx OLED parser
// ═══════════════════════════════════════════════════════════════

// Test data from bfredl/delugeclient app.js — a real Deluge OLED frame
static const uint8_t TEST_SYSEX[] = {
    0xF0, 0x7D, 0x02, 0x40, 0x01, 0x00,
    // packed data (RLE-encoded OLED framebuffer)
    126, 127, 0, 102, 0, 66, 76, 71, 18, 44, 100, 0, 6, 8, 112, 36,
    8, 6, 0, 126, 8, 16, 16, 32, 126, 0, 68, 2, 67, 126, 68, 2,
    2, 0, 126, 70, 16, 67, 126, 126, 127, 0, 114, 0, 71, 64, 72, 0,
    69, 124, 68, 108, 3, 124, 120, 68, 0, 67, 96, 69, 120, 70, 12, 69,
    120, 57, 96, 0, 0, 112, 124, 27, 28, 124, 112, 0, 68, 0, 69, 124,
    69, 76, 5, 124, 120, 48, 68, 0, 69, 124, 68, 12, 10, 28, 120, 112,
    68, 0, 20, 48, 120, 124, 108, 69, 76, 67, 0, 84, 0, 67, 96, 69,
    120, 70, 12, 69, 120, 67, 96, 68, 0, 69, 124, 71, 76, 66, 12, 86,
    0, 69, 124, 68, 12, 10, 28, 120, 112, 70, 0, 69, 124, 70, 108, 66,
    12, 70, 0, 69, 124, 98, 0, 68, 7, 68, 6, 0, 7, 3, 70, 0,
    68, 3, 70, 6, 68, 3, 12, 0, 4, 7, 3, 70, 1, 12, 3, 7,
    4, 0, 68, 7, 28, 0, 1, 7, 6, 4, 68, 0, 68, 7, 68, 6,
    4, 7, 3, 1, 68, 0, 66, 2, 70, 6, 4, 7, 3, 1, 86, 0,
    68, 3, 70, 6, 68, 3, 70, 0, 68, 7, 94, 0, 68, 7, 68, 6,
    4, 7, 3, 1, 70, 0, 68, 7, 72, 6, 70, 0, 68, 7, 72, 6,
    126, 98, 0,
    0xF7
};

void test_parse_oled_real_frame(void) {
    uint8_t oledBuf[OLED_BUF_SIZE];
    memset(oledBuf, 0xAA, sizeof(oledBuf));

    bool ok = sysex_parse_oled(TEST_SYSEX, sizeof(TEST_SYSEX), oledBuf, sizeof(oledBuf));
    TEST_ASSERT_TRUE(ok);

    // First ~158 bytes should be 0x00 (blank top of display from the RLE run)
    for (int i = 0; i < 128; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00, oledBuf[i]);
    }
    // The buffer should be fully populated (no 0xAA sentinel remaining in first 768 bytes)
    // Spot-check some non-zero pixels exist in the middle
    bool hasNonZero = false;
    for (int i = 128; i < OLED_BUF_SIZE; i++) {
        if (oledBuf[i] != 0x00) { hasNonZero = true; break; }
    }
    TEST_ASSERT_TRUE(hasNonZero);
}

void test_parse_oled_long_manufacturer(void) {
    // Build a frame with the long Synthstrom manufacturer ID: F0 00 21 7B 01 02 40 ...
    // Use a simple RLE payload: 768 bytes of 0x00
    // RLE: 5 runs of 158 zeros = 5 * [0x7E, 0x7F, 0x00] = 790 bytes decoded (>768, OK)
    // Actually need exactly 768. Let's do 4 runs of 158 (=632) + 1 run of 136 (=768)
    // run of 158: [126, 97, 0x00]
    // run of 136: 136 = 31+105, first=64+(31<<1)=126, ext=105, val=0x00
    uint8_t sysex[32];
    int pos = 0;
    sysex[pos++] = 0xF0;
    sysex[pos++] = 0x00; sysex[pos++] = 0x21;
    sysex[pos++] = 0x7B; sysex[pos++] = 0x01;  // Synthstrom mfg ID
    sysex[pos++] = 0x02; sysex[pos++] = 0x40;  // HID, OLED
    sysex[pos++] = 0x01;  // frame type: full
    sysex[pos++] = 0x00;  // flags
    // RLE: run of 768 zeros = 31+127=158 max per run
    // 4 * 158 = 632, need 136 more
    for (int r = 0; r < 4; r++) {
        sysex[pos++] = 126; sysex[pos++] = 97; sysex[pos++] = 0x00;  // run of 158
    }
    // run of 136: 31+105
    sysex[pos++] = 126; sysex[pos++] = 105; sysex[pos++] = 0x00;
    sysex[pos++] = 0xF7;

    uint8_t oledBuf[OLED_BUF_SIZE];
    bool ok = sysex_parse_oled(sysex, pos, oledBuf, sizeof(oledBuf));
    TEST_ASSERT_TRUE(ok);
    for (int i = 0; i < OLED_BUF_SIZE; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00, oledBuf[i]);
    }
}

void test_parse_oled_rejects_garbage(void) {
    uint8_t garbage[] = {0xF0, 0x00, 0x01, 0x02, 0xF7};
    uint8_t oledBuf[OLED_BUF_SIZE];
    TEST_ASSERT_FALSE(sysex_parse_oled(garbage, sizeof(garbage), oledBuf, sizeof(oledBuf)));
}

void test_parse_oled_rejects_too_short(void) {
    uint8_t buf[] = {0xF0, 0x7D, 0x02, 0x40};
    uint8_t oledBuf[OLED_BUF_SIZE];
    TEST_ASSERT_FALSE(sysex_parse_oled(buf, sizeof(buf), oledBuf, sizeof(oledBuf)));
}

void test_parse_oled_rejects_wrong_mfg(void) {
    uint8_t buf[] = {0xF0, 0x7E, 0x02, 0x40, 0x01, 0x00, 126, 97, 0x00, 0xF7};
    uint8_t oledBuf[OLED_BUF_SIZE];
    TEST_ASSERT_FALSE(sysex_parse_oled(buf, sizeof(buf), oledBuf, sizeof(oledBuf)));
}

// ═══════════════════════════════════════════════════════════════
// 7-segment parser
// ═══════════════════════════════════════════════════════════════

void test_parse_7seg_valid(void) {
    // F0 7D 02 41 00 <??> <dots> <d0> <d1> <d2> <d3> F7
    uint8_t sysex[] = {0xF0, 0x7D, 0x02, 0x41, 0x00, 0x00, 0x03, 0x47, 0x3F, 0x08, 0x19, 0xF7};
    uint8_t digits[4];
    uint8_t dots;
    bool ok = sysex_parse_7seg(sysex, sizeof(sysex), digits, &dots);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_HEX8(0x03, dots);
    TEST_ASSERT_EQUAL_HEX8(0x47, digits[0]);
    TEST_ASSERT_EQUAL_HEX8(0x3F, digits[1]);
    TEST_ASSERT_EQUAL_HEX8(0x08, digits[2]);
    TEST_ASSERT_EQUAL_HEX8(0x19, digits[3]);
}

void test_parse_7seg_rejects_oled(void) {
    uint8_t sysex[] = {0xF0, 0x7D, 0x02, 0x40, 0x01, 0x00, 0x00, 0xF7};
    uint8_t digits[4];
    uint8_t dots;
    TEST_ASSERT_FALSE(sysex_parse_7seg(sysex, sizeof(sysex), digits, &dots));
}

// ═══════════════════════════════════════════════════════════════
// Runner
// ═══════════════════════════════════════════════════════════════

static void run_all_tests() {
    UNITY_BEGIN();

    // Command builders
    RUN_TEST(test_build_ping);
    RUN_TEST(test_build_request_oled);
    RUN_TEST(test_build_request_7seg);
    RUN_TEST(test_build_toggle_display);

    // RLE decode
    RUN_TEST(test_rle_all_zeros);
    RUN_TEST(test_rle_short_run);
    RUN_TEST(test_rle_run_with_high_bit);
    RUN_TEST(test_rle_dense_2_bytes);
    RUN_TEST(test_rle_dense_2_bytes_with_high);
    RUN_TEST(test_rle_dense_3_bytes);
    RUN_TEST(test_rle_dst_overflow);
    RUN_TEST(test_rle_empty_input);

    // OLED parser
    RUN_TEST(test_parse_oled_real_frame);
    RUN_TEST(test_parse_oled_long_manufacturer);
    RUN_TEST(test_parse_oled_rejects_garbage);
    RUN_TEST(test_parse_oled_rejects_too_short);
    RUN_TEST(test_parse_oled_rejects_wrong_mfg);

    // 7-segment parser
    RUN_TEST(test_parse_7seg_valid);
    RUN_TEST(test_parse_7seg_rejects_oled);

    UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup() {
    Serial.begin(115200);
    delay(2000);
    run_all_tests();
}
void loop() {}
#else
int main() { run_all_tests(); return 0; }
#endif
