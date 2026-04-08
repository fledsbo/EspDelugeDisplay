#include "usb_midi_host.h"
#include "config.h"
#include "debug_server.h"
#include <Arduino.h>
#include <usb/usb_host.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ─── USB Host state (only accessed from USB task) ───
static volatile bool s_hostInstalled = false;
static volatile bool s_deviceConnected = false;
static usb_host_client_handle_t s_clientHandle = nullptr;
static usb_device_handle_t s_deviceHandle = nullptr;
static uint8_t s_epIn = 0;
static uint8_t s_epOut = 0;
static uint8_t s_ifaceNum = 0;
static int s_ifaceSetting = 0;

// ─── Double-buffered SysEx receive (#1, #8) ───
// USB task assembles into writeSlot; on completion, swaps to readSlot under mutex.
static uint8_t s_sysexSlots[2][SYSEX_RX_BUF_SIZE];
static size_t s_sysexSlotLen[2] = {0, 0};
static int s_writeSlot = 0;
static int s_readSlot = -1;  // -1 = none ready
static SemaphoreHandle_t s_sysexMutex = nullptr;

// Assembly state (USB task only — no sync needed)
static size_t s_assemblyLen = 0;
static bool s_assemblyActive = false;

// ─── Transfer state ───
static usb_transfer_t* s_xferIn = nullptr;
static usb_transfer_t* s_xferOut = nullptr;
static volatile bool s_readPending = false;
static volatile bool s_writePending = false;
static volatile unsigned long s_writeSubmitTime = 0;
static const unsigned long WRITE_WATCHDOG_MS = 500;

// Debug counters
static uint32_t s_inCbCount = 0;
static uint32_t s_inBytes = 0;
static uint32_t s_outLogCount = 0;
static uint32_t s_outCbCount = 0;

// Forward declarations
static void client_event_cb(const usb_host_client_event_msg_t* msg, void* arg);
static void xfer_in_cb(usb_transfer_t* xfer);
static void xfer_out_cb(usb_transfer_t* xfer);
static bool open_midi_device(usb_device_handle_t dev);
static void submit_read();
static void usb_host_task_fn(void* arg);
static TaskHandle_t s_usbTaskHandle = nullptr;

bool usb_midi_host_init() {
    s_sysexMutex = xSemaphoreCreateMutex();

    usb_host_config_t hostConfig = {};
    hostConfig.skip_phy_setup = false;
    hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;

    esp_err_t err = usb_host_install(&hostConfig);
    if (err != ESP_OK) {
        debug_log("USB Host install failed: 0x%x", err);
        return false;
    }
    s_hostInstalled = true;

    usb_host_client_config_t clientConfig = {};
    clientConfig.is_synchronous = false;
    clientConfig.max_num_event_msg = 5;
    clientConfig.async.client_event_callback = client_event_cb;
    clientConfig.async.callback_arg = nullptr;

    err = usb_host_client_register(&clientConfig, &s_clientHandle);
    if (err != ESP_OK) {
        debug_log("USB client register failed: 0x%x", err);
        return false;
    }

    usb_host_transfer_alloc(64, 0, &s_xferIn);
    usb_host_transfer_alloc(512, 0, &s_xferOut);

    xTaskCreatePinnedToCore(usb_host_task_fn, "usb_host", 4096, nullptr, 5, &s_usbTaskHandle, 0);

    debug_log("USB Host OK (dedicated task on core 0)");
    return true;
}

static void usb_host_task_fn(void* arg) {
    while (true) {
        usb_host_lib_handle_events(1, nullptr);

        if (s_clientHandle) {
            usb_host_client_handle_events(s_clientHandle, 1);
        }

        if (s_deviceConnected && !s_readPending && s_epIn) {
            submit_read();
        }

        // TX stuck detection (#11)
        if (s_writePending && (millis() - s_writeSubmitTime > WRITE_WATCHDOG_MS)) {
            debug_log("TX watchdog: write stuck for %lums, clearing", WRITE_WATCHDOG_MS);
            s_writePending = false;
        }

        taskYIELD();
    }
}

