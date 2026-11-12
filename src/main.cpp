#include <M5Unified.h>
#include <IPAddress.h>
#include <Wire.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <atomic>
#include <cstdarg>
#include "FlashLog.h"
#include "MeterWeb.h"

namespace {
constexpr int kSda = 9;
constexpr int kScl = 8;
constexpr uint8_t kAddress = 0x41;
constexpr uint8_t kConfigRegister = 0x00;
constexpr uint8_t kShuntVoltageRegister = 0x01;
constexpr uint8_t kBusVoltageRegister = 0x02;
constexpr uint8_t kMaskEnableRegister = 0x06;
constexpr uint16_t kConversionReady = 1u << 3;
// Continuous shunt and bus conversion, 1.1 ms per channel, 16-sample averaging.
constexpr uint16_t kConfig = 0x4527;
constexpr float kShuntOhms = 10.0f;
constexpr float kShuntMicrovoltsPerCount = 2.5f;
constexpr uint32_t kDisplayUpdateMs = 200;
constexpr int kScreenInset = 20;
constexpr int kPanelPadding = 12;
constexpr int kClearWidth = 92;
constexpr int kClearTop = 3;
constexpr int kClearHeight = 28;

constexpr uint16_t kBackground = 0x0842;
constexpr uint16_t kPanel = 0x1126;
constexpr uint16_t kMuted = 0xB638;
constexpr uint16_t kVoltageColor = 0x6EBF;
constexpr uint16_t kCurrentColor = 0x67D5;

M5Canvas frame(&M5.Display);
bool bufferedDisplay = false;

enum class MeterStatus { Connecting, Live, RangeLimit, SensorError };

struct Reading {
    float volts = 0;
    float microamps = 0;
    MeterStatus status = MeterStatus::Connecting;
};

QueueHandle_t latestReading;
QueueHandle_t pendingRecords;
QueueHandle_t serialMessages;
FlashLog flashLog;
uint32_t confirmUntil = 0;
enum class ClearRequest { Idle, Pending, Working, Succeeded, Failed };
std::atomic<ClearRequest> webClear{ClearRequest::Idle};
constexpr time_t kEarliestUtc = 1577836800;  // 2020-01-01
std::atomic<uint32_t> utcAtBoot{0};

struct SerialMessage {
    char text[96];
};

void sendDiagnostic(const char* format, ...)
{
    SerialMessage message{};
    va_list args;
    va_start(args, format);
    vsnprintf(message.text, sizeof(message.text), format, args);
    va_end(args);
    xQueueSend(serialMessages, &message, 0);
}

void serialTask(void*)
{
    SerialMessage message;
    for (;;) {
        if (xQueueReceive(serialMessages, &message, portMAX_DELAY) == pdPASS) {
            Serial.print(message.text);
        }
    }
}

struct PendingRecord {
    uint32_t uptimeSecond;
    LogRecord data;
};

uint32_t uptimeSeconds()
{
    return esp_timer_get_time() / 1000000;
}

void serviceSerialCommand()
{
    static char command[32];
    static size_t length = 0;
    while (Serial.available()) {
        const int ch = Serial.read();
        if (ch == '\n' || ch == '\r') {
            command[length] = '\0';
            char* end;
            if (strcmp(command, "IP") == 0) {
                const uint32_t address = meterWebAddress();
                sendDiagnostic("WEB %s\n", IPAddress(address).toString().c_str());
            } else if (strcmp(command, "WIFI RESET") == 0) {
                meterRequestWifiReset();
            } else if (strncmp(command, "TIME ", 5) == 0) {
                const long epoch = strtol(command + 5, &end, 10);
                if (*end == '\0' && epoch >= kEarliestUtc) {
                    const timeval now{epoch, 0};
                    settimeofday(&now, nullptr);
                    utcAtBoot.store(static_cast<uint32_t>(epoch) - uptimeSeconds());
                    const time_t seconds = epoch;
                    tm utc{};
                    gmtime_r(&seconds, &utc);
                    M5.Rtc.setDateTime(&utc);
                    sendDiagnostic("TIME OK %ld\n", epoch);
                }
            }
            length = 0;
        } else if (length + 1 < sizeof(command)) {
            command[length++] = ch;
        }
    }
}

void recordSample(const Reading* reading = nullptr)
{
    static uint32_t second = 0;
    static uint32_t timestamp = 0;
    static float voltageSum = 0;
    static float currentSum = 0;
    static unsigned samples = 0;
    const int64_t elapsedUs = esp_timer_get_time();
    const uint32_t now = elapsedUs / 1000000;
    if (now != second) {
        if (samples) {
            const PendingRecord record{second, {timestamp, voltageSum / samples,
                                                currentSum / samples}};
            if (xQueueSend(pendingRecords, &record, 0) != pdPASS) {
                sendDiagnostic("LOG queue full\n");
            }
        }
        second = now;
        samples = 0;
        voltageSum = currentSum = 0;
    }
    if (reading) {
        if (!samples) {
            timestamp = utcAtBoot.load() + second;
        }
        voltageSum += reading->volts;
        currentSum += reading->microamps;
        ++samples;
    }
}

bool configureSensor()
{
    Wire.beginTransmission(kAddress);
    Wire.write(kConfigRegister);
    Wire.write(static_cast<uint8_t>(kConfig >> 8));
    Wire.write(static_cast<uint8_t>(kConfig));
    return Wire.endTransmission() == 0;
}

bool readRegister(uint8_t reg, uint16_t& value)
{
    Wire.beginTransmission(kAddress);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0 ||
        Wire.requestFrom(kAddress, uint8_t{2}) != 2) {
        return false;
    }

