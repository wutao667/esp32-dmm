#include "MeterWeb.h"

#include <WebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <atomic>
#include <mutex>
#include "FlashLog.h"
#include "MeterPage.h"

namespace {
constexpr size_t kMaxPageRecords = 3600;
constexpr char kSetupSsid[] = "CoreS3-Setup";
std::atomic<uint32_t> webAddress{0};
std::mutex wifiNameMutex;
String wifiName;
std::atomic<bool> setupActive{false};
std::atomic<bool> resetRequested{false};
LogRecord page[kMaxPageRecords];
}

uint32_t meterWebAddress()
{
    return webAddress.load();
}

String meterWifiName()
{
    const std::lock_guard<std::mutex> lock(wifiNameMutex);
    return wifiName;
}

bool meterWifiSetupActive()
{
    return setupActive.load();
}

void meterRequestWifiReset()
{
    if (!setupActive.load()) {
        resetRequested.store(true);
    }
}

void meterWebTask(void* log)
{
    auto& flashLog = *static_cast<FlashLog*>(log);
    WiFiManager wifi;
    wifi.setDebugOutput(false);
    wifi.setTitle("CoreS3 Wi-Fi Setup");
    wifi.setConnectTimeout(15);
    wifi.setSaveConnectTimeout(15);
    wifi.setAPCallback([](WiFiManager*) { setupActive.store(true); });
    // The library owns the HTTP port until credentials connect successfully.
    while (!wifi.autoConnect(kSetupSsid)) {
        setupActive.store(false);
        delay(1000);
    }
    setupActive.store(false);
    WiFi.setAutoReconnect(true);

    WebServer server(80);
    server.on("/", HTTP_GET, [&] {
        server.send_P(200, "text/html; charset=utf-8", kMeterPage, sizeof(kMeterPage) - 1);
    });
    server.on("/api/live", HTTP_GET, [&] {
        float microamps;
        float volts;
        server.sendHeader("Cache-Control", "no-store");
        if (!meterLiveReading(microamps, volts)) {
            server.send(200, "application/json", "null");
            return;
        }
        char json[96];
        snprintf(json, sizeof(json), "{\"microamps\":%.6f,\"volts\":%.6f}", microamps, volts);
        server.send(200, "application/json", json);
    });
    server.on("/api/log", HTTP_GET, [&] {
        const long requested = server.hasArg("limit") ? server.arg("limit").toInt() : 900;
        size_t length = constrain(requested, 1L, static_cast<long>(kMaxPageRecords));
        size_t start = server.hasArg("start") ? std::max(0L, server.arg("start").toInt()) : SIZE_MAX;
        LogTotals totals;
        if (!flashLog.readPage(start, totals, page, length)) {
            server.send(503, "text/plain; charset=utf-8", "Flash unavailable");
            return;
        }
        server.sendHeader("Cache-Control", "no-store");
        server.sendHeader("X-Log-Count", String(totals.count));
        server.sendHeader("X-Log-Start", String(start));
        if (totals.count) {
            server.sendHeader("X-Log-Mean-Current", String(totals.microamps / totals.count, 9));
            server.sendHeader("X-Log-Mean-Voltage", String(totals.volts / totals.count, 9));
        }
        server.send_P(200, "application/octet-stream",
                      reinterpret_cast<const char*>(page), length * sizeof(LogRecord));
    });
    server.begin();
    uint32_t lastCheck = 0;
    for (;;) {
        if (resetRequested.exchange(false)) {
            // Erase the driver's saved configuration in NVS before restarting.
            if (WiFi.eraseAP()) {
                ESP.restart();
            }
            Serial.println("WEB Wi-Fi reset failed");
        }
        const uint32_t now = millis();
        if (now - lastCheck >= 250) {
            lastCheck = now;
            const uint32_t ip = WiFi.status() == WL_CONNECTED
                ? static_cast<uint32_t>(WiFi.localIP()) : 0;
            if (webAddress.load() != ip) {
                {
                    const std::lock_guard<std::mutex> lock(wifiNameMutex);
                    wifiName = ip ? WiFi.SSID() : String();
                }
                webAddress.store(ip);
                if (ip) {
                    Serial.printf("WEB http://%s/\n", IPAddress(ip).toString().c_str());
                }
            }
        }
        server.handleClient();
        delay(5);
    }
}
