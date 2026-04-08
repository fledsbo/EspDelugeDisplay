# Deluge Display Mirror — ESP32-S3 Firmware Project

## Project Goal

Build Arduino/ESP-IDF firmware for a **Waveshare ESP32-S3-AMOLED-1.91** development board that mirrors the Synthstrom Deluge's OLED display in real-time over USB MIDI SysEx.

The ESP32-S3 acts as **USB Host**. The Deluge (running community firmware) connects as a USB MIDI device. The firmware polls the Deluge for its OLED framebuffer via SysEx, decodes the response, and renders it on the AMOLED screen.

## Hardware

### Waveshare ESP32-S3-AMOLED-1.91

- **MCU**: ESP32-S3R8 (dual-core LX7, 240MHz, 8MB PSRAM, 16MB Flash)
- **Display**: 1.91" AMOLED, 240×536 pixels, RM67162 driver, QSPI interface
- **USB**: Native USB OTG (can act as USB Host)
- **Power**: Powered externally via 5V on GPIO header (USB-C port is used for Deluge connection)
- **Wiki/docs**: https://www.waveshare.com/wiki/ESP32-S3-AMOLED-1.91
- **Example code**: https://github.com/waveshareteam/Waveshare-ESP32-S3-AMOLED-1.91

### Synthstrom Deluge (target device)

- **OLED display**: 128×48 pixels, monochrome (1-bit)
- **USB**: USB type-B, presents as USB MIDI device with 3 ports when running community firmware
- **Display data port**: USB MIDI **Port 3** carries the SysEx display data
- **Firmware**: Community firmware 1.0+ required (SysEx display feature)

## SysEx Protocol

The Deluge community firmware exposes display data over MIDI SysEx. The manufacturer ID is `0x7D` (educational/development use).

### Commands (send TO the Deluge)

| Command         | Bytes               | Description                                 |
| --------------- | ------------------- | ------------------------------------------- |
| Ping            | `F0 7D 00 F7`       | Connection check, Deluge responds with pong |
| Request OLED    | `F0 7D 02 00 01 F7` | Request current OLED framebuffer            |
| Request 7-Seg   | `F0 7D 02 01 00 F7` | Request 7-segment display state             |
| Request Debug   | `F0 7D 03 00 01 F7` | Request debug messages                      |
| Request Version | `F0 7D 03 02 01 F7` | Request firmware version string             |

### OLED Response Format

The Deluge responds with a SysEx message containing the 128×48 pixel OLED framebuffer. The pixel data is:

1. **RLE compressed** (run-length encoded)
2. **7-to-8 bit packed** (because MIDI SysEx can only carry 7-bit bytes — values 0x00-0x7F)

#### Decoding steps:

1. **Strip SysEx wrapper**: Remove `F0` header bytes and `F7` end byte
2. **7-to-8 bit unpack**: Every 8 bytes of 7-bit MIDI data decode to 7 bytes of 8-bit data. The high bits are packed into every 8th byte.
3. **RLE decode**: The unpacked stream uses RLE. The exact RLE scheme: if a byte's value has a specific marker, the following byte(s) indicate repeat count. Study the DEx source (see references below) for the exact algorithm.
4. **Render**: The decoded buffer is 128×48 pixels, 1 bit per pixel (768 bytes uncompressed). Each byte contains 8 vertical pixels (column-major, like SSD1306 page addressing). There are 6 pages (48/8=6) of 128 bytes each.

### Reference Implementation

The best reference for the SysEx decode logic is the **DEx (Deluge Extensions)** web app:

- Repository: https://github.com/silicakes/deluge-extensions
- Key source files to study (in `src/`):
  - The SysEx send/receive handling
  - The 7-to-8 bit unpacking function
  - The RLE decode function
  - The OLED canvas rendering (128×48 pixels)
- Also reference: https://github.com/bfredl/delugeclient (original implementation)

**Before writing any decode logic, fetch and study the actual source files from these repos to get the exact algorithm right.** The 7-to-8 and RLE decode must match exactly or you'll get garbage pixels.

## Display Rendering

### Scaling

- Deluge OLED: 128×48 pixels
- AMOLED in landscape: 536×240 pixels
- Scale factor: **4x** → 512×192 pixels
- Centered on display with (536-512)/2 = 12px horizontal padding, (240-192)/2 = 24px vertical padding
- Use nearest-neighbor scaling (no interpolation) to keep pixel-art crispness

### Colors

- Background: Pure black (`0x0000` in RGB565) — lets the AMOLED pixels turn off completely
- Foreground (lit pixels): White (`0xFFFF`) or warm amber (`0xFD20`) to match classic OLED look
- Consider making the foreground color configurable via a boot-time button press

### Orientation

- The AMOLED is physically 240 wide × 536 tall (portrait)
- Render in **landscape** orientation (536 wide × 240 tall) to match the Deluge's wide aspect ratio
- The display driver or LVGL can handle rotation

## Architecture

### Recommended approach: Arduino + TinyUSB

Use the **Arduino framework** with:

