#include "MeterOta.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <esp_ota_ops.h>
#include <atomic>
#include "FlashLog.h"
#include "MeterVersion.h"

namespace {
constexpr char kBoardPrefix[] = "ESP32DMM-OTA:" METER_OTA_BOARD ":";
constexpr char kFirmwareIdentity[] = "ESP32DMM-OTA:" METER_OTA_BOARD ":" METER_FIRMWARE_VERSION;
constexpr char kOtaNamespace[] = "dmm-ota";
constexpr char kPendingKey[] = "pending";
constexpr uint32_t kConfirmMs = 60000;
enum class State { Idle, WaitingForButton, Armed, Uploading, Restarting };
std::atomic<State> state{State::Idle};
std::atomic<uint32_t> armedAt{0};
char uploadToken[33]{};

State currentState()
{
    auto current = state.load();
    if ((current == State::Armed || current == State::WaitingForButton) &&
        millis() - armedAt.load() >= kConfirmMs) {
        state.compare_exchange_strong(current, State::Idle);
        return state.load();
    }
    return current;
}

bool markPending(uint32_t address)
{
    Preferences prefs;
    if (!prefs.begin(kOtaNamespace, false)) {
        return false;
    }
    return prefs.putUInt(kPendingKey, address) == sizeof(address);
}

struct Upload {
    esp_ota_handle_t handle = 0;
    const esp_partition_t* target = nullptr;
    size_t prefixBytes = 0;
    size_t written = 0;
    bool matchesBoard = false;
    bool running = false;
    bool ended = false;
    bool paused = false;
    const char* error = "Upload not authorized";

    void fail(FlashLog& log, const char* message)
    {
        if (running) {
            esp_ota_abort(handle);
            running = false;
        }
        if (paused) {
            log.resumeAfterFailedUpdate();
            paused = false;
        }
        state.store(State::Idle);
        error = message;
    }

    void scan(const uint8_t* bytes, size_t length)
    {
        for (size_t i = 0; i < length && !matchesBoard; ++i) {
            if (bytes[i] == kBoardPrefix[prefixBytes]) {
                ++prefixBytes;
                matchesBoard = prefixBytes == sizeof(kBoardPrefix) - 1;
            } else {
                prefixBytes = bytes[i] == kBoardPrefix[0] ? 1 : 0;
            }
        }
    }
};
Upload upload;
}

const char* meterFirmwareVersion()
{
    // Keep the entire identity string in the binary for OTA model checks.
    const char* volatile identity = kFirmwareIdentity;
    return identity + sizeof(kBoardPrefix) - 1;
}

bool meterOtaConfirmButton()
{
    auto waiting = currentState();
    if (waiting != State::WaitingForButton) {
        return false;
    }
    armedAt.store(millis());
    return state.compare_exchange_strong(waiting, State::Armed);
}

bool meterOtaBusy()
{
    const auto status = currentState();
    return status == State::Uploading || status == State::Restarting;
}

const char* meterOtaScreenStatus()
{
    switch (currentState()) {
        case State::WaitingForButton:
#if defined(METER_BOARD_STICKS3)
            return "BOOT to update";
#else
            return "Press to update";
#endif
        case State::Armed: return "OTA ready";
        case State::Uploading: return "OTA uploading";
        case State::Restarting: return "OTA restarting";
        default: return nullptr;
    }
}

void meterOtaFinishBoot(FlashLog& log)
{
    const auto* running = esp_ota_get_running_partition();
    if (!running) return;
    Preferences prefs;
    const bool opened = prefs.begin(kOtaNamespace, false);
    const bool migrated = opened && prefs.getUInt(kPendingKey, 0) == running->address;
    esp_ota_img_states_t imageState;
    const bool verifying = esp_ota_get_state_partition(running, &imageState) == ESP_OK &&
                           imageState == ESP_OTA_IMG_PENDING_VERIFY;
    if (!migrated && !verifying) return;

    if ((migrated && !log.clear()) || log.state == FlashLog::State::Error) {
        if (verifying) esp_ota_mark_app_invalid_rollback_and_reboot();
        return;
    }
    if (verifying && esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
        return;
    }
    if (migrated) prefs.remove(kPendingKey);
}