bool usb_midi_connected() {
    return s_deviceConnected;
}

// ─── SysEx TX packetization (#4: simplified, no dead branches) ───

bool usb_midi_send_sysex_on_cable(const uint8_t* data, size_t len, uint8_t cableNum) {
    if (!s_deviceConnected || !s_xferOut || !s_epOut) return false;
    if (s_writePending) return false;

    uint8_t* pkt = s_xferOut->data_buffer;
    int pktIdx = 0;
    uint8_t cable = cableNum << 4;
    size_t i = 0;

    while (i < len && pktIdx <= 512 - 4) {
        size_t remaining = len - i;

        if (remaining >= 3 && data[i + 2] != 0xF7) {
            pkt[pktIdx++] = cable | 0x04;
            pkt[pktIdx++] = data[i]; pkt[pktIdx++] = data[i+1]; pkt[pktIdx++] = data[i+2];
            i += 3;
        } else if (remaining == 3 && data[i + 2] == 0xF7) {
            pkt[pktIdx++] = cable | 0x07;
            pkt[pktIdx++] = data[i]; pkt[pktIdx++] = data[i+1]; pkt[pktIdx++] = data[i+2];
            i += 3;
        } else if (remaining == 2 && data[i + 1] == 0xF7) {
            pkt[pktIdx++] = cable | 0x06;
            pkt[pktIdx++] = data[i]; pkt[pktIdx++] = data[i+1]; pkt[pktIdx++] = 0x00;
            i += 2;
        } else if (remaining == 1 && data[i] == 0xF7) {
            pkt[pktIdx++] = cable | 0x05;
            pkt[pktIdx++] = data[i]; pkt[pktIdx++] = 0x00; pkt[pktIdx++] = 0x00;
            i += 1;
        } else {
            pkt[pktIdx++] = cable | 0x04;
            pkt[pktIdx++] = data[i]; pkt[pktIdx++] = data[i+1]; pkt[pktIdx++] = data[i+2];
            i += 3;
        }
    }

    if (s_outLogCount < 20) {
        char hex[128];
        int pos = 0;
        for (int j = 0; j < pktIdx && pos < 120; j++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", pkt[j]);
        }
        debug_log("OUT %d bytes on cable %d: %s", pktIdx, cableNum, hex);
        s_outLogCount++;
    }

    s_xferOut->num_bytes = pktIdx;
    s_xferOut->device_handle = s_deviceHandle;
    s_xferOut->bEndpointAddress = s_epOut;
    s_xferOut->callback = xfer_out_cb;
    s_xferOut->context = nullptr;

    esp_err_t err = usb_host_transfer_submit(s_xferOut);
    if (err == ESP_OK) {
        s_writePending = true;
        s_writeSubmitTime = millis();
    } else {
        debug_log("OUT submit err: 0x%x", err);
    }
    return (err == ESP_OK);
}

bool usb_midi_send_sysex(const uint8_t* data, size_t len) {
    return usb_midi_send_sysex_on_cable(data, len, MIDI_CABLE_NUM);
}

// ─── Double-buffered SysEx receive (#1, #8) ───

