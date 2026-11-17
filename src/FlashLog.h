#pragma once

#include <LittleFS.h>
#include <esp_partition.h>
#include <esp_littlefs.h>
#include <esp_ota_ops.h>
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
    uint8_t usagePercent = 0;
};

class FlashLog {
public:
    enum class State { Ready, Full, Error };
    State state = State::Error;
    size_t count = 0;
    const char* errorDetail() const { return error; }

    void begin()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!LittleFS.begin(false, "/meterfs", 4, "spiffs")) {
            if (!isErased()) {
                return;
            }
            error = "LittleFS initialization failed";
            if (!LittleFS.begin(true, "/meterfs", 4, "spiffs")) {
                return;
            }
        }
        mounted = true;
        const size_t total = LittleFS.totalBytes();
        const size_t used = LittleFS.usedBytes();
        auto existing = LittleFS.open("/meter-v1.bin", "r");
        const size_t existingBytes = existing ? existing.size() : 0;
        existing.close();
        const size_t available = total - std::min(total, used);
        // Keep room for copy-on-write updates, even on the 64 KiB partition.
        const size_t reserve = std::max(total / 10, size_t{3 * 4096});
        limit = (existingBytes + (available > reserve ? available - reserve : 0)) /
                sizeof(LogRecord);
        open();
        if (totalsValid) {
            mountSpill(false);
            refreshState();
        }
    }

    void append(const LogRecord& record)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (paused || state != State::Ready) {
            return;
        }
        FILE* writer = file;
        if (spillCount || count >= limit) {
            if (!spill && !mountSpill(true)) {
                state = State::Full;
                return;
            }
            if (spillCount >= spillLimit) {
                state = State::Full;
                return;
            }
            writer = spill;
        }
        if (fwrite(&record, sizeof(record), 1, writer) != 1 ||
            fflush(writer) != 0 || fsync(fileno(writer)) != 0) {
            error = "Record write or sync failed";
            state = State::Error;
            return;
        }
        ++count;
        voltageSum += record.volts;
        currentSum += record.microamps;
        if (writer == spill) {
            ++spillCount;
            spillVoltageSum += record.volts;
            spillCurrentSum += record.microamps;
        }
    }

    bool clear()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!mounted || paused) {
            return false;
        }
        if (spill) {
            const auto* partition = spillPartition();
            if (!partition || !unmountSpill() ||
                esp_partition_erase_range(partition, 0, 2 * 4096) != ESP_OK) {
                state = State::Error;
                return false;
            }
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
        refreshState();
        return totalsValid;
    }

    LogTotals snapshotTotals()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        LogTotals totals;
        copyTotals(totals);
        return totals;
    }

    bool readPage(size_t& start, LogTotals& totals, LogRecord* output, size_t& length)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!copyTotals(totals)) {
            return false;
        }
        const size_t total = count;
        const size_t tail = total > length ? total - length : 0;
        start = std::min(start, tail);
        length = std::min(length, total - start);
        if (!length) {
            return true;
        }
        const size_t fixed = total - spillCount;
        const size_t fixedLength = start < fixed ? std::min(length, fixed - start) : 0;
        if (fixedLength && !readRecords("/meterfs/meter-v1.bin", start, output, fixedLength)) {
            return false;
        }
        const size_t overflow = length - fixedLength;
        return !overflow || readRecords(kSpillFile, start + fixedLength - fixed,
                                        output + fixedLength, overflow);
    }

    bool pauseForUpdate()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (paused || !unmountSpill()) {
            return false;
        }
        const auto* partition = esp_ota_get_next_update_partition(nullptr);
        if (!partition || esp_littlefs_partition_mounted(partition)) {
            return false;
        }
        paused = true;
        return true;
    }

    void resumeAfterFailedUpdate()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        paused = false;
        mountSpill(false);
        refreshState();
    }

