#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr wchar_t kWindowClass[] = L"DsuPointerTesterWindow";
constexpr uint16_t kDsuPort = 26760;
constexpr uint16_t kProtocolVersion = 1001;
constexpr uint32_t kMsgVersion = 0x100000;
constexpr uint32_t kMsgControllerInfo = 0x100001;
constexpr uint32_t kMsgControllerData = 0x100002;

enum class ConnectionState {
    Disconnected,
    WaitingForSlot,
    ReceivingMotion
};

struct MotionSample {
    float accelX = 0.0f;
    float accelY = 0.0f;
    float accelZ = 0.0f;
    float gyroPitch = 0.0f;
    float gyroYaw = 0.0f;
    float gyroRoll = 0.0f;
    uint32_t packetCounter = 0;
    uint64_t timestampMicros = 0;
};

struct AppState {
    std::mutex mutex;
    MotionSample sample;
    ConnectionState connection = ConnectionState::Disconnected;
    uint64_t lastPacketMs = 0;
    float pointerX = 0.5f;
    float pointerY = 0.5f;
    float sensitivity = 0.004f;
    bool invertPitch = false;
    bool invertYaw = false;
    bool invertRoll = false;
    bool swapYawRoll = false;
    bool paused = false;
    bool running = true;
};

AppState g_app;
std::thread g_dsuThread;
HWND g_hwnd = nullptr;

uint64_t NowMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

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

uint64_t ReadU64(const uint8_t* data)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

