#pragma once

#include <LittleFS.h>
#include <esp_partition.h>
#include <unistd.h>
#include <mutex>

struct LogRecord {
    uint32_t timestamp;
    float volts;
    float microamps;
};
static_assert(sizeof(LogRecord) == 12, "Unexpected log record size");

struct LogTotals {
    size_t count = 0;
    double volts = 0;
    double microamps = 0;
};

class FlashLog {
public:
    enum class State { Ready, Full, Error };
    State state = State::Error;
    size_t count = 0;

    void begin()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!LittleFS.begin(false, "/meterfs", 4, "spiffs") &&
            (!isErased() || !LittleFS.begin(true, "/meterfs", 4, "spiffs"))) {
            return;
        }
        mounted = true;
        const size_t total = LittleFS.totalBytes();
        const size_t used = LittleFS.usedBytes();
        auto existing = LittleFS.open("/meter-v1.bin", "r");
        const size_t existingBytes = existing ? existing.size() : 0;
        existing.close();
        const size_t available = total - std::min(total, used);
        const size_t reserve = total / 10;
        limit = (existingBytes + (available > reserve ? available - reserve : 0)) /
                sizeof(LogRecord);
        open();
    }

    void append(const LogRecord& record)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (state != State::Ready) {
            return;
        }
        if (count >= limit) {
            state = State::Full;
            return;
        }
        if (fwrite(&record, sizeof(record), 1, file) != 1 ||
            fflush(file) != 0 || fsync(fileno(file)) != 0) {
            state = State::Error;
            return;
        }
        ++count;
        voltageSum += record.volts;
        currentSum += record.microamps;
    }

    bool clear()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!mounted) {
            return false;
        }
        if (file) {
            fclose(file);
            file = nullptr;
        }
        if (LittleFS.exists("/meter-v1.bin") && !LittleFS.remove("/meter-v1.bin")) {
            state = State::Error;
            return false;
        }
        count = 0;
        voltageSum = currentSum = 0;
        open();
        return totalsValid;
    }

    bool readPage(size_t& start, LogTotals& totals, LogRecord* output, size_t& length)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!mounted || !totalsValid) {
            return false;
        }
        totals.count = count;
        totals.volts = voltageSum;
        totals.microamps = currentSum;
        const size_t total = count;
        const size_t tail = total > length ? total - length : 0;
        start = std::min(start, tail);
        length = std::min(length, total - start);
        if (!length) {
            return true;
        }
        FILE* reader = fopen("/meterfs/meter-v1.bin", "rb");
        if (!reader) {
            return false;
        }
        const bool ok = fseek(reader, start * sizeof(LogRecord), SEEK_SET) == 0 &&
                        fread(output, sizeof(LogRecord), length, reader) == length;
        fclose(reader);
        return ok;
    }

private:
    std::mutex mutex;
    FILE* file = nullptr;
    size_t limit = 0;
    bool mounted = false;
    bool totalsValid = false;
    double voltageSum = 0;
    double currentSum = 0;

    void open()
    {
        constexpr char path[] = "/meterfs/meter-v1.bin";
        state = State::Error;
        totalsValid = false;
        file = fopen(path, LittleFS.exists("/meter-v1.bin") ? "r+b" : "w+b");
        if (!file || fseek(file, 0, SEEK_END) != 0) {
            return;
        }
        const long length = ftell(file);
        if (length < 0) {
            return;
        }
        count = length / sizeof(LogRecord);
        if (fseek(file, 0, SEEK_SET) != 0) {
            return;
        }
        voltageSum = currentSum = 0;
        LogRecord records[128];
        for (size_t remaining = count; remaining;) {
            const size_t batch = std::min(remaining, size_t{128});
            if (fread(records, sizeof(LogRecord), batch, file) != batch) {
                return;
            }
            for (size_t i = 0; i < batch; ++i) {
                voltageSum += records[i].volts;
                currentSum += records[i].microamps;
            }
            remaining -= batch;
        }
        if (fseek(file, count * sizeof(LogRecord), SEEK_SET) == 0) {
            totalsValid = true;
            state = count < limit ? State::Ready : State::Full;
        }
    }

    static bool isErased()
    {
        const auto* partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
        if (!partition) {
            return false;
        }
        uint8_t block[512];
        for (size_t offset = 0; offset < partition->size; offset += sizeof(block)) {
            if (esp_partition_read(partition, offset, block, sizeof(block)) != ESP_OK) {
                return false;
            }
            for (uint8_t byte : block) {
                if (byte != 0xff) {
                    return false;
                }
            }
        }
        return true;
    }
};
