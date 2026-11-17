#include <M5Unified.h>
#include <IPAddress.h>
#include <Wire.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <atomic>
#include <cstdarg>
#include "BoardConfig.h"
#include "BoardUi.h"
#include "CurrentRange.h"
#include "FlashLog.h"
#include "MeterWeb.h"
#include "MeterOta.h"

namespace {
constexpr uint8_t kAddress = 0x41;
constexpr uint8_t kConfigRegister = 0x00;
constexpr uint8_t kShuntVoltageRegister = 0x01;
constexpr uint8_t kBusVoltageRegister = 0x02;
constexpr uint8_t kMaskEnableRegister = 0x06;
constexpr uint16_t kConversionReady = 1u << 3;
// Continuous shunt and bus conversion, 1.1 ms per channel, 16-sample averaging.
constexpr uint16_t kConfig = 0x4527;
constexpr uint32_t kDisplayUpdateMs = 200;
constexpr uint16_t kMuted = 0xB638;
constexpr uint16_t kCurrentColor = 0x67D5;

enum class MeterStatus { Connecting, Live, RangeLimit, SensorError };

struct Reading {
    float volts = 0;
    float microamps = 0;
    MeterStatus status = MeterStatus::Connecting;
    uint8_t range = 0;
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

void captureClock()
{
    const time_t utc = time(nullptr);
    if (utc >= kEarliestUtc) {
        utcAtBoot.store(static_cast<uint32_t>(utc) - uptimeSeconds());
    }
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
            } else if (strcmp(command, "LOG") == 0) {
                sendDiagnostic("LOG state=%d count=%lu %s\n", static_cast<int>(flashLog.state),
                               static_cast<unsigned long>(flashLog.count),
                               flashLog.state == FlashLog::State::Error ? flashLog.errorDetail() : "");
            } else if (strcmp(command, "WIFI RESET") == 0) {
                meterRequestWifiReset();
            } else if (strncmp(command, "TIME ", 5) == 0) {
                const long epoch = strtol(command + 5, &end, 10);
                if (*end == '\0' && epoch >= kEarliestUtc) {
                    const timeval now{epoch, 0};
                    settimeofday(&now, nullptr);
                    utcAtBoot.store(static_cast<uint32_t>(epoch) - uptimeSeconds());
                    if (BoardConfig::hasRtc && M5.Rtc.isEnabled()) {
                        const time_t seconds = epoch;
                        tm utc{};
                        gmtime_r(&seconds, &utc);
                        M5.Rtc.setDateTime(&utc);
                    }
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
    static uint8_t sampleRange = 0;
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
        if (samples && reading->range != sampleRange) {
            samples = 0;
            voltageSum = currentSum = 0;
        }
        if (!samples) {
            timestamp = utcAtBoot.load() + second;
            sampleRange = reading->range;
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

void displayTask(void*)
{
    flashLog.begin();
    meterOtaFinishBoot(flashLog);
    sendDiagnostic("LOG storage state=%d count=%lu\n", static_cast<int>(flashLog.state),
                   static_cast<unsigned long>(flashLog.count));
    uint32_t nextRefresh = millis();
    uint32_t recordFromSecond = 1;  // Skip the partial startup window.
    for (;;) {
        const auto input = BoardUi::poll();
        serviceSerialCommand();
        const uint32_t now = millis();
        if (confirmUntil && static_cast<int32_t>(now - confirmUntil) >= 0) {
            confirmUntil = 0;
            nextRefresh = now;
        }
        bool clearConfirmed = false;
        if (input == BoardUi::Input::ResetWifi) {
            confirmUntil = 0;
            if (!meterOtaBusy()) meterRequestWifiReset();
            nextRefresh = now;
        } else if (input == BoardUi::Input::NextRange) {
            confirmUntil = 0;
            if (!meterOtaBusy()) {
                const uint8_t nextRange = (currentRangeIndex() + 1) % kCurrentRangeCount;
                if (!saveCurrentRange(nextRange)) {
                    sendDiagnostic("Range change failed\n");
                }
            }
            nextRefresh = now;
        } else if (input == BoardUi::Input::ConfirmUpdate) {
            if (meterOtaConfirmButton()) {
                confirmUntil = 0;
                nextRefresh = now;
            }
        } else if (input == BoardUi::Input::Clear) {
#if defined(METER_BOARD_STICKS3)
            if (meterOtaBusy()) {
#else
            if (meterOtaConfirmButton() || meterOtaBusy()) {
#endif
                confirmUntil = 0;
            } else {
                clearConfirmed = confirmUntil != 0;
                confirmUntil = clearConfirmed ? 0 : now + 5000;
                if (!clearConfirmed) {
                    BoardUi::playClearFeedback(false);
                }
            }
            nextRefresh = now;
        } else if (input == BoardUi::Input::Cancel && confirmUntil) {
            confirmUntil = 0;
            nextRefresh = now;
        }
        auto pendingClear = ClearRequest::Pending;
        const bool clearFromWeb = webClear.compare_exchange_strong(pendingClear, ClearRequest::Working);
        if (clearConfirmed || clearFromWeb) {
            const bool cleared = flashLog.clear();
            if (cleared) {
                BoardUi::playClearFeedback(true);
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
        if (!BoardConfig::hasRtc && !utcAtBoot.load()) {
            captureClock();
        }

        Reading reading;
        xQueuePeek(latestReading, &reading, 0);
        const uint8_t rangeIndex = currentRangeIndex();
        const auto& range = kCurrentRanges[rangeIndex];
        char averageCurrent[16] = "--";
        char averageVoltage[16] = "--";
        uint8_t storagePercent = 0;
#if defined(METER_BOARD_STICKS3)
        const LogTotals totals = flashLog.snapshotTotals();
        storagePercent = totals.usagePercent;
        if (totals.count) {
            snprintf(averageCurrent, sizeof(averageCurrent), "%+.2f",
                     range.displayValue(totals.microamps / totals.count));
            snprintf(averageVoltage, sizeof(averageVoltage), "%.6f", totals.volts / totals.count);
        }
#endif
        const char* logIssue = meterOtaScreenStatus();
        if (!logIssue) {
            logIssue = flashLog.state == FlashLog::State::Full ? "FLASH FULL" :
                       flashLog.state == FlashLog::State::Error ? "FLASH ERROR" : nullptr;
        }
        if (reading.range == rangeIndex &&
            (reading.status == MeterStatus::Live || reading.status == MeterStatus::RangeLimit)) {
            char voltage[16];
            char current[16];
            snprintf(voltage, sizeof(voltage), "%.3f", reading.volts);
            snprintf(current, sizeof(current), "%+.2f", range.displayValue(reading.microamps));
            const bool overRange = reading.status == MeterStatus::RangeLimit;
            BoardUi::render(voltage, current, range.unit(), logIssue ? logIssue :
                            overRange ? BoardConfig::rangeStatus : BoardConfig::liveStatus,
                            logIssue || overRange ? TFT_ORANGE : kCurrentColor,
                            flashLog.count, !logIssue, confirmUntil != 0,
                            averageCurrent, averageVoltage, storagePercent, range.id);
        } else {
            const bool failed = reading.status == MeterStatus::SensorError;
            BoardUi::render("--", "--", range.unit(), logIssue ? logIssue :
                            failed ? BoardConfig::sensorError : BoardConfig::connectingStatus,
                            logIssue || failed ? TFT_ORANGE : kMuted,
                            flashLog.count, !logIssue, confirmUntil != 0,
                            averageCurrent, averageVoltage, storagePercent, range.id);
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
        reading.range != currentRangeIndex() ||
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
#if defined(METER_BOARD_STICKS3)
    config.internal_spk = true;
#else
    config.internal_spk = false;
#endif
    config.internal_rtc = BoardConfig::hasRtc;
    M5.begin(config);
#if defined(METER_BOARD_STICKS3)
    M5.Power.setExtOutput(true);
#endif
    loadCurrentRange();
    setenv("TZ", "UTC0", 1);
    tzset();
    if (BoardConfig::hasRtc && M5.Rtc.isEnabled() && !M5.Rtc.getVoltLow()) {
        M5.Rtc.setSystemTimeFromRtc();
        captureClock();
    }
    Serial.begin(115200);
    // HWCDC decrements its retry counter when the USB host stops reading.
    // A zero timeout can underflow that counter when the TX buffer is full.
    Serial.setTxTimeoutMs(2);
    BoardUi::begin();

    Wire.setTimeOut(25);
    latestReading = xQueueCreate(1, sizeof(Reading));
    pendingRecords = xQueueCreate(8, sizeof(PendingRecord));
    serialMessages = xQueueCreate(12, sizeof(SerialMessage));
    if (!latestReading || !pendingRecords || !serialMessages ||
        xTaskCreatePinnedToCore(serialTask, "meter-serial", 4096, nullptr, 1, nullptr, 0) != pdPASS ||
        xTaskCreatePinnedToCore(displayTask, "meter-display", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
        BoardUi::render("--", "--", kCurrentRanges[currentRangeIndex()].unit(),
                        "Display task failed", TFT_ORANGE, 0, false, false);
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
        i2cStarted = Wire.begin(BoardConfig::sensorSda, BoardConfig::sensorScl, 100000);
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
    reading.range = currentRangeIndex();
    const auto& range = kCurrentRanges[reading.range];
    reading.microamps = range.microamps(static_cast<int16_t>(shuntRaw));
    const bool overRange = busRaw > 24000 || shuntRaw == 0x7FFF || shuntRaw == 0x8000 ||
                           fabsf(reading.microamps) > range.maxMicroamps;
    reading.status = overRange ? MeterStatus::RangeLimit : MeterStatus::Live;
    recordSample(&reading);
    xQueueOverwrite(latestReading, &reading);
    sendDiagnostic("V=%.3f V  I=%+.2f %s\n", reading.volts,
                   range.displayValue(reading.microamps), range.unit());
}