size_t usb_midi_receive_sysex(uint8_t* buf, size_t bufSize) {
    size_t result = 0;

    if (xSemaphoreTake(s_sysexMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (s_readSlot >= 0) {
            size_t copyLen = (s_sysexSlotLen[s_readSlot] < bufSize)
                           ? s_sysexSlotLen[s_readSlot] : bufSize;
            memcpy(buf, s_sysexSlots[s_readSlot], copyLen);
            result = copyLen;
            s_readSlot = -1;
        }
        xSemaphoreGive(s_sysexMutex);
    }

    return result;
}

// ─── Internal ───

static void submit_read() {
    if (!s_xferIn || !s_epIn || s_readPending) return;

    s_xferIn->num_bytes = 64;
    s_xferIn->device_handle = s_deviceHandle;
    s_xferIn->bEndpointAddress = s_epIn;
    s_xferIn->callback = xfer_in_cb;
    s_xferIn->context = nullptr;
    s_xferIn->timeout_ms = 0;

    esp_err_t err = usb_host_transfer_submit(s_xferIn);
    if (err == ESP_OK) {
        s_readPending = true;
    } else {
        debug_log("IN submit err: 0x%x", err);
    }
}

static void xfer_in_cb(usb_transfer_t* xfer) {
    s_readPending = false;
    s_inCbCount++;

    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        if (s_inCbCount <= 20 || s_inCbCount % 100 == 0) {
            debug_log("IN xfer status: %d (cb #%lu)", xfer->status, s_inCbCount);
        }
        return;
    }

    if (xfer->actual_num_bytes < 4) return;
    s_inBytes += xfer->actual_num_bytes;

    if (s_inCbCount <= 20 || s_inCbCount % 100 == 0) {
        debug_log("IN cb #%lu: %d bytes [%02X %02X %02X %02X ...]",
                  s_inCbCount, xfer->actual_num_bytes,
                  xfer->data_buffer[0], xfer->data_buffer[1],
                  xfer->data_buffer[2], xfer->data_buffer[3]);
    }

    // Reassemble SysEx into the current write slot
    uint8_t* asmBuf = s_sysexSlots[s_writeSlot];

    for (int i = 0; i + 4 <= xfer->actual_num_bytes; i += 4) {
        uint8_t header = xfer->data_buffer[i];
        uint8_t cin = header & 0x0F;

        switch (cin) {
            case 0x04: {
                uint8_t b1 = xfer->data_buffer[i + 1];
                if (b1 == 0xF0) {
                    s_assemblyLen = 0;
                    s_assemblyActive = true;
                } else if (!s_assemblyActive) {
                    break;
                }
                for (int b = 1; b <= 3; b++) {
                    if (s_assemblyLen < SYSEX_RX_BUF_SIZE)
                        asmBuf[s_assemblyLen++] = xfer->data_buffer[i + b];
                }
                break;
            }

            case 0x05:
                if (!s_assemblyActive) break;
                if (s_assemblyLen < SYSEX_RX_BUF_SIZE)
                    asmBuf[s_assemblyLen++] = xfer->data_buffer[i + 1];
                goto sysex_complete;

            case 0x06:
                if (!s_assemblyActive) break;
                for (int b = 1; b <= 2; b++) {
                    if (s_assemblyLen < SYSEX_RX_BUF_SIZE)
                        asmBuf[s_assemblyLen++] = xfer->data_buffer[i + b];
                }
                goto sysex_complete;

            case 0x07:
                if (!s_assemblyActive) break;
                for (int b = 1; b <= 3; b++) {
                    if (s_assemblyLen < SYSEX_RX_BUF_SIZE)
                        asmBuf[s_assemblyLen++] = xfer->data_buffer[i + b];
                }
                goto sysex_complete;

            sysex_complete: {
                s_assemblyActive = false;
                if (xSemaphoreTake(s_sysexMutex, 0) == pdTRUE) {
                    s_sysexSlotLen[s_writeSlot] = s_assemblyLen;
                    s_readSlot = s_writeSlot;
                    s_writeSlot = 1 - s_writeSlot;
                    xSemaphoreGive(s_sysexMutex);
                }
                s_assemblyLen = 0;
                break;
            }

            default:
                if (s_inCbCount <= 20) {
                    debug_log("  MIDI pkt: cable=%d CIN=0x%X [%02X %02X %02X]",
                              (header >> 4) & 0x0F, cin,
                              xfer->data_buffer[i + 1],
                              xfer->data_buffer[i + 2],
                              xfer->data_buffer[i + 3]);
                }
                break;
        }
    }
}

static void xfer_out_cb(usb_transfer_t* xfer) {
    s_writePending = false;
    s_outCbCount++;
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        debug_log("TX failed: status=%d", xfer->status);
    } else if (s_outCbCount <= 10) {
        debug_log("TX ok #%lu (%d bytes)", s_outCbCount, xfer->actual_num_bytes);
    }
}

