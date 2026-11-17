#include <M5Unified.h>
#include <IPAddress.h>
#include <time.h>
#include "BoardConfig.h"
#include "BoardUi.h"
#include "MeterWeb.h"

namespace {
M5Canvas frame(&M5.Display);
bool bufferedDisplay = false;

m5gfx::LovyanGFX& target()
{
    return bufferedDisplay ? static_cast<m5gfx::LovyanGFX&>(frame)
                           : static_cast<m5gfx::LovyanGFX&>(M5.Display);
}

void pushFrame()
{
    if (bufferedDisplay) {
        frame.pushSprite(0, 0);
    }
}

#if defined(METER_BOARD_CORES3)
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


#else
constexpr uint16_t kBackground = 0x0842;
constexpr uint16_t kMuted = 0xB638;
constexpr uint16_t kVoltageColor = 0x6EBF;
constexpr uint16_t kCurrentColor = 0x67D5;

#if defined(METER_BOARD_STICKS3)
constexpr int kVoltageTop = 67;
constexpr int kRecordTop = 180;
constexpr int kWifiTop = 202;
constexpr int kIpTop = 221;
constexpr uint16_t kRecordColor = TFT_ORANGE;
constexpr uint32_t kScreenTimeoutMs = 30000;
constexpr uint32_t kDoubleClickMs = 350;
constexpr uint32_t kShutdownMessageMs = 3000;
constexpr uint8_t kPowerManagerAddress = 0x6e;
constexpr uint8_t kPowerButtonConfig1 = 0x49;
constexpr uint8_t kPowerButtonConfig2 = 0x4a;
constexpr uint8_t kDisableSingleClickReset = 1;
constexpr uint8_t kDisableImmediateDoubleClickPowerOff = 1;
constexpr uint8_t kScreenBrightness = 160;
uint32_t lastScreenActivity = 0;
bool screenAsleep = false;
bool screenAlwaysOn = false;
bool aClickPending = false;
uint32_t lastAClick = 0;
bool bClickPending = false;
bool wifiResetAfterBeep = false;
uint32_t lastBClick = 0;
bool displayRailOff = false;
bool shutdownPending = false;
uint32_t shutdownStarted = 0;

enum class Beep { Single, Double, Hold, Confirm };
enum class SpeakerState { Off, Priming, DoubleGap, Finishing };
constexpr uint32_t kShortBeepMs = 95;
constexpr uint32_t kSingleClickBeepMs = 135;
constexpr uint32_t kSpeakerTailMs = 65;
constexpr int kSpeakerChannel = 0;
SpeakerState speakerState = SpeakerState::Off;
uint32_t speakerDeadline = 0;
uint8_t silentPasses = 0;
Beep pendingBeep = Beep::Single;

void cutDisplayRail()
{
    displayRailOff = M5.Power.M5pm1.setGPIOOutput(m5::M5PM1_Class::gpio2, false);
}

bool playSilence()
{
    // Start I2S clocks with a finite silent clip.
    static constexpr int16_t silence[256]{};
    return M5.Speaker.playRaw(silence, sizeof(silence) / sizeof(silence[0]),
                              22050, false, 12, kSpeakerChannel);
}

void startTone(Beep beep, bool afterSilence = false)
{
    uint32_t duration = kShortBeepMs;
    if (beep == Beep::Single) {
        duration = kSingleClickBeepMs;
    } else if (beep == Beep::Hold) {
        duration = 180;
    }
    const float frequency = beep == Beep::Hold ? 650 : beep == Beep::Confirm ? 1800 : 1200;
    M5.Speaker.tone(frequency, duration, kSpeakerChannel, !afterSilence);
    speakerState = beep == Beep::Double ? SpeakerState::DoubleGap : SpeakerState::Finishing;
    speakerDeadline = millis() + duration + kSpeakerTailMs;
}

void playBeep(Beep beep)
{
    if (displayRailOff) {
        return;
    }
    if (speakerState == SpeakerState::Priming) {
        pendingBeep = beep;
        return;
    }
    if (speakerState != SpeakerState::Off) {
        startTone(beep);
        return;
    }
    if (M5.Speaker.begin()) {
        M5.Speaker.setVolume(180);
        if (playSilence()) {
            silentPasses = 1;
            pendingBeep = beep;
            speakerState = SpeakerState::Priming;
            speakerDeadline = millis();
        } else {
            startTone(beep);
        }
    }
}

void updateSpeaker(uint32_t now)
{
    if (speakerState == SpeakerState::Off || static_cast<int32_t>(now - speakerDeadline) < 0) {
        return;
    }
    if (M5.Speaker.isPlaying(kSpeakerChannel)) {
        return;
    }
    if (speakerState == SpeakerState::Priming) {
        const bool queued = playSilence();
        if (!queued || ++silentPasses == 3) {
            startTone(pendingBeep, queued);
        }
        return;
    }
    if (speakerState == SpeakerState::DoubleGap) {
        M5.Speaker.tone(1550, kShortBeepMs, kSpeakerChannel);
        speakerState = SpeakerState::Finishing;
        speakerDeadline = now + kShortBeepMs + kSpeakerTailMs;
        return;
    }
    M5.Speaker.end();
    speakerState = SpeakerState::Off;
    if (screenAsleep && !displayRailOff) {
        cutDisplayRail();
    }
}

void sleepScreen()
{
    if (screenAsleep) {
        return;
    }
    M5.Display.sleep();
    screenAsleep = true;
    if (speakerState == SpeakerState::Off) {
        cutDisplayRail();
    }
}

void wakeScreen(uint32_t now)
{
    lastScreenActivity = now;
    if (!screenAsleep) {
        return;
    }
    if (displayRailOff) {
        if (!M5.Power.M5pm1.setGPIOOutput(m5::M5PM1_Class::gpio2, true)) {
            return;
        }
        displayRailOff = false;
        const auto rotation = M5.Display.getRotation();
        auto* panel = M5.Display.getPanel();
        // L3B also feeds the LCD, so restore its controller after power returns.
        delay(10);
        panel->init(true);
        M5.Display.invertDisplay(panel->getInvert());
        M5.Display.setRotation(rotation);
        M5.Display.setBrightness(kScreenBrightness);
    } else {
        M5.Display.wakeup();
    }
    screenAsleep = false;
}

void showShutdownMessage()
{
    auto& display = target();
    display.startWrite();
    display.fillScreen(kBackground);
    display.setFont(&fonts::efontCN_16);
    display.setTextSize(2);
    display.setTextColor(TFT_WHITE);
    const int center = display.width() / 2;
    const int titleTop = display.height() / 2 - display.fontHeight();
    display.drawCenterString("关机中", center, titleTop);
    display.setTextSize(1);
    display.setTextColor(kMuted);
    display.drawCenterString("短按左侧", center, display.height() / 2 + 8);
    display.drawCenterString("BOOT键开机", center, display.height() / 2 + 28);
    display.endWrite();
    pushFrame();
}

int batteryPercent = -1;

void sampleBattery(uint32_t now)
{
    constexpr uint32_t kSampleIntervalMs = 5000;
    constexpr size_t kWindowSamples = 120000 / kSampleIntervalMs;
    // Zero denotes a missing sample; valid percentages are stored as 1–101.
    static uint8_t samples[kWindowSamples]{};
    static size_t nextSlot = 0;
    static uint32_t nextRead = now;
    if (static_cast<int32_t>(now - nextRead) < 0) {
        return;
    }
    const uint32_t elapsedSlots = (now - nextRead) / kSampleIntervalMs + 1;
    nextRead += elapsedSlots * kSampleIntervalMs;
    const size_t slotsToFill = std::min<size_t>(elapsedSlots, kWindowSamples);
    for (size_t slot = 1; slot < slotsToFill; ++slot) {
        samples[nextSlot] = 0;
        nextSlot = (nextSlot + 1) % kWindowSamples;
    }
    const int level = M5.Power.getBatteryLevel();
    samples[nextSlot] = level >= 0 && level <= 100 ? level + 1 : 0;
    nextSlot = (nextSlot + 1) % kWindowSamples;

    int sum = 0;
    int validSamples = 0;
    for (const uint8_t sample : samples) {
        if (sample) {
            sum += sample - 1;
            ++validSamples;
        }
    }
    batteryPercent = validSamples ? (sum + validSamples / 2) / validSamples : -1;
}

void drawStatusBar(m5gfx::LovyanGFX& display, const char* dateTime)
{
    char battery[8] = "--%";
    if (batteryPercent >= 0) {
        snprintf(battery, sizeof(battery), "%d%%", batteryPercent);
    }
    display.fillRect(0, 0, display.width(), 17, 0x1126);
    display.drawString(dateTime, 5, 4);
    display.setTextColor(TFT_WHITE);
    const int batteryRight = display.width() - 5;
    if (screenAlwaysOn) {
        const int dotX = batteryRight - display.textWidth(battery) - 6;
        display.fillCircle(dotX, 8, 2, kCurrentColor);
    }
    display.drawRightString(battery, batteryRight, 4);
}
#else
constexpr int kVoltageTop = 54;
constexpr int kRecordTop = 72;
constexpr int kWifiTop = 91;
constexpr int kIpTop = 110;
constexpr uint16_t kRecordColor = TFT_GOLD;
#endif

void centerFit(m5gfx::LovyanGFX& display, const char* text, int y)
{
    const int width = display.textWidth(text);
    const int available = display.width() - 10;
    if (width > available) {
        display.setTextSize(static_cast<float>(available) / width, 1);
    }
    display.drawCenterString(text, display.width() / 2, y);
    display.setTextSize(1);
}

void drawWifiName(m5gfx::LovyanGFX& display, const String& name)
{
    constexpr int top = kWifiTop;
    const int width = display.textWidth(name);
    const int available = display.width() - 10;
    if (width <= available) {
        centerFit(display, name.c_str(), top);
        return;
    }
    const int offset = (millis() / 70) % (width + 24);
    display.setClipRect(5, top, available, display.fontHeight());
    display.drawString(name, 5 - offset, top);
    display.drawString(name, 5 - offset + width + 24, top);
    display.clearClipRect();
}
#endif
}  // namespace