void meterOtaInstallRoutes(WebServer& server, FlashLog& log)
{
    server.on("/api/ota", HTTP_GET, [&server] {
        const auto status = currentState();
        const auto* target = esp_ota_get_next_update_partition(nullptr);
        const auto* running = esp_ota_get_running_partition();
        const bool available = target && running && target->address != running->address;
        char json[160];
        snprintf(json, sizeof(json), "{\"model\":\"%s\",\"version\":\"%s\",\"available\":%s,\"armed\":%s}",
                 METER_OTA_BOARD, meterFirmwareVersion(), available ? "true" : "false",
                 status == State::Armed ? "true" : "false");
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json", json);
    });
    server.on("/api/ota/arm", HTTP_POST, [&server] {
        if (server.header("X-Meter-Action") != "firmware-update") {
            server.send(403, "text/plain", "Confirmation required");
            return;
        }
        if (currentState() != State::Idle || !esp_ota_get_next_update_partition(nullptr)) {
            server.send(409, "text/plain", "OTA slot unavailable or confirmation pending");
            return;
        }
        uint8_t randomBytes[16];
        esp_fill_random(randomBytes, sizeof(randomBytes));
        for (size_t i = 0; i < sizeof(randomBytes); ++i) {
            snprintf(uploadToken + 2 * i, 3, "%02x", randomBytes[i]);
        }
        armedAt.store(millis());
        state.store(State::WaitingForButton);
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "text/plain", uploadToken);
    });
    server.on("/api/ota/upload", HTTP_POST, [&server, &log] {
        if (!upload.running || !upload.ended || !upload.matchesBoard) {
            const char* error = upload.running && !upload.matchesBoard
                ? "Firmware model does not match this meter" : upload.error;
            upload.fail(log, error);
            server.send(400, "text/plain", error);
            return;
        }
        upload.running = false;
        if (esp_ota_end(upload.handle) != ESP_OK ||
            !markPending(upload.target->address) ||
            esp_ota_set_boot_partition(upload.target) != ESP_OK) {
            upload.fail(log, "Firmware validation or activation failed");
            server.send(400, "text/plain", upload.error);
            return;
        }
        state.store(State::Restarting);
        server.send(200, "text/plain", "Firmware verified, restarting");
        delay(350);
        ESP.restart();
    }, [&server, &log] {
        auto& part = server.upload();
        switch (part.status) {
            case UPLOAD_FILE_START: {
                if (upload.running || upload.paused) {
                    upload.fail(log, "Only one firmware file is allowed");
                    return;
                }
                upload = Upload{};
                if (server.header("X-Meter-Action") != "firmware-update" ||
                    server.header("X-Meter-Ota-Token") != uploadToken ||
                    currentState() != State::Armed || part.name != "firmware") {
                    return;
                }
                upload.target = esp_ota_get_next_update_partition(nullptr);
                const auto* active = esp_ota_get_running_partition();
                const auto* boot = esp_ota_get_boot_partition();
                if (!upload.target || !active || !boot || upload.target->address == active->address ||
                    upload.target->address == boot->address || !log.pauseForUpdate()) {
                    upload.fail(log, "OTA slot or log is busy");
                    return;
                }
                upload.paused = true;
                state.store(State::Uploading);
                if (esp_ota_begin(upload.target, OTA_SIZE_UNKNOWN, &upload.handle) != ESP_OK) {
                    upload.fail(log, "Cannot open OTA slot");
                    return;
                }
                upload.running = true;
                upload.error = "Upload incomplete";
                break;
            }
            case UPLOAD_FILE_WRITE:
                if (upload.running) {
                    if (part.currentSize > upload.target->size - upload.written ||
                        esp_ota_write(upload.handle, part.buf, part.currentSize) != ESP_OK) {
                        upload.fail(log, "Image too large or write failed");
                        return;
                    }
                    upload.written += part.currentSize;
                    upload.scan(part.buf, part.currentSize);
                }
                break;
            case UPLOAD_FILE_END:
                upload.ended = upload.running;
                break;
            case UPLOAD_FILE_ABORTED:
                if (upload.running || upload.paused) upload.fail(log, "Upload interrupted");
                break;
        }
    });
}
