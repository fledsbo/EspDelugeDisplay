# DelugeDisplay

ESP32-S3 firmware that mirrors the Synthstrom Deluge's display in real-time over USB MIDI SysEx. Renders the Deluge's 128×48 pixel display at 4× scale on a 1.91" AMOLED, and also streams it to a live web view.

Works with both OLED and 7-segment Deluge models running [community firmware](https://github.com/SynthstromAudible/DelugeFirmware) 1.0+.

## Hardware

- **[Waveshare ESP32-S3-AMOLED-1.91](https://www.waveshare.com/wiki/ESP32-S3-AMOLED-1.91)** — ESP32-S3R8 with 240×536 AMOLED (RM67162, QSPI) - I got mine from here: https://www.aliexpress.com/item/1005007871571802.html?spm=a2g0o.order_list.order_list_main.78.6c1a1802RjNY60
- **Synthstrom Deluge** — running community firmware 1.0 or later
- **USB-C cable** — connects the Deluge's USB port to the ESP32-S3's USB-C port

The ESP32-S3 acts as USB Host. No separate power supply is needed — the board is powered directly by the Deluge over USB.

## Building

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)

### Setup

1. Clone the repository
2. Create a `.env` file in the project root with your WiFi credentials:

```
WIFI_SSID=your_network_name
WIFI_PASSWORD=your_password
```

WiFi enables the debug web server and OTA firmware updates.

3. Build and flash:

```sh
pio run                          # build
pio run -t upload                # flash (first time, via USB)
```

To enter USB bootloader mode on the Waveshare board: hold **BOOT**, press **RST**, release **BOOT**. The board appears as a new COM port.

### OTA Updates

After the first USB flash, subsequent updates can be done over WiFi:

- Browse to `http://<device-ip>/ota` and upload the firmware binary
- Or from the command line:

```sh
curl -F "firmware=@.pio/build/esp32s3/firmware.bin" http://<device-ip>/ota
```

The device IP is printed to serial on boot and shown in the debug logs.

## Testing

Unit tests cover the SysEx command builders, RLE decoder, and OLED/7-segment parsers using a real Deluge frame capture as test data.

```sh
# Compile tests (no device needed)
pio test -e esp32s3 --without-uploading --without-testing

# Run on device (flashes test firmware, reads results via serial)
pio test -e esp32s3

# Run natively (requires gcc/g++ on PATH)
pio test -e native_test
```

## Usage

1. Connect the Deluge to the ESP32-S3's USB-C port (this also powers the board)
2. The AMOLED displays "WAITING FOR DELUGE..." until connected
3. Once connected, the Deluge's display is mirrored at 4× scale on the AMOLED

### Web Interface

The device runs a web server on your local network:

| Endpoint   | Description                                 |
| ---------- | ------------------------------------------- |
| `/display` | Live display mirror (canvas, auto-updating) |
| `/`        | Debug log viewer                            |
| `/log`     | Plain text log dump                         |
| `/ota`     | Firmware update page                        |
| `/fb`      | Raw framebuffer (768 bytes, binary)         |

## How It Works

```
Deluge ──USB──▶ ESP32-S3 (USB Host) ──QSPI──▶ RM67162 AMOLED
                     │
          Poll via SysEx on MIDI Port 3
          Decode: 7-to-8 + RLE → 128×48 → 4× scale
```

The firmware polls the Deluge for its display framebuffer via SysEx (`F0 7D 02 00 01 F7`) on USB MIDI cable 2 (Port 3). The Deluge responds with RLE-compressed pixel data. The firmware decodes it and renders at 4× nearest-neighbor scale (512×192, centered on 536×240).

For 7-segment Deluges, the firmware sends a display toggle command to switch the Deluge's SysEx output to OLED mode.

### SysEx Protocol

Manufacturer ID: `0x7D`. All commands on MIDI cable 2.

| Command        | Bytes               | Description                 |
| -------------- | ------------------- | --------------------------- |
| Ping           | `F0 7D 00 F7`       | Connection check            |
| Request OLED   | `F0 7D 02 00 01 F7` | Request display framebuffer |
| Request 7-Seg  | `F0 7D 02 01 00 F7` | Request 7-segment state     |
| Toggle Display | `F0 7D 02 00 04 F7` | Switch display SysEx mode   |

The decode algorithm is ported from [delugeclient](https://github.com/bfredl/delugeclient) (`lib.c`).

## Project Structure

```
├── platformio.ini        # PlatformIO config (pioarduino, ESP32-S3)
├── partitions.csv        # Dual OTA partition layout
├── load_env.py           # Reads .env into build flags
├── .env                  # WiFi credentials (gitignored)
└── src/
    ├── main.cpp          # Setup, loop, probe sequence, polling
    ├── config.h          # Pins, timing, display constants
    ├── display.h/.cpp    # RM67162 AMOLED driver (QSPI), 4× scaling
    ├── usb_midi_host.h/.cpp  # ESP-IDF USB Host, MIDI SysEx on cable 2
    ├── deluge_sysex.h/.cpp   # SysEx commands, 7-to-8 + RLE decode
    └── debug_server.h/.cpp   # WiFi, HTTP debug/display/OTA server
```

## Acknowledgments

- Github Copilot, who wrote all the code...

- [DEx (Deluge Extensions)](https://github.com/silicakes/deluge-extensions) — web-based Deluge display mirror
- [delugeclient](https://github.com/bfredl/delugeclient) — original SysEx decode implementation
- [DelugeDisplay (macOS)](https://github.com/douglas-carmichael/DelugeDisplay) — native Mac display mirror
- [Deluge Community Firmware](https://github.com/SynthstromAudible/DelugeFirmware) — SysEx display feature
