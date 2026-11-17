#pragma once

namespace BoardConfig {
#if defined(METER_BOARD_STICKS3)
constexpr int sensorSda = 9;
constexpr int sensorScl = 10;
constexpr bool hasRtc = false;
constexpr char setupTitle[] = "StickS3 Wi-Fi Setup";
constexpr char liveStatus[] = "GROVE / LIVE";
constexpr char rangeStatus[] = "Range limit";
constexpr char sensorError[] = "Check INA226";
constexpr char connectingStatus[] = "Connecting...";
#elif defined(METER_BOARD_ATOMS3R)
constexpr int sensorSda = 2;
constexpr int sensorScl = 1;
constexpr bool hasRtc = false;
constexpr char setupTitle[] = "AtomS3R Wi-Fi Setup";
constexpr char liveStatus[] = "GROVE / LIVE";
constexpr char rangeStatus[] = "Range limit";
constexpr char sensorError[] = "Check INA226";
constexpr char connectingStatus[] = "Connecting...";
#elif defined(METER_BOARD_CORES3)
constexpr int sensorSda = 9;
constexpr int sensorScl = 8;
constexpr bool hasRtc = true;
constexpr char setupTitle[] = "CoreS3 Wi-Fi Setup";
constexpr char liveStatus[] = "PORT B  /  LIVE";
constexpr char rangeStatus[] = "PORT B  /  Range limit";
constexpr char sensorError[] = "PORT B  /  Check INA226 cable";
constexpr char connectingStatus[] = "PORT B  /  Connecting...";
#else
#error Select a supported meter board in platformio.ini
#endif
}
