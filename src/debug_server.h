#pragma once
#ifndef DEBUG_SERVER_H
#define DEBUG_SERVER_H

#include <Arduino.h>

// WiFi credentials — defined via build flags from .env file
#ifndef WIFI_SSID
#error "WIFI_SSID not defined. Create a .env file with WIFI_SSID=your_network"
#endif
#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD not defined. Create a .env file with WIFI_PASSWORD=your_password"
#endif

// Initialize WiFi + HTTP debug server
void debug_server_init();

// Call from loop() to handle HTTP clients
void debug_server_task();

// Log a message to the ring buffer (also prints to Serial)
void debug_log(const char* fmt, ...);

// Set pointer to the OLED framebuffer for the web display
void debug_server_set_framebuffer(const uint8_t* buf, size_t len);

#endif // DEBUG_SERVER_H