private:
    static constexpr const char* kSpillFile = "/spillfs/meter-v1.bin";
    std::mutex mutex;
    FILE* file = nullptr;
    FILE* spill = nullptr;
    size_t limit = 0;
    size_t spillLimit = 0;
    size_t spillCount = 0;
    bool mounted = false;
    bool totalsValid = false;
    const char* error = "LittleFS mount failed";
    bool paused = false;
    double voltageSum = 0;
    double currentSum = 0;
    double spillVoltageSum = 0;
    double spillCurrentSum = 0;

    bool copyTotals(LogTotals& totals) const
    {
        if (!mounted || !totalsValid) {
            return false;
        }
        totals.count = count;
        totals.volts = voltageSum;
        totals.microamps = currentSum;
        totals.usagePercent = usagePercent();
        return true;
    }

    uint8_t usagePercent() const
    {
        if (state == State::Full) {
            return 100;
        }
        size_t capacity = limit;
        if (spill) {
            capacity += spillLimit;
        } else if (const auto* partition = spillPartition()) {
            // Before the overflow filesystem exists, estimate its usable space.
            const size_t bytes = partition->size;
            capacity += (bytes - std::max(bytes / 10, size_t{3 * 4096})) / sizeof(LogRecord);
        }
        return capacity ? std::min(size_t{100}, count * 100 / capacity) : 0;
    }

    static bool readRecords(const char* path, size_t start, LogRecord* output, size_t length)
    {
        FILE* reader = fopen(path, "rb");
        if (!reader) {
            return false;
        }
        const bool ok = fseek(reader, start * sizeof(LogRecord), SEEK_SET) == 0 &&
                        fread(output, sizeof(LogRecord), length, reader) == length;
        fclose(reader);
        return ok;
    }

    static const esp_partition_t* spillPartition()
    {
#if defined(METER_BOARD_ATOMS3R) || defined(METER_BOARD_STICKS3)
        const auto* partition = esp_ota_get_next_update_partition(nullptr);
        const auto* running = esp_ota_get_running_partition();
        const auto* boot = esp_ota_get_boot_partition();
        esp_ota_img_states_t imageState;
        if (!partition || !running || !boot || partition->encrypted ||
            partition->address == running->address || partition->address == boot->address ||
            (esp_ota_get_state_partition(running, &imageState) == ESP_OK &&
             imageState == ESP_OTA_IMG_PENDING_VERIFY)) {
            return nullptr;
        }
        return partition;
#else
        return nullptr;
#endif
    }

    bool mountSpill(bool allowFormat)
    {
        const auto* partition = spillPartition();
        if (!partition || spill || !totalsValid) {
            return spill != nullptr;
        }
        // Preserve a bootable rollback image until the fixed log actually fills.
        esp_vfs_littlefs_conf_t config{};
        config.base_path = "/spillfs";
        config.partition = partition;
        config.format_if_mount_failed = allowFormat;
        if (esp_vfs_littlefs_register(&config) != ESP_OK) {
            return false;
        }
        FILE* writer = fopen(kSpillFile, "r+b");
        if (!writer && allowFormat) {
            writer = fopen(kSpillFile, "w+b");
        }
        size_t total = 0;
        size_t used = 0;
        long bytes = -1;
        if (writer && fseek(writer, 0, SEEK_END) == 0) {
            bytes = ftell(writer);
        }
        if (bytes < 0 || esp_littlefs_partition_info(partition, &total, &used) != ESP_OK ||
            fseek(writer, 0, SEEK_SET) != 0) {
            if (writer) fclose(writer);
            esp_vfs_littlefs_unregister_partition(partition);
            return false;
        }
        const size_t records = static_cast<size_t>(bytes) / sizeof(LogRecord);
        double volts = 0;
        double microamps = 0;
        for (size_t i = 0; i < records; ++i) {
            LogRecord record;
            if (fread(&record, sizeof(record), 1, writer) != 1) {
                fclose(writer);
                esp_vfs_littlefs_unregister_partition(partition);
                return false;
            }
            volts += record.volts;
            microamps += record.microamps;
        }
        if (fseek(writer, records * sizeof(LogRecord), SEEK_SET) != 0) {
            fclose(writer);
            esp_vfs_littlefs_unregister_partition(partition);
            return false;
        }
        const size_t available = total - std::min(total, used);
        const size_t reserve = std::max(total / 10, size_t{3 * 4096});
        spillLimit = records + (available > reserve ? (available - reserve) / sizeof(LogRecord) : 0);
        spill = writer;
        spillCount = records;
        spillVoltageSum = volts;
        spillCurrentSum = microamps;
        count += records;
        voltageSum += volts;
        currentSum += microamps;
        return true;
    }

    bool unmountSpill()
    {
        if (!spill) {
            return true;
        }
        const auto* partition = spillPartition();
        if (!partition) {
            return false;
        }
        fclose(spill);
        spill = nullptr;
        count -= spillCount;
        voltageSum -= spillVoltageSum;
        currentSum -= spillCurrentSum;
        spillCount = 0;
        spillVoltageSum = spillCurrentSum = 0;
        return esp_vfs_littlefs_unregister_partition(partition) == ESP_OK;
    }

    void refreshState()
    {
        if (totalsValid) {
            state = count < limit || (spill && spillCount < spillLimit) ||
                (!spill && spillPartition()) ? State::Ready : State::Full;
        }
    }

    void open()
    {
        constexpr char path[] = "/meterfs/meter-v1.bin";
        state = State::Error;
        error = "Record file open or scan failed";
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

    bool isErased()
    {
        const auto* partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
        if (!partition) {
            error = "Log partition missing";
            return false;
        }
        uint8_t block[512];
        for (size_t offset = 0; offset < partition->size; offset += sizeof(block)) {
            if (esp_partition_read(partition, offset, block, sizeof(block)) != ESP_OK) {
                error = "Log partition read failed";
                return false;
            }
            for (uint8_t byte : block) {
                if (byte != 0xff) {
                    error = "LittleFS mount failed on non-empty partition";
                    return false;
                }
            }
        }
        return true;
    }
};
