#pragma once

#include <stddef.h>
#include <stdint.h>

namespace BoardUi {
enum class Input { None, Clear, Cancel, ResetWifi, NextRange, ConfirmUpdate };

void begin();
Input poll();
void playClearFeedback(bool cleared);
void render(const char* voltage, const char* current, const char* currentUnit, const char* status,
            uint16_t statusColor, size_t recordCount, bool logReady, bool confirmClear,
            const char* averageCurrent = "--", const char* averageVoltage = "--",
            uint8_t storagePercent = 0, const char* rangeId = "");
}
