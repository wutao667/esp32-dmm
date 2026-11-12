#pragma once

#include <Arduino.h>

class FlashLog;
uint32_t meterWebAddress();
String meterWifiName();
bool meterWifiSetupActive();
void meterRequestWifiReset();
bool meterLiveReading(float& microamps, float& volts);
bool meterClearHistory();
void meterWebTask(void* log);