void BoardUi::playClearFeedback(bool cleared)
{
#if defined(METER_BOARD_STICKS3)
    playBeep(cleared ? Beep::Confirm : Beep::Single);
#else
    (void)cleared;
#endif
}

void BoardUi::begin()
{
#if defined(METER_BOARD_CORES3)
    M5.Display.setRotation(1);
    if (M5.Display.width() < M5.Display.height()) {
        M5.Display.setRotation(0);
    }

#elif defined(METER_BOARD_STICKS3)
    M5.Display.setRotation(0);
    if (M5.Display.width() > M5.Display.height()) {
        M5.Display.setRotation(1);
    }
    M5.BtnA.setHoldThresh(1500);
    M5.BtnB.setHoldThresh(1500);
    lastScreenActivity = millis();
    auto& pmic = M5.Power.M5pm1;
    pmic.setGPIOOutput(m5::M5PM1_Class::gpio3, false);
    // The PMIC clears its click flags when no pin is assigned to IRQ.
    if (pmic.setGPIOFunction(m5::M5PM1_Class::gpio1, m5::M5PM1_Class::irq) &&
        M5.In_I2C.bitOn(kPowerManagerAddress, kPowerButtonConfig1, kDisableSingleClickReset, 100000)) {
        M5.In_I2C.bitOn(kPowerManagerAddress, kPowerButtonConfig2,
                        kDisableImmediateDoubleClickPowerOff, 100000);
    }
#else
    M5.Display.setRotation(0);
    M5.BtnA.setHoldThresh(1500);
#endif
#if defined(METER_BOARD_STICKS3)
    M5.Display.setBrightness(kScreenBrightness);
#else
    M5.Display.setBrightness(160);
#endif
    frame.setColorDepth(16);
    bufferedDisplay = frame.createSprite(M5.Display.width(), M5.Display.height()) != nullptr;
}

