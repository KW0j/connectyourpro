#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "DsuServer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>
#include <ViGEm/Common.h>

namespace {
constexpr uint16_t kProtocolVersion = 1001;
constexpr uint32_t kMsgVersion = 0x100000;
constexpr uint32_t kMsgControllerInfo = 0x100001;
constexpr uint32_t kMsgControllerData = 0x100002;

void WriteU16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void WriteU32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

void WriteU64(std::vector<uint8_t>& out, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

void WriteFloat(std::vector<uint8_t>& out, float value)
{
    static_assert(sizeof(float) == 4);
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    WriteU32(out, bits);
}

uint16_t ReadU16(const uint8_t* data)
{
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t ReadU32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0])
        | (static_cast<uint32_t>(data[1]) << 8)
        | (static_cast<uint32_t>(data[2]) << 16)
        | (static_cast<uint32_t>(data[3]) << 24);
}

uint32_t Crc32(const uint8_t* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void WriteControllerHeader(std::vector<uint8_t>& out, uint8_t slot, bool connected)
{
    out.push_back(slot);
    out.push_back(connected ? 2 : 0);
    out.push_back(connected ? 2 : 0);
    out.push_back(connected ? 2 : 0);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(static_cast<uint8_t>(0x10 + slot));
    out.push_back(connected ? 3 : 0);
}

std::vector<uint8_t> MakePacket(uint32_t serverId, uint32_t messageType)
{
    std::vector<uint8_t> out;
    out.reserve(128);
    out.push_back('D');
    out.push_back('S');
    out.push_back('U');
    out.push_back('S');
    WriteU16(out, kProtocolVersion);
    WriteU16(out, 0);
    WriteU32(out, 0);
    WriteU32(out, serverId);
    WriteU32(out, messageType);
    return out;
}

void FinalizePacket(std::vector<uint8_t>& packet)
{
    const uint16_t payloadLength = static_cast<uint16_t>(packet.size() - 16);
    packet[6] = static_cast<uint8_t>(payloadLength);
    packet[7] = static_cast<uint8_t>(payloadLength >> 8);
    packet[8] = packet[9] = packet[10] = packet[11] = 0;
    const uint32_t crc = Crc32(packet.data(), packet.size());
    packet[8] = static_cast<uint8_t>(crc);
    packet[9] = static_cast<uint8_t>(crc >> 8);
    packet[10] = static_cast<uint8_t>(crc >> 16);
    packet[11] = static_cast<uint8_t>(crc >> 24);
}

uint64_t NowMicros()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

uint8_t DpadNibble(uint16_t buttons)
{
    return static_cast<uint8_t>(buttons & 0x0F);
}

bool DpadUp(uint8_t dpad) { return dpad == 0 || dpad == 1 || dpad == 7; }
bool DpadRight(uint8_t dpad) { return dpad == 1 || dpad == 2 || dpad == 3; }
bool DpadDown(uint8_t dpad) { return dpad == 3 || dpad == 4 || dpad == 5; }
bool DpadLeft(uint8_t dpad) { return dpad == 5 || dpad == 6 || dpad == 7; }

std::vector<uint8_t> BuildVersionPacket(uint32_t serverId)
{
    auto out = MakePacket(serverId, kMsgVersion);
    WriteU16(out, kProtocolVersion);
    FinalizePacket(out);
    return out;
}

std::vector<uint8_t> BuildInfoPacket(uint32_t serverId, uint8_t slot, bool connected)
{
    auto out = MakePacket(serverId, kMsgControllerInfo);
    WriteControllerHeader(out, slot, connected);
    out.push_back(0);
    FinalizePacket(out);
    return out;
}

std::vector<uint8_t> BuildDataPacket(uint32_t serverId, uint8_t slot, DsuServer::ControllerState state)
{
    const auto& report = state.report.Report;
    auto out = MakePacket(serverId, kMsgControllerData);
    WriteControllerHeader(out, slot, state.connected);
    out.push_back(state.connected ? 1 : 0);
    WriteU32(out, state.packetCounter);

    const uint8_t dpad = DpadNibble(report.wButtons);
    uint8_t buttons1 = 0;
    if (DpadLeft(dpad)) buttons1 |= 0x80;
    if (DpadDown(dpad)) buttons1 |= 0x40;
    if (DpadRight(dpad)) buttons1 |= 0x20;
    if (DpadUp(dpad)) buttons1 |= 0x10;
    if (report.wButtons & DS4_BUTTON_OPTIONS) buttons1 |= 0x08;
    if (report.wButtons & DS4_BUTTON_THUMB_RIGHT) buttons1 |= 0x04;
    if (report.wButtons & DS4_BUTTON_THUMB_LEFT) buttons1 |= 0x02;
    if (report.wButtons & DS4_BUTTON_SHARE) buttons1 |= 0x01;
    out.push_back(buttons1);

    uint8_t buttons2 = 0;
    if (report.wButtons & DS4_BUTTON_SQUARE) buttons2 |= 0x80;
    if (report.wButtons & DS4_BUTTON_CROSS) buttons2 |= 0x40;
    if (report.wButtons & DS4_BUTTON_CIRCLE) buttons2 |= 0x20;
    if (report.wButtons & DS4_BUTTON_TRIANGLE) buttons2 |= 0x10;
    if (report.wButtons & DS4_BUTTON_SHOULDER_RIGHT) buttons2 |= 0x08;
    if (report.wButtons & DS4_BUTTON_SHOULDER_LEFT) buttons2 |= 0x04;
    if (report.wButtons & DS4_BUTTON_TRIGGER_RIGHT) buttons2 |= 0x02;
    if (report.wButtons & DS4_BUTTON_TRIGGER_LEFT) buttons2 |= 0x01;
    out.push_back(buttons2);

    out.push_back((report.bSpecial & DS4_SPECIAL_BUTTON_PS) ? 1 : 0);
    out.push_back((report.bSpecial & DS4_SPECIAL_BUTTON_TOUCHPAD) ? 1 : 0);
    out.push_back(report.bThumbLX);
    out.push_back(static_cast<uint8_t>(255 - report.bThumbLY));
    out.push_back(report.bThumbRX);
    out.push_back(static_cast<uint8_t>(255 - report.bThumbRY));

    out.push_back(DpadLeft(dpad) ? 255 : 0);
    out.push_back(DpadDown(dpad) ? 255 : 0);
    out.push_back(DpadRight(dpad) ? 255 : 0);
    out.push_back(DpadUp(dpad) ? 255 : 0);
    out.push_back((report.wButtons & DS4_BUTTON_SQUARE) ? 255 : 0);
    out.push_back((report.wButtons & DS4_BUTTON_CROSS) ? 255 : 0);
    out.push_back((report.wButtons & DS4_BUTTON_CIRCLE) ? 255 : 0);
    out.push_back((report.wButtons & DS4_BUTTON_TRIANGLE) ? 255 : 0);
    out.push_back((report.wButtons & DS4_BUTTON_SHOULDER_RIGHT) ? 255 : 0);
    out.push_back((report.wButtons & DS4_BUTTON_SHOULDER_LEFT) ? 255 : 0);
    out.push_back(report.bTriggerR);
    out.push_back(report.bTriggerL);

    out.insert(out.end(), 12, 0);
    WriteU64(out, NowMicros());
    WriteFloat(out, report.wAccelX / 4096.0f);
    WriteFloat(out, report.wAccelY / 4096.0f);
    WriteFloat(out, report.wAccelZ / 4096.0f);
    WriteFloat(out, report.wGyroX * 360.0f / 48000.0f);
    WriteFloat(out, report.wGyroY * 360.0f / 48000.0f);
    WriteFloat(out, report.wGyroZ * 360.0f / 48000.0f);

    FinalizePacket(out);
    return out;
}
}

