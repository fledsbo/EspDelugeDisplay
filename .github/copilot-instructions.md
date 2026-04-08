# Copilot Instructions — DelugeDisplay

ESP32-S3 firmware that mirrors the Synthstrom Deluge's OLED display in real-time over USB MIDI SysEx. Runs on a **Waveshare ESP32-S3-AMOLED-1.91** board.

## Build & Flash

PlatformIO with Arduino framework. Board target: `esp32-s3-devkitc-1` (or Waveshare-specific board def).

```sh
pio run                  # build
pio run -t upload        # flash
pio device monitor       # serial monitor
```

USB mode must be configured for **OTG/Host**, not the default CDC/serial. This requires specific build flags or `sdkconfig` overrides.

## Architecture

```
Deluge (USB MIDI device) ──USB──▶ ESP32-S3 (USB Host) ──QSPI──▶ RM67162 AMOLED
                                       │
                          Poll every ~50ms (20Hz)
                          on USB MIDI Port 3 (cable number 2)
```

**Data flow per frame:**
1. Send SysEx request `F0 7D 02 00 01 F7` on MIDI cable 2
2. Receive SysEx response → strip wrapper (`F0`…`F7`)
3. **7-to-8 bit unpack**: every 8 MIDI bytes (7-bit) → 7 real bytes (8-bit), high bits packed into every 8th byte
4. **RLE decode** → 768 bytes: 128×48 monochrome framebuffer (column-major, 6 pages of 128 bytes, like SSD1306)
5. **Render**: 4× nearest-neighbor scale → 512×192 centered on 536×240 AMOLED in landscape

### Source layout (intended)

- `src/main.cpp` — setup, main loop, orchestration
- `src/usb_midi_host.*` — USB Host init, MIDI send/receive, SysEx reassembly from 4-byte USB MIDI packets
- `src/deluge_sysex.*` — SysEx command builders, 7-to-8 unpack, RLE decode
- `src/display.*` — AMOLED init (RM67162 driver), 4× scaling, framebuffer blit
- `src/config.h` — pins, timing constants, colors

## SysEx Protocol

Manufacturer ID: `0x7D` (educational/development). All commands sent on **MIDI Port 3** (cable number 2, zero-indexed).

| Command         | Bytes               |
| --------------- | ------------------- |
| Ping            | `F0 7D 00 F7`       |
| Request OLED    | `F0 7D 02 00 01 F7` |
| Request 7-Seg   | `F0 7D 02 01 00 F7` |
| Request Debug   | `F0 7D 03 00 01 F7` |
| Request Version | `F0 7D 03 02 01 F7` |

## Critical Implementation Details

- **SysEx reassembly**: USB MIDI packets are 4 bytes. SysEx spans multiple packets — `0x04` for continuation, `0x05`/`0x06`/`0x07` for endings. The cable number is in the high nibble of byte 0.
- **7-to-8 and RLE decode must match the Deluge exactly.** Reference the [DEx source](https://github.com/silicakes/deluge-extensions) (`src/` — the unpack and RLE functions) and [delugeclient](https://github.com/bfredl/delugeclient). Fetch and study those before writing decode logic.
- **Display driver**: Use Waveshare's provided RM67162 init code — don't write custom init sequences. [Waveshare examples](https://github.com/waveshareteam/Waveshare-ESP32-S3-AMOLED-1.91).
- **Power**: Board is powered via 5V on GPIO header, not USB-C. The USB-C port connects to the Deluge.
- **Colors**: Background black (`0x0000` RGB565, AMOLED pixels off). Foreground white (`0xFFFF`) or amber (`0xFD20`).

## Development Phases

1. Display only — test patterns on AMOLED in landscape
2. USB Host — enumerate connected MIDI device, log descriptors
3. SysEx ping — send ping on Port 3, verify pong response
4. Display request — receive and hex-dump OLED SysEx response
5. Decode & render — full pipeline to AMOLED
6. Polish — reconnection handling, status indicators, color config
