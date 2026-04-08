#include "deluge_sysex.h"
#include "debug_server.h"
#include <string.h>

// ─── Command builders ───

void sysex_build_ping(uint8_t* buf, size_t* len) {
    buf[0] = 0xF0; buf[1] = SYSEX_MFG_ID; buf[2] = 0x00; buf[3] = 0xF7;
    *len = 4;
}

void sysex_build_request_oled(uint8_t* buf, size_t* len) {
    buf[0] = 0xF0; buf[1] = SYSEX_MFG_ID;
    buf[2] = 0x02; buf[3] = 0x00; buf[4] = 0x01;
    buf[5] = 0xF7;
    *len = 6;
}

void sysex_build_request_7seg(uint8_t* buf, size_t* len) {
    buf[0] = 0xF0; buf[1] = SYSEX_MFG_ID;
    buf[2] = 0x02; buf[3] = 0x01; buf[4] = 0x00;
    buf[5] = 0xF7;
    *len = 6;
}

void sysex_build_toggle_display(uint8_t* buf, size_t* len) {
    buf[0] = 0xF0; buf[1] = SYSEX_MFG_ID;
    buf[2] = 0x02; buf[3] = 0x00; buf[4] = 0x04;
    buf[5] = 0xF7;
    *len = 6;
}

// ─── Combined 7-to-8 + RLE decode ───
// Ported verbatim from bfredl/delugeclient lib.c

int unpack_7to8_rle(uint8_t* dst, int dst_size, const uint8_t* src, int src_len) {
    int d = 0;
    int s = 0;

    while (s + 1 < src_len) {
        uint8_t first = src[s++];

        if (first < 64) {
            // Dense (literal) block: 2–5 bytes with packed high bits
            int size = 0, off = 0;
            if (first < 4)       { size = 2; off = 0;  }
            else if (first < 12) { size = 3; off = 4;  }
            else if (first < 28) { size = 4; off = 12; }
            else if (first < 60) { size = 5; off = 28; }
            else                 { return -7; }

            if (size > src_len - s) return -1;
            if (size > dst_size - d) return -11;

            int highbits = first - off;
            for (int j = 0; j < size; j++) {
                dst[d + j] = src[s + j] & 0x7F;
                if (highbits & (1 << j)) {
                    dst[d + j] |= 0x80;
                }
            }
            d += size;
            s += size;
        } else {
            // RLE repeat block
            first = first - 64;
            int high = (first & 1);
            int runlen = first >> 1;
            if (runlen == 31) {
                runlen = 31 + src[s++];
                if (s == src_len) return -3;
            }
            int byte_val = src[s++] + 128 * high;
            if (runlen > dst_size - d) return -12;
            memset(dst + d, byte_val, runlen);
            d += runlen;
        }
    }
    return d;
}

// ─── SysEx response parser ───

bool sysex_parse_oled(const uint8_t* sysex, size_t len,
                      uint8_t* oledBuf, size_t oledBufSize)
{
    if (len < 8 || sysex[0] != 0xF0) return false;

    // Deluge responds with either:
    //   F0 7D 02 40 ... F7           (dev manufacturer ID)
    //   F0 00 21 7B 01 02 40 ... F7  (real Synthstrom manufacturer ID)
    int dataOffset;
    if (sysex[1] == SYSEX_MFG_ID && sysex[2] == 0x02 && sysex[3] == 0x40) {
        // Short form: F0 7D 02 40 <type> <flags> <data...> F7
        dataOffset = 4;
    } else if (len >= 11 && sysex[1] == 0x00 && sysex[2] == 0x21
               && sysex[3] == 0x7B && sysex[4] == 0x01
               && sysex[5] == 0x02 && sysex[6] == 0x40) {
        // Long form: F0 00 21 7B 01 02 40 <type> <flags> <data...> F7
        dataOffset = 7;
    } else {
        return false;
    }

    uint8_t frameType = sysex[dataOffset];

    if (frameType == 0x01) {
        // Full OLED frame — decode into a larger temp buffer
        // (some Deluge models may send slightly more than 768 bytes)
        const uint8_t* packed = &sysex[dataOffset + 2];
        int packedLen = (int)len - (dataOffset + 2) - 1;
        if (packedLen <= 0) return false;

        static uint8_t decodeBuf[1024];
        int decoded = unpack_7to8_rle(decodeBuf, sizeof(decodeBuf), packed, packedLen);
        if (decoded < OLED_BUF_SIZE) {
            static int failCount = 0;
            if (failCount++ < 5) {
                debug_log("OLED decode: packed=%d decoded=%d need=%d",
                          packedLen, decoded, OLED_BUF_SIZE);
            }
            return false;
        }
        // Copy first 768 bytes (128×48 framebuffer)
        memcpy(oledBuf, decodeBuf, OLED_BUF_SIZE);
        return true;

    } else if (frameType == 0x02) {
        // Delta OLED frame
        if ((int)len < dataOffset + 4) return false;
        int first_block = sysex[dataOffset + 1];
        int num_blocks  = sysex[dataOffset + 2];
        const uint8_t* packed = &sysex[dataOffset + 3];
        int packedLen = (int)len - (dataOffset + 3) - 1;
        if (packedLen <= 0) return false;

        uint8_t tmpBuf[OLED_BUF_SIZE];
        int decoded = unpack_7to8_rle(tmpBuf, sizeof(tmpBuf), packed, packedLen);
        if (decoded <= 0) return false;

        int offset = first_block * 8;
        int copyLen = num_blocks * 8;
        if (offset + copyLen > (int)oledBufSize) return false;
        if (decoded < copyLen) copyLen = decoded;
        memcpy(oledBuf + offset, tmpBuf, copyLen);
        return true;
    }

    return false;
}

// Parse 7-segment response: F0 7D 02 41 00 <...> <dots> <d0> <d1> <d2> <d3> F7
bool sysex_parse_7seg(const uint8_t* sysex, size_t len,
                      uint8_t digits[4], uint8_t* dots)
{
    if (len < 8 || sysex[0] != 0xF0) return false;

    int dataOffset;
    if (sysex[1] == SYSEX_MFG_ID && sysex[2] == 0x02 && sysex[3] == 0x41) {
        dataOffset = 4;
    } else if (len >= 12 && sysex[1] == 0x00 && sysex[2] == 0x21
               && sysex[3] == 0x7B && sysex[4] == 0x01
               && sysex[5] == 0x02 && sysex[6] == 0x41) {
        dataOffset = 7;
    } else {
        return false;
    }

    // Format after header: <type=0> ... <dots> <digit0> <digit1> <digit2> <digit3> F7
    // From Mac app: bytes[6]=dots, bytes[7..10]=digits (with short header)
    int dotsIdx = dataOffset + 2;  // skip type byte and one more
    if ((int)len < dotsIdx + 5 + 1) return false;  // need dots + 4 digits + F7

    *dots = sysex[dotsIdx];
    digits[0] = sysex[dotsIdx + 1];
    digits[1] = sysex[dotsIdx + 2];
    digits[2] = sysex[dotsIdx + 3];
    digits[3] = sysex[dotsIdx + 4];
    return true;
}