    const uint16_t highByte = Wire.read();
    value = (highByte << 8) | static_cast<uint16_t>(Wire.read());
    return true;
}

void drawReading(m5gfx::LovyanGFX& display, int y, const char* label,
                 const char* value, const char* unit, uint16_t color)
{
    const int panelWidth = display.width() - 2 * kScreenInset;
    const int textLeft = kScreenInset + kPanelPadding;
    const int textRight = display.width() - textLeft;
    display.fillRoundRect(kScreenInset, y, panelWidth, 76, 9, kPanel);
    display.setFont(&fonts::Font2);
    display.setTextColor(kMuted);
    display.drawString(label, textLeft, y + 7);
    display.drawRightString(unit, textRight, y + 7);
    display.setFont(&fonts::FreeMonoBold24pt7b);
    display.setTextColor(color);
    const int valueWidth = display.textWidth(value);
    const int availableWidth = textRight - textLeft;
    if (valueWidth > availableWidth) {
        display.setTextSize(static_cast<float>(availableWidth) / valueWidth, 1);
    }
    display.drawString(value, textLeft, y + 25);
    display.setTextSize(1);
}

void showReadings(const char* voltage, const char* current,
                  const char* status, uint16_t statusColor)
{
    m5gfx::LovyanGFX& display = bufferedDisplay
        ? static_cast<m5gfx::LovyanGFX&>(frame)
        : static_cast<m5gfx::LovyanGFX&>(M5.Display);
    display.startWrite();
    display.fillScreen(kBackground);
    display.setTextSize(1);
    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE);
    const int right = display.width() - kScreenInset;
    const int clearLeft = right - kClearWidth;
    display.drawString(confirmUntil ? "Delete all records?" : "INA226  /  DC METER",
                       kScreenInset, 10);
    display.fillRoundRect(clearLeft, kClearTop, kClearWidth, kClearHeight, 5,
                          confirmUntil ? TFT_RED : kPanel);
    display.drawCenterString(confirmUntil ? "CONFIRM" : "CLEAR", clearLeft + kClearWidth / 2, 9);
    drawReading(display, 34, "BUS VOLTAGE", voltage, "V", kVoltageColor);
    drawReading(display, 118, "CURRENT", current, "uA", kCurrentColor);
    display.setFont(&fonts::Font2);
    char count[24];
    snprintf(count, sizeof(count), "REC %lu", static_cast<unsigned long>(flashLog.count));
    display.setTextColor(flashLog.state == FlashLog::State::Ready ? kCurrentColor : TFT_ORANGE);
    display.drawRightString(count, right, 200);
    const int statusWidth = right - display.textWidth(count) - 12 - kScreenInset;
    const int fullStatusWidth = display.textWidth(status);
    if (fullStatusWidth > statusWidth) {
        display.setTextSize(static_cast<float>(statusWidth) / fullStatusWidth, 1);
    }
    display.setTextColor(statusColor);
    display.drawString(status, kScreenInset, 200);
    display.setTextSize(1);
    display.setTextColor(kMuted);
    const uint32_t address = meterWebAddress();
    if (meterWifiSetupActive()) {
        display.drawString("CoreS3-Setup  192.168.4.1", kScreenInset, 220);
    } else if (address) {
        const String network = meterWifiName() + "  " + IPAddress(address).toString();
        const int available = right - kScreenInset;
        const int width = display.textWidth(network);
        if (width > available) {
            display.setTextSize(static_cast<float>(available) / width, 1);
        }
        display.drawString(network, kScreenInset, 220);
        display.setTextSize(1);
    } else {
        display.drawString("Wi-Fi connecting...", kScreenInset, 220);
    }
    display.endWrite();
    if (bufferedDisplay) {
        frame.pushSprite(0, 0);
    }
}

