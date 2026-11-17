#include "CurrentRange.h"

#include <Preferences.h>
#include <atomic>
#include <mutex>

namespace {
constexpr uint8_t kDefaultRangeIndex = 1;  // 1 A, 80 mΩ
std::atomic<uint8_t> selectedRange{kDefaultRangeIndex};
std::mutex rangeSaveMutex;
}

uint8_t currentRangeIndex()
{
    return selectedRange.load();
}

void loadCurrentRange()
{
    Preferences prefs;
    if (prefs.begin("dmm-meter", true)) {
        const uint8_t saved = prefs.getUChar("range", kDefaultRangeIndex);
        if (saved < kCurrentRangeCount) {
            selectedRange.store(saved);
        }
    }
}

bool saveCurrentRange(uint8_t index)
{
    if (index >= kCurrentRangeCount) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(rangeSaveMutex);
    if (index == currentRangeIndex()) {
        return true;
    }
    Preferences prefs;
    if (!prefs.begin("dmm-meter", false) || prefs.putUChar("range", index) != sizeof(index)) {
        return false;
    }
    selectedRange.store(index);
    return true;
}