DsuServer::DsuServer()
{
    std::random_device rd;
    serverId_ = (static_cast<uint32_t>(rd()) << 16) ^ static_cast<uint32_t>(rd());
}

DsuServer::~DsuServer()
{
    Stop();
}

bool DsuServer::Start(uint16_t port)
{
    if (running_.load()) {
        return true;
    }

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }

    SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(sock);
        WSACleanup();
        return false;
    }

    socket_ = static_cast<uintptr_t>(sock);
    running_.store(true);
    serverThread_ = std::thread([this]() {
        std::array<uint8_t, 1024> buffer{};
        while (running_.load()) {
            sockaddr_in client{};
            int clientLen = sizeof(client);
            const int received = recvfrom(static_cast<SOCKET>(socket_), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&client), &clientLen);
            if (received < 20) {
                continue;
            }

            if (std::memcmp(buffer.data(), "DSUC", 4) != 0 || ReadU16(buffer.data() + 4) != kProtocolVersion) {
                continue;
            }

            const uint32_t messageType = ReadU32(buffer.data() + 16);
            if (messageType == kMsgVersion) {
                auto response = BuildVersionPacket(serverId_);
                sendto(static_cast<SOCKET>(socket_), reinterpret_cast<const char*>(response.data()), static_cast<int>(response.size()), 0, reinterpret_cast<sockaddr*>(&client), clientLen);
            }
            else if (messageType == kMsgControllerInfo) {
                const uint32_t requested = received >= 24 ? std::min<uint32_t>(ReadU32(buffer.data() + 20), 4) : 4;
                for (uint32_t i = 0; i < requested; ++i) {
                    if (received < static_cast<int>(25 + i)) {
                        break;
                    }
                    const uint8_t slot = received >= 25 ? buffer[24 + i] : static_cast<uint8_t>(i);
                    if (slot >= controllers_.size()) {
                        continue;
                    }
                    bool connected = false;
                    {
                        std::lock_guard<std::mutex> lock(controllersMutex_);
                        connected = controllers_[slot].connected;
                    }
                    auto response = BuildInfoPacket(serverId_, slot, connected);
                    sendto(static_cast<SOCKET>(socket_), reinterpret_cast<const char*>(response.data()), static_cast<int>(response.size()), 0, reinterpret_cast<sockaddr*>(&client), clientLen);
                }
            }
            else if (messageType == kMsgControllerData) {
                uint8_t slot = 0;
                const uint8_t flags = buffer[20];
                if (received >= 22 && (buffer[20] & 0x01)) {
                    slot = buffer[21];
                }
                if (slot >= controllers_.size()) {
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(clientMutex_);
                    std::memset(client_.address.data(), 0, client_.address.size());
                    std::memcpy(client_.address.data(), &client, std::min<int>(clientLen, static_cast<int>(client_.address.size())));
                    client_.addressLength = std::min<int>(clientLen, static_cast<int>(client_.address.size()));
                    client_.slots.fill(flags == 0 || (flags & 0x02));
                    if (flags & 0x01) {
                        client_.slots[slot] = true;
                    }
                    client_.subscribed = true;
                }

                ControllerState state{};
                {
                    std::lock_guard<std::mutex> lock(controllersMutex_);
                    state = controllers_[slot];
                }
                if (!state.connected) {
                    continue;
                }
                auto response = BuildDataPacket(serverId_, slot, state);
                sendto(static_cast<SOCKET>(socket_), reinterpret_cast<const char*>(response.data()), static_cast<int>(response.size()), 0, reinterpret_cast<sockaddr*>(&client), clientLen);
            }
        }
    });

    return true;
}