void displayTask(void*)
{
    flashLog.begin();
    sendDiagnostic("LOG storage state=%d count=%lu\n", static_cast<int>(flashLog.state),
                   static_cast<unsigned long>(flashLog.count));
    uint32_t nextRefresh = millis();
    uint32_t recordFromSecond = 1;  // Skip the partial startup window.
    for (;;) {
        M5.update();
        serviceSerialCommand();
        const uint32_t now = millis();
        if (confirmUntil && static_cast<int32_t>(now - confirmUntil) >= 0) {
            confirmUntil = 0;
            nextRefresh = now;
        }
        const auto touch = M5.Touch.getDetail();
        const int right = M5.Display.width() - kScreenInset;
        const bool clearTouched = touch.wasPressed() &&
            touch.x >= right - kClearWidth && touch.x < right &&
            touch.y >= kClearTop && touch.y < kClearTop + kClearHeight;
        bool clearConfirmed = false;
        if (M5.BtnPWR.wasHold()) {
            confirmUntil = 0;
            meterRequestWifiReset();
            nextRefresh = now;
        } else if (clearTouched || M5.BtnPWR.wasClicked()) {
            clearConfirmed = confirmUntil != 0;
            confirmUntil = clearConfirmed ? 0 : now + 5000;
            nextRefresh = now;
        } else if (touch.wasPressed() && confirmUntil) {
            confirmUntil = 0;
            nextRefresh = now;
        }
        auto pendingClear = ClearRequest::Pending;
        const bool clearFromWeb = webClear.compare_exchange_strong(pendingClear, ClearRequest::Working);
        if (clearConfirmed || clearFromWeb) {
            const bool cleared = flashLog.clear();
            if (cleared) {
                recordFromSecond = uptimeSeconds() + 1;
                sendDiagnostic("LOG clear count=%lu\n", static_cast<unsigned long>(flashLog.count));
            }
            confirmUntil = 0;
            nextRefresh = now;
            if (clearFromWeb) {
                webClear.store(cleared ? ClearRequest::Succeeded : ClearRequest::Failed);
            }
        }

        PendingRecord pending;
        while (xQueueReceive(pendingRecords, &pending, 0) == pdPASS) {
            if (pending.uptimeSecond >= recordFromSecond) {
                const size_t previousCount = flashLog.count;
                flashLog.append(pending.data);
                if (flashLog.count != previousCount) {
                    sendDiagnostic("LOG count=%lu t=%lu V=%.6f I=%+.6f\n",
                                  static_cast<unsigned long>(flashLog.count),
                                  static_cast<unsigned long>(pending.data.timestamp),
                                  pending.data.volts, pending.data.microamps);
                }
            }
        }
        if (static_cast<int32_t>(now - nextRefresh) < 0) {
            delay(15);
            continue;
        }
        nextRefresh = now + kDisplayUpdateMs;

        Reading reading;
        xQueuePeek(latestReading, &reading, 0);
        const char* logIssue = flashLog.state == FlashLog::State::Full ? "FLASH FULL" :
                               flashLog.state == FlashLog::State::Error ? "FLASH ERROR" : nullptr;
        if (reading.status == MeterStatus::Live || reading.status == MeterStatus::RangeLimit) {
            char voltage[16];
            char current[16];
            snprintf(voltage, sizeof(voltage), "%.3f", reading.volts);
            snprintf(current, sizeof(current), "%+.2f", reading.microamps);
            const bool overRange = reading.status == MeterStatus::RangeLimit;
            showReadings(voltage, current, logIssue ? logIssue :
                         overRange ? "PORT B  /  Range limit" : "PORT B  /  LIVE",
                         logIssue || overRange ? TFT_ORANGE : kCurrentColor);
        } else {
            const bool failed = reading.status == MeterStatus::SensorError;
            showReadings("--", "--", logIssue ? logIssue :
                         failed ? "PORT B  /  Check INA226 cable" : "PORT B  /  Connecting...",
                         logIssue || failed ? TFT_ORANGE : kMuted);
        }
        delay(15);
    }
}
}  // namespace