float ReadFloat(const uint8_t* data)
{
    float value = 0.0f;
    std::memcpy(&value, data, sizeof(value));
    return value;
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

std::vector<uint8_t> MakePacket(uint32_t messageType)
{
    std::vector<uint8_t> out;
    out.reserve(64);
    out.push_back('D');
    out.push_back('S');
    out.push_back('U');
    out.push_back('C');
    WriteU16(out, kProtocolVersion);
    WriteU16(out, 0);
    WriteU32(out, 0);
    WriteU32(out, 0x50545231u);
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

std::vector<uint8_t> MakeVersionRequest()
{
    auto packet = MakePacket(kMsgVersion);
    FinalizePacket(packet);
    return packet;
}

std::vector<uint8_t> MakeInfoRequest(uint8_t slot)
{
    auto packet = MakePacket(kMsgControllerInfo);
    WriteU32(packet, 1);
    packet.push_back(slot);
    FinalizePacket(packet);
    return packet;
}

std::vector<uint8_t> MakeDataRequest(uint8_t slot)
{
    auto packet = MakePacket(kMsgControllerData);
    packet.push_back(0x01);
    packet.push_back(slot);
    FinalizePacket(packet);
    return packet;
}

void SetConnection(ConnectionState state)
{
    std::lock_guard<std::mutex> lock(g_app.mutex);
    g_app.connection = state;
}

bool ParseDataPacket(const uint8_t* data, int size, MotionSample& sample)
{
    if (size < 100 || std::memcmp(data, "DSUS", 4) != 0 || ReadU16(data + 4) != kProtocolVersion) {
        return false;
    }

    if (ReadU32(data + 16) != kMsgControllerData) {
        return false;
    }

    const uint8_t connectionState = data[21];
    if (connectionState == 0) {
        return false;
    }

    sample.packetCounter = ReadU32(data + 32);
    sample.timestampMicros = ReadU64(data + 68);
    sample.accelX = ReadFloat(data + 76);
    sample.accelY = ReadFloat(data + 80);
    sample.accelZ = ReadFloat(data + 84);
    sample.gyroPitch = ReadFloat(data + 88);
    sample.gyroYaw = ReadFloat(data + 92);
    sample.gyroRoll = ReadFloat(data + 96);
    return true;
}

void DsuThread()
{
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        SetConnection(ConnectionState::Disconnected);
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        SetConnection(ConnectionState::Disconnected);
        return;
    }

    DWORD timeoutMs = 100;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(kDsuPort);
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

    const auto versionRequest = MakeVersionRequest();
    const auto infoRequest = MakeInfoRequest(0);
    const auto dataRequest = MakeDataRequest(0);
    std::array<uint8_t, 512> buffer{};
    uint64_t lastRequestMs = 0;

    while (true) {
        {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            if (!g_app.running) {
                break;
            }
            if (g_app.lastPacketMs != 0 && NowMs() - g_app.lastPacketMs > 1500) {
                g_app.connection = ConnectionState::WaitingForSlot;
            }
        }

        const uint64_t now = NowMs();
        if (now - lastRequestMs >= 50) {
            sendto(sock, reinterpret_cast<const char*>(versionRequest.data()), static_cast<int>(versionRequest.size()), 0, reinterpret_cast<sockaddr*>(&server), sizeof(server));
            sendto(sock, reinterpret_cast<const char*>(infoRequest.data()), static_cast<int>(infoRequest.size()), 0, reinterpret_cast<sockaddr*>(&server), sizeof(server));
            sendto(sock, reinterpret_cast<const char*>(dataRequest.data()), static_cast<int>(dataRequest.size()), 0, reinterpret_cast<sockaddr*>(&server), sizeof(server));
            lastRequestMs = now;
            SetConnection(ConnectionState::WaitingForSlot);
        }

        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int received = recvfrom(sock, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (received > 0) {
            MotionSample sample{};
            if (ParseDataPacket(buffer.data(), received, sample)) {
                std::lock_guard<std::mutex> lock(g_app.mutex);
                g_app.sample = sample;
                g_app.lastPacketMs = NowMs();
                g_app.connection = ConnectionState::ReceivingMotion;
            }
        }
    }

    closesocket(sock);
    WSACleanup();
}

std::wstring FormatFloat(float value, int decimals = 2)
{
    wchar_t text[64]{};
    swprintf_s(text, L"%.*f", decimals, value);
    return text;
}

void DrawTextLine(HDC hdc, int x, int y, const std::wstring& text, COLORREF color = RGB(230, 230, 230))
{
    SetTextColor(hdc, color);
    TextOutW(hdc, x, y, text.c_str(), static_cast<int>(text.size()));
}

void DrawBar(HDC hdc, int x, int y, int width, int height, float value, float range, COLORREF color)
{
    RECT frame{ x, y, x + width, y + height };
    HBRUSH border = CreateSolidBrush(RGB(70, 70, 70));
    FrameRect(hdc, &frame, border);
    DeleteObject(border);

    const int center = x + width / 2;
    const float normalized = std::clamp(value / range, -1.0f, 1.0f);
    RECT fill{};
    if (normalized >= 0.0f) {
        fill = { center, y + 2, center + static_cast<int>((width / 2 - 2) * normalized), y + height - 2 };
    }
    else {
        fill = { center + static_cast<int>((width / 2 - 2) * normalized), y + 2, center, y + height - 2 };
    }

    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &fill, brush);
    DeleteObject(brush);
}

const wchar_t* ConnectionText(ConnectionState state)
{
    switch (state) {
        case ConnectionState::ReceivingMotion:
            return L"Receiving motion";
        case ConnectionState::WaitingForSlot:
            return L"Waiting for DSU slot 0";
        default:
            return L"Disconnected";
    }
}

void UpdatePointer(float dt)
{
    std::lock_guard<std::mutex> lock(g_app.mutex);
    if (g_app.paused || g_app.connection != ConnectionState::ReceivingMotion) {
        return;
    }

    float pitch = g_app.sample.gyroPitch;
    float yaw = g_app.sample.gyroYaw;
    float roll = g_app.sample.gyroRoll;
    if (g_app.swapYawRoll) {
        std::swap(yaw, roll);
    }
    if (g_app.invertPitch) pitch = -pitch;
    if (g_app.invertYaw) yaw = -yaw;
    if (g_app.invertRoll) roll = -roll;

    g_app.pointerX = std::clamp(g_app.pointerX + yaw * g_app.sensitivity * dt * 60.0f, 0.0f, 1.0f);
    g_app.pointerY = std::clamp(g_app.pointerY - pitch * g_app.sensitivity * dt * 60.0f, 0.0f, 1.0f);
}

void Render(HWND hwnd, HDC hdc)
{
    RECT client{};
    GetClientRect(hwnd, &client);

    HBRUSH bg = CreateSolidBrush(RGB(24, 25, 28));
    FillRect(hdc, &client, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);

    AppState snapshot;
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        snapshot.sample = g_app.sample;
        snapshot.connection = g_app.connection;
        snapshot.lastPacketMs = g_app.lastPacketMs;
        snapshot.pointerX = g_app.pointerX;
        snapshot.pointerY = g_app.pointerY;
        snapshot.sensitivity = g_app.sensitivity;
        snapshot.invertPitch = g_app.invertPitch;
        snapshot.invertYaw = g_app.invertYaw;
        snapshot.invertRoll = g_app.invertRoll;
        snapshot.swapYawRoll = g_app.swapYawRoll;
        snapshot.paused = g_app.paused;
    }

    RECT screen{ 24, 24, client.right - 330, client.bottom - 24 };
    HBRUSH screenBrush = CreateSolidBrush(RGB(11, 32, 47));
    FillRect(hdc, &screen, screenBrush);
    DeleteObject(screenBrush);

    HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(34, 78, 96));
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, gridPen));
    for (int i = 1; i < 4; ++i) {
        const int x = screen.left + (screen.right - screen.left) * i / 4;
        MoveToEx(hdc, x, screen.top, nullptr);
        LineTo(hdc, x, screen.bottom);
        const int y = screen.top + (screen.bottom - screen.top) * i / 4;
        MoveToEx(hdc, screen.left, y, nullptr);
        LineTo(hdc, screen.right, y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(gridPen);

    const int px = screen.left + static_cast<int>((screen.right - screen.left) * snapshot.pointerX);
    const int py = screen.top + static_cast<int>((screen.bottom - screen.top) * snapshot.pointerY);
    HPEN crossPen = CreatePen(PS_SOLID, 3, RGB(255, 215, 90));
    oldPen = static_cast<HPEN>(SelectObject(hdc, crossPen));
    MoveToEx(hdc, px - 22, py, nullptr);
    LineTo(hdc, px + 22, py);
    MoveToEx(hdc, px, py - 22, nullptr);
    LineTo(hdc, px, py + 22);
    SelectObject(hdc, oldPen);
    DeleteObject(crossPen);
    HBRUSH dotBrush = CreateSolidBrush(RGB(255, 90, 70));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, dotBrush));
    Ellipse(hdc, px - 7, py - 7, px + 7, py + 7);
    SelectObject(hdc, oldBrush);
    DeleteObject(dotBrush);

    const int panelX = client.right - 300;
    int y = 24;
    DrawTextLine(hdc, panelX, y, L"DSU Pointer Tester", RGB(255, 255, 255));
    y += 30;
    const COLORREF stateColor = snapshot.connection == ConnectionState::ReceivingMotion ? RGB(110, 230, 130) : RGB(255, 180, 80);
    DrawTextLine(hdc, panelX, y, std::wstring(L"Status: ") + ConnectionText(snapshot.connection), stateColor);
    y += 24;
    DrawTextLine(hdc, panelX, y, L"Server: 127.0.0.1:26760");
    y += 24;
    DrawTextLine(hdc, panelX, y, L"Slot: 0");
    y += 32;

    float pitch = snapshot.sample.gyroPitch;
    float yaw = snapshot.sample.gyroYaw;
    float roll = snapshot.sample.gyroRoll;
    if (snapshot.swapYawRoll) {
        std::swap(yaw, roll);
    }
    if (snapshot.invertPitch) pitch = -pitch;
    if (snapshot.invertYaw) yaw = -yaw;
    if (snapshot.invertRoll) roll = -roll;

    DrawTextLine(hdc, panelX, y, L"Pitch: " + FormatFloat(pitch) + L" deg/s");
    DrawBar(hdc, panelX, y + 20, 250, 14, pitch, 360.0f, RGB(95, 170, 255));
    y += 46;
    DrawTextLine(hdc, panelX, y, L"Yaw:   " + FormatFloat(yaw) + L" deg/s");
    DrawBar(hdc, panelX, y + 20, 250, 14, yaw, 360.0f, RGB(255, 210, 95));
    y += 46;
    DrawTextLine(hdc, panelX, y, L"Roll:  " + FormatFloat(roll) + L" deg/s");
    DrawBar(hdc, panelX, y + 20, 250, 14, roll, 360.0f, RGB(210, 130, 255));
    y += 50;

    DrawTextLine(hdc, panelX, y, L"Accel X/Y/Z: "
        + FormatFloat(snapshot.sample.accelX) + L", "
        + FormatFloat(snapshot.sample.accelY) + L", "
        + FormatFloat(snapshot.sample.accelZ) + L" g");
    y += 24;
    DrawTextLine(hdc, panelX, y, L"Packets: " + std::to_wstring(snapshot.sample.packetCounter));
    y += 24;
    DrawTextLine(hdc, panelX, y, L"Sensitivity: " + FormatFloat(snapshot.sensitivity, 4));
    y += 32;

    DrawTextLine(hdc, panelX, y, std::wstring(L"1 Pitch invert: ") + (snapshot.invertPitch ? L"ON" : L"OFF"));
    y += 22;
    DrawTextLine(hdc, panelX, y, std::wstring(L"2 Yaw invert:   ") + (snapshot.invertYaw ? L"ON" : L"OFF"));
    y += 22;
    DrawTextLine(hdc, panelX, y, std::wstring(L"3 Roll invert:  ") + (snapshot.invertRoll ? L"ON" : L"OFF"));
    y += 22;
    DrawTextLine(hdc, panelX, y, std::wstring(L"4 Swap yaw/roll: ") + (snapshot.swapYawRoll ? L"ON" : L"OFF"));
    y += 30;

    DrawTextLine(hdc, panelX, y, L"Space/R: recenter");
    y += 22;
    DrawTextLine(hdc, panelX, y, L"+/-: sensitivity");
    y += 22;
    DrawTextLine(hdc, panelX, y, std::wstring(L"P: pause ") + (snapshot.paused ? L"(ON)" : L"(OFF)"));
    y += 22;
    DrawTextLine(hdc, panelX, y, L"Esc: exit");

    DrawTextLine(hdc, screen.left + 12, screen.top + 10, L"Virtual pointer screen", RGB(200, 230, 240));
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, 1, 16, nullptr);
            return 0;
        case WM_TIMER:
            UpdatePointer(1.0f / 60.0f);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
        {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            switch (wParam) {
                case VK_ESCAPE:
                    PostMessage(hwnd, WM_CLOSE, 0, 0);
                    break;
                case '1':
                    g_app.invertPitch = !g_app.invertPitch;
                    break;
                case '2':
                    g_app.invertYaw = !g_app.invertYaw;
                    break;
                case '3':
                    g_app.invertRoll = !g_app.invertRoll;
                    break;
                case '4':
                    g_app.swapYawRoll = !g_app.swapYawRoll;
                    break;
                case VK_SPACE:
                case 'R':
                    g_app.pointerX = 0.5f;
                    g_app.pointerY = 0.5f;
                    break;
                case 'P':
                    g_app.paused = !g_app.paused;
                    break;
                case VK_OEM_PLUS:
                case VK_ADD:
                    g_app.sensitivity = std::min(g_app.sensitivity * 1.25f, 0.1f);
                    break;
                case VK_OEM_MINUS:
                case VK_SUBTRACT:
                    g_app.sensitivity = std::max(g_app.sensitivity / 1.25f, 0.0001f);
                    break;
            }
            return 0;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            Render(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            {
                std::lock_guard<std::mutex> lock(g_app.mutex);
                g_app.running = false;
            }
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register window class.", L"DSU Pointer Tester", MB_ICONERROR);
        return 1;
    }

    g_hwnd = CreateWindowExW(
        0,
        kWindowClass,
        L"DSU Pointer Tester",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1120,
        720,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!g_hwnd) {
        MessageBoxW(nullptr, L"Failed to create window.", L"DSU Pointer Tester", MB_ICONERROR);
        return 1;
    }

    g_dsuThread = std::thread(DsuThread);

    ShowWindow(g_hwnd, showCmd);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        g_app.running = false;
    }
    if (g_dsuThread.joinable()) {
        g_dsuThread.join();
    }

    return static_cast<int>(msg.wParam);
}
