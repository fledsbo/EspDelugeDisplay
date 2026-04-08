#include "debug_server.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <stdarg.h>

static WebServer server(80);

// OLED framebuffer reference
static const uint8_t* s_fbPtr = nullptr;
static size_t s_fbLen = 0;

// Ring buffer for log messages
static const int LOG_LINES = 200;
static const int LOG_LINE_LEN = 160;
static char logBuf[LOG_LINES][LOG_LINE_LEN];
static int logHead = 0;
static int logCount = 0;

void debug_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    // Timestamp prefix
    unsigned long ms = millis();
    int n = snprintf(logBuf[logHead], LOG_LINE_LEN, "[%7lu.%03lu] ",
                     ms / 1000, ms % 1000);

    vsnprintf(logBuf[logHead] + n, LOG_LINE_LEN - n, fmt, args);
    va_end(args);

    // Also print to Serial
    Serial.println(logBuf[logHead]);

    logHead = (logHead + 1) % LOG_LINES;
    if (logCount < LOG_LINES) logCount++;
}

static void handleRoot() {
    String html;
    html.reserve(LOG_LINES * (LOG_LINE_LEN + 30));

    html += "<!DOCTYPE html><html><head><title>DelugeDisplay Debug</title>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>"
            "body{background:#111;color:#0f0;font:13px/1.4 monospace;margin:8px;}"
            "h1{color:#0a0;font-size:16px;}"
            "pre{white-space:pre-wrap;word-break:break-all;}"
            ".t{color:#666;}"
            "a{color:#0af;}"
            "</style></head><body>"
            "<h1>DelugeDisplay Log</h1>"
            "<p><a href='/display'>Display</a> | <a href='/'>Refresh</a> | <a href='/clear'>Clear</a> | <a href='/ota'>OTA Update</a> | "
            "Lines: " + String(logCount) + "</p><pre>";

    // Print oldest to newest
    int start = (logCount < LOG_LINES) ? 0 : logHead;
    for (int i = 0; i < logCount; i++) {
        int idx = (start + i) % LOG_LINES;
        // Escape HTML
        for (int c = 0; logBuf[idx][c]; c++) {
            char ch = logBuf[idx][c];
            if (ch == '<') html += "&lt;";
            else if (ch == '>') html += "&gt;";
            else if (ch == '&') html += "&amp;";
            else html += ch;
        }
        html += '\n';
    }

    html += "</pre>"
            "<script>window.scrollTo(0,document.body.scrollHeight);</script>"
            "</body></html>";

    server.send(200, "text/html", html);
}

static void handleClear() {
    logHead = 0;
    logCount = 0;
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting...");
}

static void handlePlain() {
    String text;
    text.reserve(logCount * LOG_LINE_LEN);

    int start = (logCount < LOG_LINES) ? 0 : logHead;
    for (int i = 0; i < logCount; i++) {
        int idx = (start + i) % LOG_LINES;
        text += logBuf[idx];
        text += '\n';
    }
    server.send(200, "text/plain", text);
}

static void handleOtaPage() {
    server.send(200, "text/html",
        "<!DOCTYPE html><html><head><title>OTA Update</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{background:#111;color:#0f0;font:14px monospace;margin:20px;}"
        "h1{color:#0a0;}input,button{margin:8px 0;font:14px monospace;}"
        "button{background:#0a0;color:#111;border:none;padding:8px 20px;cursor:pointer;}"
        "#prog{color:#ff0;}</style></head><body>"
        "<h1>OTA Firmware Update</h1>"
        "<form method='POST' action='/ota' enctype='multipart/form-data'>"
        "<p>Select firmware.bin:</p>"
        "<input type='file' name='firmware' accept='.bin'><br>"
        "<button type='submit'>Upload &amp; Flash</button>"
        "</form>"
        "<p id='prog'></p>"
        "<p><a href='/' style='color:#0af;'>Back to logs</a></p>"
        "</body></html>");
}

static void handleOtaUpload() {
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        debug_log("OTA: receiving %s", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            debug_log("OTA: begin failed: %s", Update.errorString());
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            debug_log("OTA: write failed: %s", Update.errorString());
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            debug_log("OTA: success! %u bytes. Rebooting...", upload.totalSize);
        } else {
            debug_log("OTA: end failed: %s", Update.errorString());
        }
    }
}

