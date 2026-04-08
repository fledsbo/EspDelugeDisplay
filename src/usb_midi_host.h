#pragma once

#include <Arduino.h>
#include "config.h"

// USB MIDI Host — manages USB Host stack, device enumeration,
// SysEx send/receive on the configured cable number.

// Call once in setup() after Serial is ready
bool usb_midi_host_init();

// Connection state
bool usb_midi_connected();

// Send raw SysEx bytes on a SPECIFIC cable (overriding MIDI_CABLE_NUM)
bool usb_midi_send_sysex_on_cable(const uint8_t* data, size_t len, uint8_t cable);

// Convenience: send on the default MIDI_CABLE_NUM
bool usb_midi_send_sysex(const uint8_t* data, size_t len);

// Check if a complete SysEx message has been received.
// Returns length of message, or 0 if none ready.
// Copies into the provided buffer.
size_t usb_midi_receive_sysex(uint8_t* buf, size_t bufSize);