void DsuServer::Stop()
{
    if (!running_.exchange(false)) {
        return;
    }

    if (socket_ != ~uintptr_t{ 0 }) {
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = ~uintptr_t{ 0 };
    }

    if (serverThread_.joinable()) {
        serverThread_.join();
    }

    WSACleanup();
}

bool DsuServer::IsRunning() const
{
    return running_.load();
}

void DsuServer::SetControllerConnected(uint8_t slot, bool connected)
{
    if (slot >= controllers_.size()) {
        return;
    }

    std::lock_guard<std::mutex> lock(controllersMutex_);
    controllers_[slot].connected = connected;
    if (connected) {
        std::cout << "DSU slot " << static_cast<int>(slot) << " marked connected." << std::endl;
    }
}

void DsuServer::UpdateController(uint8_t slot, const DS4_REPORT_EX& report, bool connected)
{
    if (slot >= controllers_.size()) {
        return;
    }

    ControllerState snapshot{};
    {
        std::lock_guard<std::mutex> lock(controllersMutex_);
        auto& state = controllers_[slot];
        state.report = report;
        state.connected = connected;
        ++state.packetCounter;
        snapshot = state;
    }

    if (!running_.load() || !snapshot.connected || socket_ == ~uintptr_t{ 0 }) {
        return;
    }

    ClientEndpoint endpoint{};
    {
        std::lock_guard<std::mutex> clientLock(clientMutex_);
        endpoint = client_;
    }

    if (!endpoint.subscribed || !endpoint.slots[slot] || endpoint.addressLength <= 0) {
        return;
    }

    auto packet = BuildDataPacket(serverId_, slot, snapshot);
    sendto(static_cast<SOCKET>(socket_), reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0, reinterpret_cast<const sockaddr*>(endpoint.address.data()), endpoint.addressLength);
}