static void handleOtaResult() {
    if (Update.hasError()) {
        server.send(500, "text/plain", String("FAIL: ") + Update.errorString());
    } else {
        server.send(200, "text/html",
            "<html><body style='background:#111;color:#0f0;font:14px monospace;margin:20px;'>"
            "<h1>Update OK!</h1><p>Rebooting in 2 seconds...</p></body></html>");
        delay(2000);
        ESP.restart();
    }
}

static void handleDisplayPage() {
    server.send(200, "text/html",
        "<!DOCTYPE html><html><head><title>Deluge Display</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>"
        "body{background:#000;margin:0;display:flex;flex-direction:column;"
        "align-items:center;justify-content:center;min-height:100vh;font:14px monospace;color:#0f0;}"
        "canvas{image-rendering:pixelated;border:1px solid #333;}"
        "nav{margin:12px;} nav a{color:#0af;margin:0 8px;}"
        "#status{margin:8px;color:#666;font-size:12px;}"
        "</style></head><body>"
        "<canvas id='c' width='512' height='192'></canvas>"
        "<div id='status'>Connecting...</div>"
        "<nav><a href='/'>Logs</a> <a href='/ota'>OTA</a></nav>"
        "<script>"
        "const W=128,H=48,S=4,canvas=document.getElementById('c'),"
        "ctx=canvas.getContext('2d'),st=document.getElementById('status');"
        "const img=ctx.createImageData(W*S,H*S);"
        "let frames=0,last=Date.now(),prev=null;"
        "async function loop(){"
        "try{const r=await fetch('/fb');"
        "if(!r.ok){st.textContent='No data';setTimeout(loop,200);return;}"
        "const buf=new Uint8Array(await r.arrayBuffer());"
        "if(buf.length<768){setTimeout(loop,200);return;}"
        // Skip render if framebuffer unchanged
        "let changed=!prev;"
        "if(!changed){for(let i=0;i<768;i++){if(buf[i]!==prev[i]){changed=true;break;}}}"
        "if(changed){"
        "for(let y=0;y<H;y++){const pg=(y>>3),bit=y&7,mask=1<<bit;"
        "for(let x=0;x<W;x++){const on=buf[pg*W+x]&mask?255:0;"
        "for(let sy=0;sy<S;sy++){const row=(y*S+sy)*W*S;"
        "for(let sx=0;sx<S;sx++){const i=(row+x*S+sx)*4;"
        "img.data[i]=on;img.data[i+1]=on;img.data[i+2]=on;img.data[i+3]=255;}}}}"
        "ctx.putImageData(img,0,0);prev=buf;}"
        "frames++;"
        "const now=Date.now();if(now-last>=1000){"
        "st.textContent=frames+' fps';frames=0;last=now;}"
        // Immediately poll again (sequential — no pileup)
        "setTimeout(loop,0);"
        "}catch(e){st.textContent='Error: '+e.message;setTimeout(loop,1000);}}"
        "loop();"
        "</script></body></html>");
}

static void handleFramebuffer() {
    if (!s_fbPtr || s_fbLen == 0) {
        server.send(204, "application/octet-stream", "");
        return;
    }
    server.sendHeader("Cache-Control", "no-cache");
    server.send_P(200, "application/octet-stream", (const char*)s_fbPtr, s_fbLen);
}

void debug_server_set_framebuffer(const uint8_t* buf, size_t len) {
    s_fbPtr = buf;
    s_fbLen = len;
}

void debug_server_init() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    debug_log("WiFi connecting to %s...", WIFI_SSID);

    // Non-blocking: we'll check status in task
    server.on("/", handleRoot);
    server.on("/display", handleDisplayPage);
    server.on("/fb", handleFramebuffer);
    server.on("/clear", handleClear);
    server.on("/log", handlePlain);
    server.on("/ota", HTTP_GET, handleOtaPage);
    server.on("/ota", HTTP_POST, handleOtaResult, handleOtaUpload);
    server.begin();

    debug_log("HTTP server started (waiting for WiFi...)");
}

static bool s_wifiConnected = false;

void debug_server_task() {
    server.handleClient();

    bool connected = (WiFi.status() == WL_CONNECTED);
    if (connected && !s_wifiConnected) {
        debug_log("WiFi connected! IP: %s", WiFi.localIP().toString().c_str());
        s_wifiConnected = true;
    } else if (!connected && s_wifiConnected) {
        debug_log("WiFi disconnected, reconnecting...");
        WiFi.reconnect();
        s_wifiConnected = false;
    }
}
