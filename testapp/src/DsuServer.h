#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <ViGEm/Client.h>

class DsuServer {
public:
    DsuServer();
    ~DsuServer();

    DsuServer(const DsuServer&) = delete;
    DsuServer& operator=(const DsuServer&) = delete;

    bool Start(uint16_t port = 26760);
    void Stop();
    bool IsRunning() const;

    void SetControllerConnected(uint8_t slot, bool connected = true);
    void UpdateController(uint8_t slot, const DS4_REPORT_EX& report, bool connected = true);

    struct ControllerState {
        DS4_REPORT_EX report{};
        bool connected = false;
        uint32_t packetCounter = 0;
    };

private:
    struct ClientEndpoint {
        std::array<uint8_t, 32> address{};
        int addressLength = 0;
        std::array<bool, 4> slots{};
        bool subscribed = false;
    };

    std::array<ControllerState, 4> controllers_{};
    ClientEndpoint client_{};
    mutable std::mutex controllersMutex_;
    mutable std::mutex clientMutex_;
    std::atomic<bool> running_{ false };
    std::thread serverThread_;
    uint32_t serverId_ = 0;
    uintptr_t socket_ = ~uintptr_t{ 0 };
};