BoardUi::Input BoardUi::poll()
{
    M5.update();
#if defined(METER_BOARD_CORES3)
    if (M5.BtnPWR.wasHold()) {
        return Input::ResetWifi;
    }
    if (M5.BtnPWR.wasClicked()) {
        return Input::Clear;
    }
    const auto touch = M5.Touch.getDetail();
    if (touch.wasPressed()) {
        const int right = M5.Display.width() - kScreenInset;
        if (touch.x >= right - kClearWidth && touch.x < right &&
            touch.y >= kClearTop && touch.y < kClearTop + kClearHeight) {
            return Input::Clear;
        }
        return Input::Cancel;
    }
#elif defined(METER_BOARD_STICKS3)
    const uint32_t now = millis();
    updateSpeaker(now);
    sampleBattery(now);
    const bool powerDoubleClick = M5.Power.M5pm1.wasPekDoubleClicked();
    if (shutdownPending) {
        if (now - shutdownStarted >= kShutdownMessageMs) {
            M5.Power.M5pm1.powerOff();
            shutdownPending = false;
        }
        return Input::None;
    }
    if (powerDoubleClick) {
        wakeScreen(now);
        playBeep(Beep::Double);
        showShutdownMessage();
        shutdownStarted = millis();
        shutdownPending = true;
        return Input::None;
    }
    if (wifiResetAfterBeep && speakerState == SpeakerState::Off) {
        wifiResetAfterBeep = false;
        return Input::ResetWifi;
    }
    if (M5.BtnPWR.wasClicked()) {
        playBeep(Beep::Single);
        return Input::ConfirmUpdate;
    }
    if (M5.BtnA.wasHold()) {
        screenAlwaysOn = false;
        aClickPending = false;
        playBeep(Beep::Hold);
        sleepScreen();
    } else if (M5.BtnA.wasClicked()) {
        wakeScreen(now);
        if (aClickPending && now - lastAClick <= kDoubleClickMs) {
            screenAlwaysOn = true;
            aClickPending = false;
            playBeep(Beep::Double);
        } else {
            screenAlwaysOn = false;
            lastAClick = now;
            aClickPending = true;
        }
    } else if (M5.BtnB.wasPressed()) {
        wakeScreen(now);
    } else if (!screenAlwaysOn && !M5.BtnA.isPressed() && !M5.BtnB.isPressed() &&
               now - lastScreenActivity >= kScreenTimeoutMs) {
        sleepScreen();
    }
    if (aClickPending && !M5.BtnA.isPressed() && now - lastAClick > kDoubleClickMs) {
        aClickPending = false;
        playBeep(Beep::Single);
    }
    if (M5.BtnB.wasHold()) {
        bClickPending = false;
        playBeep(Beep::Hold);
        if (speakerState != SpeakerState::Off) {
            wifiResetAfterBeep = true;
            return Input::None;
        }
        return Input::ResetWifi;
    }
    if (M5.BtnB.wasClicked()) {
        if (bClickPending && now - lastBClick <= kDoubleClickMs) {
            bClickPending = false;
            playBeep(Beep::Double);
            return Input::NextRange;
        }
        bClickPending = true;
        lastBClick = now;
    } else if (bClickPending && !M5.BtnB.isPressed() && now - lastBClick > kDoubleClickMs) {
        bClickPending = false;
        return Input::Clear;
    }
#else
    if (M5.BtnA.wasHold()) {
        return Input::ResetWifi;
    }
    if (M5.BtnA.wasClicked()) {
        return Input::Clear;
    }
#endif
    return Input::None;
}

