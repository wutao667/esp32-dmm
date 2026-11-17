#pragma once

#if defined(METER_BOARD_STICKS3)
#define METER_OTA_BOARD "sticks3"
#elif defined(METER_BOARD_ATOMS3R)
#define METER_OTA_BOARD "atoms3r"
#else
#define METER_OTA_BOARD "cores3"
#endif

#define METER_FIRMWARE_VERSION "0.3.0"

const char* meterFirmwareVersion();
