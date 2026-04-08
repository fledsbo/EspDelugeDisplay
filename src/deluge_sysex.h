#pragma once

#include <Arduino.h>
#include "config.h"

// SysEx command builders
void sysex_build_ping(uint8_t* buf, size_t* len);
void sysex_build_request_oled(uint8_t* buf, size_t* len);
void sysex_build_request_7seg(uint8_t* buf, size_t* len);
void sysex_build_toggle_display(uint8_t* buf, size_t* len);

// Combined 7-to-8 + RLE decode (ported from delugeclient lib.c).
// Returns decoded byte count, or negative on error.
int unpack_7to8_rle(uint8_t* dst, int dst_size, const uint8_t* src, int src_len);

// Parse a raw SysEx message. Returns true if it contained OLED data.
// On success, oledBuf is filled with the 768-byte framebuffer.
bool sysex_parse_oled(const uint8_t* sysex, size_t len,
                      uint8_t* oledBuf, size_t oledBufSize);

// Parse 7-segment response. Returns true if valid.
// digits[4] gets the segment bitmasks, dots gets the dot state.
bool sysex_parse_7seg(const uint8_t* sysex, size_t len,
                      uint8_t digits[4], uint8_t* dots);