static void client_event_cb(const usb_host_client_event_msg_t* msg, void* arg) {
    switch (msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV: {
            uint8_t addr = msg->new_dev.address;
            debug_log("New USB device: addr=%d", addr);

            usb_device_handle_t devHandle;
            if (usb_host_device_open(s_clientHandle, addr, &devHandle) == ESP_OK) {
                if (open_midi_device(devHandle)) {
                    s_deviceHandle = devHandle;
                    s_deviceConnected = true;
                    debug_log("MIDI device connected");
                } else {
                    usb_host_device_close(s_clientHandle, devHandle);
                    debug_log("Not a MIDI device");
                }
            }
            break;
        }
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            debug_log("USB device disconnected");
            if (s_deviceHandle) {
                usb_host_interface_release(s_clientHandle, s_deviceHandle, s_ifaceNum);
                usb_host_device_close(s_clientHandle, s_deviceHandle);
                s_deviceHandle = nullptr;
            }
            s_deviceConnected = false;
            s_epIn = 0;
            s_epOut = 0;
            s_readPending = false;
            s_writePending = false;
            s_assemblyLen = 0;
            s_assemblyActive = false;
            s_readSlot = -1;
            s_inCbCount = 0;
            s_inBytes = 0;
            break;
    }
}

static bool open_midi_device(usb_device_handle_t dev) {
    const usb_device_desc_t* devDesc;
    if (usb_host_get_device_descriptor(dev, &devDesc) != ESP_OK) return false;

    debug_log("USB Device: VID=0x%04X PID=0x%04X", devDesc->idVendor, devDesc->idProduct);

    const usb_config_desc_t* cfgDesc;
    if (usb_host_get_active_config_descriptor(dev, &cfgDesc) != ESP_OK) return false;

    debug_log("Config: totalLen=%d, numInterfaces=%d",
              cfgDesc->wTotalLength, cfgDesc->bNumInterfaces);

    const uint8_t* raw = (const uint8_t*)cfgDesc;
    for (int off = 0; off < cfgDesc->wTotalLength; off += 16) {
        char hex[80];
        int pos = 0;
        for (int j = 0; j < 16 && off + j < cfgDesc->wTotalLength; j++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", raw[off + j]);
        }
        debug_log("  desc[%3d]: %s", off, hex);
    }

    const uint8_t* p = raw;
    const uint8_t* end = p + cfgDesc->wTotalLength;
    bool foundMidi = false;

    while (p < end) {
        uint8_t dLen = p[0];
        uint8_t dType = p[1];
        if (dLen < 2) break;

        if (dType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            uint8_t ifNum = p[2];
            uint8_t altSetting = p[3];
            uint8_t ifClass = p[5];
            uint8_t ifSubclass = p[6];

            debug_log("  Interface %d alt=%d class=0x%02X sub=0x%02X",
                      ifNum, altSetting, ifClass, ifSubclass);

            if (ifClass == 0x01 && ifSubclass == 0x03) {
                s_ifaceNum = ifNum;
                s_ifaceSetting = altSetting;
                foundMidi = true;
            }
        } else if (dType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && foundMidi) {
            uint8_t epAddr = p[2];
            uint16_t maxPkt = p[4] | (p[5] << 8);
            debug_log("  Endpoint 0x%02X maxPkt=%d", epAddr, maxPkt);
            if (epAddr & 0x80) {
                s_epIn = epAddr;
            } else {
                s_epOut = epAddr;
            }
        }

        p += dLen;
    }

    if (!foundMidi || !s_epIn || !s_epOut) {
        debug_log("MIDI not found: midi=%d epIn=0x%02X epOut=0x%02X",
                  foundMidi, s_epIn, s_epOut);
        return false;
    }

    esp_err_t err = usb_host_interface_claim(s_clientHandle, dev, s_ifaceNum, s_ifaceSetting);
    if (err != ESP_OK) {
        debug_log("Interface claim failed: 0x%x", err);
        return false;
    }

    debug_log("MIDI interface %d claimed, EP IN=0x%02X OUT=0x%02X",
              s_ifaceNum, s_epIn, s_epOut);
    return true;
}
