#pragma once

#include <stdint.h>

struct CurrentRange {
    const char* id;
    float shuntOhms;
    float maxMicroamps;
    bool milliamps;

    constexpr float microamps(int16_t counts) const { return counts * (2.5f / shuntOhms); }
    constexpr float displayValue(float microamps) const { return milliamps ? microamps / 1000 : microamps; }
    constexpr const char* unit() const { return milliamps ? "mA" : "uA"; }
};

constexpr CurrentRange kCurrentRanges[] = {
    {"8mA", 10.0f, 8000.0f, false},
    {"1A", 0.080f, 1000000.0f, true},
    {"10A", 0.005f, 10000000.0f, true},
};
constexpr uint8_t kCurrentRangeCount = sizeof(kCurrentRanges) / sizeof(kCurrentRanges[0]);

uint8_t currentRangeIndex();
void loadCurrentRange();
bool saveCurrentRange(uint8_t index);