- **ESP32 TinyUSB Host** for USB MIDI host functionality
- **Waveshare's display driver / LVGL** for the AMOLED (the Waveshare examples include the RM67162 driver)
- Alternatively, use the Waveshare-provided `rm67162.h` driver directly without LVGL for simplicity (just blit a framebuffer)

### Main loop pseudocode

```
setup():
    init_display()  // landscape, black background
    init_usb_host() // configure as USB MIDI host
    show "Waiting for Deluge..." on display

loop():
    if deluge_connected and enough_time_since_last_poll (e.g., 50ms):
        send_sysex(REQUEST_OLED)  // F0 7D 02 00 01 F7

    if sysex_response_received:
        raw = strip_sysex_wrapper(response)
        unpacked = unpack_7to8(raw)
        pixels = rle_decode(unpacked)
        render_to_display(pixels)  // scale 4x and blit

    handle_usb_events()  // TinyUSB task
```

### Polling rate

- Poll at ~20Hz (every 50ms) for smooth updates
- The Deluge only sends a new frame when the display has actually changed, so bandwidth is modest
- If updates feel sluggish, try 30Hz; if USB gets overwhelmed, back off to 10Hz

## USB Host Configuration

The ESP32-S3 USB OTG port needs to be configured as **USB Host** (not device). Key considerations:

- The Deluge presents as a **USB MIDI class-compliant device** with **3 virtual MIDI ports**
- You need to communicate on **Port 3** (the third virtual cable/port, cable number 2 in zero-indexed terms)
- MIDI SysEx messages can span multiple USB MIDI packets (SysEx is variable-length)
- You must handle SysEx assembly from potentially fragmented USB MIDI packets
- The USB MIDI cable number is encoded in the high nibble of the first byte of each 4-byte USB MIDI packet

## File Structure

```
deluge-display-mirror/
├── CLAUDE.md                    # This file
├── platformio.ini               # PlatformIO config (or use Arduino IDE)
├── src/
│   ├── main.cpp                 # Setup, main loop, orchestration
│   ├── usb_midi_host.h/.cpp     # USB Host MIDI init, send/receive, SysEx assembly
│   ├── deluge_sysex.h/.cpp      # SysEx command builders, 7-to-8 unpack, RLE decode
│   ├── display.h/.cpp           # AMOLED init, scaling, rendering
│   └── config.h                 # Pin definitions, timing constants, colors
└── lib/                         # External libraries
```

## Development Environment

- **PlatformIO** preferred (well-supported for ESP32-S3 + TinyUSB)
- Board: `esp32-s3-devkitc-1` or appropriate Waveshare board definition
- Framework: Arduino
- USB mode: Must be configured for OTG/Host (not the default CDC mode)
- Partition scheme: Needs enough space for the firmware + PSRAM for framebuffer

## Key Challenges & Gotchas

1. **USB Host mode on ESP32-S3**: The default Arduino ESP32 config uses USB as CDC/serial. You need to explicitly configure for USB Host mode. This may require specific `sdkconfig` or build flags.

2. **MIDI Port 3 targeting**: USB MIDI uses "cable numbers" to distinguish virtual ports. The display SysEx goes on cable number 2 (Port 3, zero-indexed). Make sure to filter/send on the correct cable.

3. **SysEx reassembly**: USB MIDI packets are 4 bytes. SysEx longer than 3 bytes spans multiple packets with specific packet types (0x04 for continuation, 0x05/0x06/0x07 for endings). Handle this correctly.

4. **7-to-8 bit encoding**: MIDI SysEx bytes are 7-bit (0x00-0x7F). The Deluge packs 8-bit data by distributing high bits across groups of bytes. Get this exactly right by studying the DEx source.

5. **Display init**: The RM67162 AMOLED driver needs specific initialization. Use Waveshare's provided driver code — don't try to write your own init sequence.

6. **Power**: The board is powered via 5V on GPIO header pins, NOT via USB-C. The USB-C port connects to the Deluge for MIDI data only.

## Testing Strategy

1. **Phase 1 — Display only**: Get the AMOLED working in landscape mode, draw test patterns (e.g., a 128×48 white rectangle at 4x scale, centered)
2. **Phase 2 — USB Host**: Get the ESP32-S3 to enumerate a connected USB MIDI device and log its descriptors
3. **Phase 3 — SysEx ping**: Send the ping command (`F0 7D 00 F7`) on Port 3 and verify a response
4. **Phase 4 — Display request**: Send the OLED request, receive and hex-dump the response
5. **Phase 5 — Decode & render**: Implement 7-to-8 unpack + RLE decode, render to AMOLED
6. **Phase 6 — Polish**: Optimize polling rate, add connection status indicator, handle reconnection

## References

- DEx source (SysEx protocol + decode logic): https://github.com/silicakes/deluge-extensions
- Original deluge client: https://github.com/bfredl/delugeclient
- Deluge community firmware source: https://github.com/SynthstromAudible/DelugeFirmware
- Waveshare board wiki: https://www.waveshare.com/wiki/ESP32-S3-AMOLED-1.91
- Waveshare example code: https://github.com/waveshareteam/Waveshare-ESP32-S3-AMOLED-1.91
- USB MIDI spec: https://www.usb.org/sites/default/files/midi10.pdf
- ESP32-S3 USB Host: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_host.html