#if defined(METER_BOARD_CORES3)
void BoardUi::render(const char* voltage, const char* current, const char* currentUnit, const char* status,
                     uint16_t statusColor, size_t recordCount, bool logReady, bool confirmClear,
                     const char*, const char*, uint8_t, const char*)
{
    auto& display = target();
    display.startWrite();
    display.fillScreen(kBackground);
    display.setTextSize(1);
    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE);
    const int right = display.width() - kScreenInset;
    const int clearLeft = right - kClearWidth;
    char title[24];
    snprintf(title, sizeof(title), "DC METER  #%s", meterDeviceId());
    display.drawString(confirmClear ? "Delete all records?" : title, kScreenInset, 10);
    display.fillRoundRect(clearLeft, kClearTop, kClearWidth, kClearHeight, 5,
                          confirmClear ? TFT_RED : kPanel);
    display.drawCenterString(confirmClear ? "CONFIRM" : "CLEAR", clearLeft + kClearWidth / 2, 9);
    drawReading(display, 34, "BUS VOLTAGE", voltage, "V", kVoltageColor);
    drawReading(display, 118, "CURRENT", current, currentUnit, kCurrentColor);
    display.setFont(&fonts::Font2);
    char count[24];
    snprintf(count, sizeof(count), "REC %lu", static_cast<unsigned long>(recordCount));
    display.setTextColor(logReady ? kCurrentColor : TFT_ORANGE);
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
        const String setup = String(meterSetupSsid()) + "  192.168.4.1";
        display.drawString(setup, kScreenInset, 220);
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
    pushFrame();
}


