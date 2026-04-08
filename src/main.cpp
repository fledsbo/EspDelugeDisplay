#include <Arduino.h>
#include "config.h"
#include "display.h"
#include "usb_midi_host.h"
#include "deluge_sysex.h"
#include "debug_server.h"

// ─── State ───
static uint8_t oledFramebuffer[OLED_BUF_SIZE];
static uint8_t sysexRxBuf[SYSEX_RX_BUF_SIZE];
static uint8_t sysexCmd[8];
static unsigned long lastPollTime = 0;
static bool wasConnected = false;
static bool hasFrame = false;
static unsigned long connectTime = 0;
static uint32_t pollCount = 0;
static uint32_t frameCount = 0;
static int probePhase = 0;  // 0=7seg request, 1=wait 7seg, 2=toggle, 3=first OLED, 4+=polling
static unsigned long lastProbeTime = 0;

// Status text scale: 3× makes 5×7 font → 15×21 pixels, readable on 536×240
static const uint8_t STATUS_SCALE = 3;

static void showStatus(const char* line1, const char* line2 = nullptr) {
    display.clear();
    // Center vertically: line height = 8*3 = 24px, gap = 8px
    uint16_t y1 = line2 ? (LCD_HEIGHT / 2 - 28) : (LCD_HEIGHT / 2 - 12);
    display.drawText(OFFSET_X, y1, line1, COLOR_FG, STATUS_SCALE);
    if (line2) {
        display.drawText(OFFSET_X, y1 + 32, line2, COLOR_FG, STATUS_SCALE);
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== DelugeDisplay ===");

    if (psramFound()) {
        Serial.printf("PSRAM: %d bytes\n", ESP.getPsramSize());
    }

    if (!display.begin()) {
        Serial.println("FATAL: Display init failed");
        while (1) delay(1000);
    }
    display.clear();
    showStatus("DELUGE DISPLAY", "INIT...");

    debug_server_init();

    if (!usb_midi_host_init()) {
        debug_log("FATAL: USB Host init failed");
        showStatus("USB HOST FAIL");
        while (1) delay(1000);
    }

    showStatus("WAITING FOR", "DELUGE...");
    memset(oledFramebuffer, 0, sizeof(oledFramebuffer));
    debug_server_set_framebuffer(oledFramebuffer, sizeof(oledFramebuffer));
    debug_log("Setup complete, waiting for Deluge");
}

void loop() {
    debug_server_task();

    unsigned long now = millis();
    bool connected = usb_midi_connected();

    // Connection state change
    if (connected && !wasConnected) {
        debug_log("Deluge connected!");
        showStatus("DELUGE FOUND", "PROBING...");
        hasFrame = false;
        connectTime = now;
        pollCount = 0;
        frameCount = 0;
        probePhase = 0;
        lastProbeTime = 0;
    } else if (!connected && wasConnected) {
        debug_log("Deluge disconnected");
        showStatus("WAITING FOR", "DELUGE...");
        hasFrame = false;
    }
    wasConnected = connected;

    if (!connected) {
        delay(10);
        return;
    }

    // Wait 500ms after connection before probing
    if (now - connectTime < 500) return;

    // Probe sequence for 7-segment Deluge:
    // 0: Request 7-seg on cable 2
    // 1: Wait for response (confirms communication)
    // 2: Send toggle command (switch Deluge SysEx output to OLED mode)
    // 3: Wait 200ms for toggle to take effect
    // 4: Start OLED polling
    if (probePhase == 0 && now - lastProbeTime >= 300) {
        lastProbeTime = now;
        size_t cmdLen;
        sysex_build_request_7seg(sysexCmd, &cmdLen);
        bool sent = usb_midi_send_sysex(sysexCmd, cmdLen);
        debug_log("PROBE 7seg request: %s", sent ? "OK" : "FAIL");
        probePhase = 1;
    }

    if (probePhase == 1 && now - lastProbeTime >= 500) {
        // Timeout waiting for 7-seg response — try OLED directly
        debug_log("PROBE 7seg timeout, trying OLED directly");
        probePhase = 2;
        lastProbeTime = now;
    }

    if (probePhase == 2 && now - lastProbeTime >= 100) {
        lastProbeTime = now;
        size_t cmdLen;
        sysex_build_toggle_display(sysexCmd, &cmdLen);
        bool sent = usb_midi_send_sysex(sysexCmd, cmdLen);
        debug_log("PROBE toggle display: %s", sent ? "OK" : "FAIL");
        probePhase = 3;
    }

    if (probePhase == 3 && now - lastProbeTime >= 300) {
        // Request OLED after toggle
        size_t cmdLen;
        sysex_build_request_oled(sysexCmd, &cmdLen);
        bool sent = usb_midi_send_sysex(sysexCmd, cmdLen);
        debug_log("PROBE oled after toggle: %s", sent ? "OK" : "FAIL");
        probePhase = 4;
        lastProbeTime = now;
    }

    // Regular polling — alternate between OLED and 7-seg until we know which works
    if (probePhase >= 4) {
        if (now - lastPollTime >= POLL_INTERVAL_MS) {
            lastPollTime = now;
            pollCount++;

            size_t cmdLen;
            if (hasFrame) {
                // We know OLED works — keep requesting it
                sysex_build_request_oled(sysexCmd, &cmdLen);
            } else if (pollCount % 20 == 0) {
                // Every ~1 second, try toggling if we haven't got a frame yet
                sysex_build_toggle_display(sysexCmd, &cmdLen);
                debug_log("Retry toggle (poll #%lu)", pollCount);
            } else {
                sysex_build_request_oled(sysexCmd, &cmdLen);
            }
            bool sent = usb_midi_send_sysex(sysexCmd, cmdLen);

            if (pollCount <= 5 || pollCount % 100 == 0) {
                debug_log("Poll #%lu: send=%s", pollCount, sent ? "OK" : "FAIL");
            }
        }
    }

    // Check for received SysEx
    size_t rxLen = usb_midi_receive_sysex(sysexRxBuf, sizeof(sysexRxBuf));
    if (rxLen > 0) {
        debug_log("SysEx rx: %d bytes [%02X %02X %02X %02X %02X ...]",
                  rxLen,
                  rxLen > 0 ? sysexRxBuf[0] : 0,
                  rxLen > 1 ? sysexRxBuf[1] : 0,
                  rxLen > 2 ? sysexRxBuf[2] : 0,
                  rxLen > 3 ? sysexRxBuf[3] : 0,
                  rxLen > 4 ? sysexRxBuf[4] : 0);

        // Try OLED parse
        if (sysex_parse_oled(sysexRxBuf, rxLen, oledFramebuffer, sizeof(oledFramebuffer))) {
            display.renderOled(oledFramebuffer, sizeof(oledFramebuffer));
            frameCount++;
            if (!hasFrame) {
                debug_log("First OLED frame rendered!");
            }
            hasFrame = true;
        } else {
            // Try 7-seg parse
            uint8_t digits[4];
            uint8_t dots;
            if (sysex_parse_7seg(sysexRxBuf, rxLen, digits, &dots)) {
                debug_log("7-seg: [%02X %02X %02X %02X] dots=%02X",
                          digits[0], digits[1], digits[2], digits[3], dots);
                // Got 7-seg data — advance probe to toggle
                if (probePhase == 1) {
                    debug_log("7-seg confirmed! Sending toggle to switch to OLED mode");
                    probePhase = 2;
                    lastProbeTime = now;
                }
            } else {
                debug_log("SysEx unknown: cmd=0x%02X sub=0x%02X len=%d",
                         rxLen >= 3 ? sysexRxBuf[2] : 0,
                         rxLen >= 4 ? sysexRxBuf[3] : 0,
                         rxLen);
            }
        }
    }
}