bool meterClearHistory()
{
    if (webClear.load() == ClearRequest::Working) {
        return false;
    }
    webClear.store(ClearRequest::Pending);
    const uint32_t start = millis();
    while (millis() - start < 4000) {
        const auto state = webClear.load();
        if (state == ClearRequest::Succeeded || state == ClearRequest::Failed) {
            return state == ClearRequest::Succeeded;
        }
        delay(10);
    }
    auto pending = ClearRequest::Pending;
    webClear.compare_exchange_strong(pending, ClearRequest::Idle);
    return false;
}

bool meterLiveReading(float& microamps, float& volts)
{
    Reading reading;
    if (xQueuePeek(latestReading, &reading, 0) != pdPASS ||
        (reading.status != MeterStatus::Live && reading.status != MeterStatus::RangeLimit)) {
        return false;
    }
    microamps = reading.microamps;
    volts = reading.volts;
    return true;
}

void setup()
{
    auto config = M5.config();
    config.internal_imu = false;
    config.internal_mic = false;
    config.internal_spk = false;
    M5.begin(config);
    setenv("TZ", "UTC0", 1);
    tzset();
    if (M5.Rtc.isEnabled() && !M5.Rtc.getVoltLow()) {
        M5.Rtc.setSystemTimeFromRtc();
        const time_t utc = time(nullptr);
        if (utc >= kEarliestUtc) {
            utcAtBoot.store(static_cast<uint32_t>(utc) - uptimeSeconds());
        }
    }
    Serial.begin(115200);
    // HWCDC decrements its retry counter when the USB host stops reading.
    // A zero timeout can underflow that counter when the TX buffer is full.
    Serial.setTxTimeoutMs(2);
    M5.Display.setRotation(1);
    if (M5.Display.width() < M5.Display.height()) {
        M5.Display.setRotation(0);
    }
    M5.Display.setBrightness(160);
    frame.setColorDepth(16);
    bufferedDisplay = frame.createSprite(M5.Display.width(), M5.Display.height()) != nullptr;

    Wire.setTimeOut(25);
    latestReading = xQueueCreate(1, sizeof(Reading));
    pendingRecords = xQueueCreate(8, sizeof(PendingRecord));
    serialMessages = xQueueCreate(12, sizeof(SerialMessage));
    if (!latestReading || !pendingRecords || !serialMessages ||
        xTaskCreatePinnedToCore(serialTask, "meter-serial", 4096, nullptr, 1, nullptr, 0) != pdPASS ||
        xTaskCreatePinnedToCore(displayTask, "meter-display", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
        showReadings("--", "--", "Display task failed", TFT_ORANGE);
        for (;;) {
            delay(1000);
        }
    }
    if (xTaskCreatePinnedToCore(meterWebTask, "meter-web", 8192, &flashLog, 1, nullptr, 0) != pdPASS) {
        sendDiagnostic("WEB task failed\n");
    }
}

void loop()
{
    recordSample();
    static bool i2cStarted = false;
    static bool sensorConfigured = false;
    if (!i2cStarted) {
        i2cStarted = Wire.begin(kSda, kScl, 100000);
    }
    if (!sensorConfigured) {
        sensorConfigured = i2cStarted && configureSensor();
        Reading reading;
        if (!sensorConfigured) {
            reading.status = MeterStatus::SensorError;
        }
        xQueueOverwrite(latestReading, &reading);
        delay(sensorConfigured ? 1 : 200);
        return;
    }

    uint16_t flags;
    const bool flagsRead = readRegister(kMaskEnableRegister, flags);
    if (flagsRead && !(flags & kConversionReady)) {
        delay(1);
        return;
    }

    uint16_t activeConfig;
    uint16_t shuntRaw;
    uint16_t busRaw;
    if (!flagsRead || !readRegister(kConfigRegister, activeConfig) ||
        activeConfig != kConfig ||
        !readRegister(kShuntVoltageRegister, shuntRaw) ||
        !readRegister(kBusVoltageRegister, busRaw)) {
        sensorConfigured = false;
        Reading reading;
        reading.status = MeterStatus::SensorError;
        xQueueOverwrite(latestReading, &reading);
        sendDiagnostic("INA226 read failed\n");
        return;
    }

    Reading reading;
    reading.volts = busRaw * 0.00125f;
    reading.microamps =
        static_cast<int16_t>(shuntRaw) * kShuntMicrovoltsPerCount / kShuntOhms;
    const bool overRange = busRaw > 24000 || shuntRaw == 0x7FFF || shuntRaw == 0x8000;
    reading.status = overRange ? MeterStatus::RangeLimit : MeterStatus::Live;
    recordSample(&reading);
    xQueueOverwrite(latestReading, &reading);
    sendDiagnostic("V=%.3f V  I=%+.2f uA\n", reading.volts, reading.microamps);
}