#else
void BoardUi::render(const char* voltage, const char* current, const char* currentUnit, const char* status,
                     uint16_t statusColor, size_t recordCount, bool logReady, bool confirmClear,
                     const char* averageCurrent, const char* averageVoltage, uint8_t storagePercent,
                     const char* rangeId)
{
#if defined(METER_BOARD_STICKS3)
    if (screenAsleep || shutdownPending) {
        return;
    }
#endif
    auto& display = target();
    display.startWrite();
    display.fillScreen(kBackground);
    display.setTextSize(1);
    display.setFont(&fonts::Font0);
    display.setTextColor(kMuted);
#if defined(METER_BOARD_STICKS3)
    char dateTime[24] = "--/-- --:--:--";
    constexpr char dateFormat[] = "%m/%d %H:%M:%S";
#else
    char dateTime[24] = "----/--/-- --:--:--";
    constexpr char dateFormat[] = "%Y/%m/%d %H:%M:%S";
#endif
    const time_t utc = time(nullptr);
    if (utc >= 1577836800) {
        constexpr int kChinaUtcOffset = 8 * 60 * 60;
        const time_t local = utc + kChinaUtcOffset;
        tm localTime{};
        if (gmtime_r(&local, &localTime)) {
            strftime(dateTime, sizeof(dateTime), dateFormat, &localTime);
        }
    }
#if defined(METER_BOARD_STICKS3)
    drawStatusBar(display, dateTime);
    constexpr int baseline = 57;
    const auto* currentFont = &fonts::FreeMonoBold24pt7b;
#else
    centerFit(display, dateTime, 5);
    constexpr int baseline = 45;
    const auto* currentFont = &fonts::FreeMonoBold18pt7b;
#endif
    constexpr int unitGap = 3;
    const int unitWidth = display.textWidth(currentUnit);
    display.setFont(currentFont);
    display.setTextColor(kCurrentColor);
    const int available = display.width() - 10 - unitGap - unitWidth;
    const int currentWidth = display.textWidth(current);
    if (currentWidth > available) {
        display.setTextSize(static_cast<float>(available) / currentWidth, 1);
    }
    const int numberWidth = display.textWidth(current);
    const int left = (display.width() - numberWidth - unitGap - unitWidth) / 2;
    display.setTextDatum(m5gfx::textdatum_t::baseline_left);
    display.drawString(current, left, baseline);
    display.setTextSize(1);
    display.setFont(&fonts::Font0);
    display.drawString(currentUnit, left + numberWidth + unitGap, baseline);
    display.setTextDatum(m5gfx::textdatum_t::top_left);

    display.setFont(&fonts::Font2);
    char line[24];
    snprintf(line, sizeof(line), "%s V", voltage);
    display.setTextColor(kVoltageColor);
#if defined(METER_BOARD_STICKS3)
    const int rangeLeft = display.width() - 41;
    display.drawCenterString(line, (rangeLeft + 4) / 2, kVoltageTop);
    display.fillRoundRect(rangeLeft, kVoltageTop, 36, 16, 3, 0x0368);
    display.setTextColor(TFT_WHITE);
    display.drawCenterString(rangeId, rangeLeft + 18, kVoltageTop);
    if (logReady && statusColor == TFT_ORANGE) {
        display.setFont(&fonts::Font0);
        display.setTextColor(TFT_ORANGE);
        centerFit(display, status, 91);
    }
    display.drawFastHLine(5, 86, display.width() - 10, 0x2945);
    display.setFont(&fonts::Font0);
    display.setTextColor(kMuted);
    centerFit(display, "AVG CURRENT", 103);
    centerFit(display, "AVG VOLTAGE", 138);
    display.setFont(&fonts::Font2);
    display.setTextColor(kCurrentColor);
    snprintf(line, sizeof(line), "%s %s", averageCurrent, currentUnit);
    centerFit(display, line, 114);
    display.setTextColor(kVoltageColor);
    snprintf(line, sizeof(line), "%s V", averageVoltage);
    centerFit(display, line, 149);
#else
    centerFit(display, line, kVoltageTop);
    (void)statusColor;
    (void)averageCurrent;
    (void)averageVoltage;
    (void)storagePercent;
    (void)rangeId;
#endif
    display.setTextColor(logReady ? kMuted : TFT_ORANGE);
    char device[8];
    snprintf(device, sizeof(device), "#%s", meterDeviceId());
    if (confirmClear) {
        display.setTextColor(TFT_ORANGE);
        display.setFont(&fonts::Font0);
        display.drawString("Press to clear", 5, kRecordTop + 4);
    } else if (logReady) {
        snprintf(line, sizeof(line), "%lu", static_cast<unsigned long>(recordCount));
        const int labelWidth = display.textWidth("REC") + 4;
        display.drawString("REC", 5, kRecordTop);
        display.setTextColor(kRecordColor);
        display.drawString(line, 5 + labelWidth, kRecordTop);
    } else {
        display.setFont(&fonts::Font0);
        display.drawString(status, 5, kRecordTop + 4);
    }
    display.setFont(&fonts::Font2);
    display.setTextColor(kMuted);
    display.drawRightString(device, display.width() - 5, kRecordTop);
#if defined(METER_BOARD_STICKS3)
    constexpr int progressTop = kRecordTop + 17;
    const int progressWidth = display.width() - 10;
    display.fillRect(5, progressTop, progressWidth, 2, 0x2945);
    if (storagePercent) {
        display.fillRect(5, progressTop, progressWidth * storagePercent / 100, 2, kRecordColor);
    }
#endif
    const uint32_t address = meterWebAddress();
    if (meterWifiSetupActive()) {
        drawWifiName(display, meterSetupSsid());
        centerFit(display, "192.168.4.1", kIpTop);
    } else if (address) {
        drawWifiName(display, meterWifiName());
        centerFit(display, IPAddress(address).toString().c_str(), kIpTop);
    } else {
        drawWifiName(display, "Wi-Fi connecting");
    }
    display.endWrite();
    pushFrame();
}
#endif
