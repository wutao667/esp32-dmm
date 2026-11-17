#include "MeterWeb.h"

#include <WebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_mac.h>
#include <atomic>
#include <mutex>
#include "BoardConfig.h"
#include "CurrentRange.h"
#include "FlashLog.h"
#include "MeterPage.h"
#include "MeterOta.h"
#include "MeterVersion.h"

namespace {
constexpr size_t kMaxPageRecords = 3600;
constexpr char kSetupPageStyle[] = R"HTML(<style>
body {margin:0; padding:16px; font:18px/1.5 system-ui,sans-serif}
.wrap {display:block; width:100%; min-width:0; max-width:500px; margin:0 auto}
h1 {font-size:clamp(24px,7vw,32px); overflow-wrap:anywhere}
h2,h3 {font-size:22px}
label {font-size:18px}
button,input[type='button'],input[type='submit'] {
  min-height:52px; padding:10px 14px; font:600 19px/1.25 system-ui,sans-serif;
}
input:not([type='checkbox']):not([type='radio']),select {
  min-height:48px; padding:10px 12px; font:18px/1.3 system-ui,sans-serif;
}
input[type='checkbox'] {width:22px; height:22px; vertical-align:middle}
a[data-ssid] {
  display:inline-flex; align-items:center; min-height:44px; max-width:calc(100% - 48px);
  overflow-wrap:anywhere;
}
</style>)HTML";
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

const char* meterSetupSsid()
{
    static const String ssid = [] {
        uint8_t mac[6]{};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char name[sizeof("Esp32DMM-FFFF")];
        snprintf(name, sizeof(name), "Esp32DMM-%02X%02X", mac[4], mac[5]);
        return String(name);
    }();
    return ssid.c_str();
}

const char* meterDeviceId()
{
    return meterSetupSsid() + sizeof("Esp32DMM-") - 1;
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
    wifi.setTitle(BoardConfig::setupTitle);
    const char* menu[] = {"wifi", "info"};
    wifi.setMenu(menu, sizeof(menu) / sizeof(menu[0]));
    wifi.setShowInfoUpdate(false);
    wifi.setCustomHeadElement(kSetupPageStyle);
    wifi.setWebServerCallback([&wifi] {
        // WiFiManager registers its built-in updater even when its menu entry is hidden.
        const auto reject = [&wifi] { wifi.server->send(404, "text/plain", "Not found"); };
        wifi.server->on("/update", HTTP_ANY, reject);
        wifi.server->on("/u", HTTP_ANY, reject);
    });
    wifi.setConnectTimeout(15);
    wifi.setSaveConnectTimeout(15);
    wifi.setAPCallback([](WiFiManager*) { setupActive.store(true); });
    // The library owns the HTTP port until credentials connect successfully.
    while (!wifi.autoConnect(meterSetupSsid())) {
        setupActive.store(false);
        delay(1000);
    }
    setupActive.store(false);
    WiFi.setAutoReconnect(true);

    WebServer server(80);
    const char* actionHeaders[] = {"X-Meter-Action", "X-Meter-Ota-Token"};
    server.collectHeaders(actionHeaders, sizeof(actionHeaders) / sizeof(actionHeaders[0]));
    server.on("/", HTTP_GET, [&] {
        server.send_P(200, "text/html; charset=utf-8", kMeterPage, sizeof(kMeterPage) - 1);
    });
    server.on("/api/live", HTTP_GET, [&] {
        float microamps;
        float volts;
        server.sendHeader("Cache-Control", "no-store");
        server.sendHeader("X-Meter-Id", meterDeviceId());
        server.sendHeader("X-Meter-Version", meterFirmwareVersion());
        server.sendHeader("X-Meter-Range", kCurrentRanges[currentRangeIndex()].id);
        if (!meterLiveReading(microamps, volts)) {
            server.send(200, "application/json", "null");
            return;
        }
        char json[96];
        snprintf(json, sizeof(json), "{\"microamps\":%.6f,\"volts\":%.6f}", microamps, volts);
        server.send(200, "application/json", json);
    });
    server.on("/api/range", HTTP_POST, [&] {
        server.sendHeader("Cache-Control", "no-store");
        if (server.header("X-Meter-Action") != "set-range") {
            server.send(403, "text/plain", "Confirmation required");
            return;
        }
        const String requested = server.arg("range");
        for (uint8_t i = 0; i < kCurrentRangeCount; ++i) {
            if (requested == kCurrentRanges[i].id) {
                if (saveCurrentRange(i)) {
                    server.send(200, "text/plain", kCurrentRanges[i].id);
                } else {
                    server.send(503, "text/plain", "Could not save range");
                }
                return;
            }
        }
        server.send(400, "text/plain", "Invalid range");
    });
    server.on("/api/log/clear", HTTP_POST, [&] {
        server.sendHeader("Cache-Control", "no-store");
        if (server.header("X-Meter-Action") != "clear-history") {
            server.send(403, "text/plain", "Confirmation required");
            return;
        }
        if (!meterClearHistory()) {
            server.send(503, "text/plain", "Clear could not be confirmed");
            return;
        }
        server.send(204, "text/plain", "");
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
        server.sendHeader("X-Log-Usage-Percent", String(totals.usagePercent));
        server.sendHeader("X-Log-Start", String(start));
        if (totals.count) {
            server.sendHeader("X-Log-Mean-Current", String(totals.microamps / totals.count, 9));
            server.sendHeader("X-Log-Mean-Voltage", String(totals.volts / totals.count, 9));
        }
        server.send_P(200, "application/octet-stream",
                      reinterpret_cast<const char*>(page), length * sizeof(LogRecord));
    });
    meterOtaInstallRoutes(server, flashLog);
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
                    if (!BoardConfig::hasRtc) {
                        configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
                    }
                    Serial.printf("WEB http://%s/\n", IPAddress(ip).toString().c_str());
                }
            }
        }
        server.handleClient();
        delay(5);
    }
}
