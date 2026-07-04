#define NOMINMAX
#include <Windows.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <winusb.h>
#include <setupapi.h>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "JoyConDecoder.h"
#include "DsuServer.h"
#include <ViGEm/Client.h>
#include <ViGEm/Common.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ─── Constants ───────────────────────────────────────────────────────────────

constexpr USHORT  SWITCH_VID        = 0x057E;
constexpr USHORT  PRO_CTRL_PID      = 0x2069;

// Nintendo USB command IDs (Report ID 0x80)
constexpr uint8_t USB_CMD_HANDSHAKE    = 0x02;
constexpr uint8_t USB_CMD_HID_ENABLE   = 0x03;
constexpr uint8_t USB_CMD_KEEPALIVE_OFF= 0x04;

// Subcommand IDs (sent via Report ID 0x01)
constexpr uint8_t SUBCMD_SET_INPUT_MODE = 0x03;
constexpr uint8_t SUBCMD_ENABLE_IMU     = 0x40;
constexpr uint8_t INPUT_MODE_FULL       = 0x30;

// ─── App state ───────────────────────────────────────────────────────────────

enum class AppScreen    { Setup, Connecting, Running };
enum class UpdatePolicy { LowLatency, Balanced120Hz, Legacy60Hz };

static AppScreen    g_screen       = AppScreen::Setup;
static UpdatePolicy g_updatePolicy = UpdatePolicy::LowLatency;
static bool         g_dsuEnabled   = false;

static PVIGEM_CLIENT g_vigem     = nullptr;
static PVIGEM_TARGET g_ds4       = nullptr;
static DsuServer     g_dsuServer;

static HANDLE            g_hidDevice  = INVALID_HANDLE_VALUE;
static std::atomic<bool> g_running   {false};
static std::thread       g_readThread;
static std::string       g_connectError;
static std::string       g_statusMsg = "Ready.";

// ─── WinUSB globals ───────────────────────────────────────────────────────────
// {88BAE032-5A81-49F0-BC3D-A4FF138216D6} — GUID Zadig assigns when installing WinUSB
static const GUID ZADIG_WINUSB_GUID =
    {0x88BAE032, 0x5A81, 0x49F0, {0xBC, 0x3D, 0xA4, 0xFF, 0x13, 0x82, 0x16, 0xD6}};

static bool                    g_useWinUSB    = false;
static WINUSB_INTERFACE_HANDLE g_winusbHandle = nullptr;
static BYTE                    g_pipeIn       = 0;
static BYTE                    g_pipeOut      = 0;

// ─── Logging ─────────────────────────────────────────────────────────────────

static std::mutex              g_logMutex;
static std::vector<std::string> g_logLines;

static void AppLog(const std::string& s) {
    printf("[LOG] %s\n", s.c_str());
    fflush(stdout);
    OutputDebugStringA(("[SW2Pro] " + s + "\n").c_str());
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logLines.push_back(s);
    if (g_logLines.size() > 300) g_logLines.erase(g_logLines.begin());
}

// ─── WinUSB helpers ──────────────────────────────────────────────────────────

// Parse "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" GUID string (no ole32 needed)
static bool ParseGUIDStr(const WCHAR* s, GUID& g) {
    unsigned a,b,c, d[8];
    if (swscanf_s(s, L"{%8X-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X}",
                  &a,&b,&c,&d[0],&d[1],&d[2],&d[3],&d[4],&d[5],&d[6],&d[7]) != 11)
        return false;
    g.Data1=a; g.Data2=(USHORT)b; g.Data3=(USHORT)c;
    for(int i=0;i<8;i++) g.Data4[i]=(BYTE)d[i];
    return true;
}

// Open the WinUSB interface for our device.
// Zadig 2.9+ writes a unique DeviceInterfaceGUIDs value per device into the
// device's registry "Device Parameters" key.  We read it dynamically instead
// of relying on a hard-coded GUID.
static HANDLE FindProControllerWinUSB() {
    // Enumerate devices in the WinUSB/libusb device class
    HDEVINFO di = SetupDiGetClassDevs(&ZADIG_WINUSB_GUID, nullptr, nullptr, DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) {
        AppLog("[WINUSB] SetupDiGetClassDevs failed");
        return INVALID_HANDLE_VALUE;
    }

    HANDLE result = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA dd = {}; dd.cbSize = sizeof(dd);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd) && result == INVALID_HANDLE_VALUE; i++) {
        char instId[512] = {};
        if (!SetupDiGetDeviceInstanceIdA(di, &dd, instId, sizeof(instId), nullptr)) continue;

        char upper[512]; strncpy_s(upper, instId, 511);
        for (char* p = upper; *p; p++) *p = (char)toupper((unsigned char)*p);
        AppLog("[WINUSB] Class device: " + std::string(instId));

        if (!strstr(upper, "VID_057E") || !strstr(upper, "PID_2069")) continue;
        AppLog("[WINUSB] VID/PID matched: " + std::string(instId));

        // Read DeviceInterfaceGUIDs (Zadig 2.9+) or DeviceInterfaceGUID from registry
        HKEY regKey = SetupDiOpenDevRegKey(di, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (regKey == INVALID_HANDLE_VALUE) { AppLog("[WINUSB] Can't open regkey"); continue; }

        WCHAR guidBuf[256] = {};
        DWORD type = 0, sz = sizeof(guidBuf);
        LSTATUS ls = RegQueryValueExW(regKey, L"DeviceInterfaceGUIDs", nullptr, &type, (LPBYTE)guidBuf, &sz);
        if (ls != ERROR_SUCCESS) {
            sz = sizeof(guidBuf);
            ls = RegQueryValueExW(regKey, L"DeviceInterfaceGUID", nullptr, &type, (LPBYTE)guidBuf, &sz);
        }
        RegCloseKey(regKey);

        if (ls != ERROR_SUCCESS) { AppLog("[WINUSB] No DeviceInterfaceGUID in registry"); continue; }

        char guidA[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, guidBuf, -1, guidA, 64, nullptr, nullptr);
        AppLog("[WINUSB] DeviceInterfaceGUID = " + std::string(guidA));

        GUID ifGuid;
        if (!ParseGUIDStr(guidBuf, ifGuid)) { AppLog("[WINUSB] GUID parse failed"); continue; }

        // Enumerate device interfaces with this GUID and find our VID/PID
        HDEVINFO di2 = SetupDiGetClassDevs(&ifGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (di2 == INVALID_HANDLE_VALUE) continue;

        SP_DEVICE_INTERFACE_DATA ifd = {}; ifd.cbSize = sizeof(ifd);
        for (DWORD j = 0; SetupDiEnumDeviceInterfaces(di2, nullptr, &ifGuid, j, &ifd) && result == INVALID_HANDLE_VALUE; j++) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetail(di2, &ifd, nullptr, 0, &need, nullptr);
            if (!need) continue;
            auto* det = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(need);
            det->cbSize = sizeof(*det);
            if (SetupDiGetDeviceInterfaceDetail(di2, &ifd, det, need, nullptr, nullptr)) {
                // DevicePath is already ANSI in non-Unicode build — use directly
                std::string p(det->DevicePath);
                for (auto& c : p) c = (char)toupper((unsigned char)c);
                AppLog("[WINUSB] Interface path: " + p.substr(0, 80));
                if (p.find("VID_057E") != std::string::npos && p.find("PID_2069") != std::string::npos && p.find("MI_00") != std::string::npos) {
                    HANDLE h = CreateFile(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                           0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                    if (h != INVALID_HANDLE_VALUE) {
                        AppLog("[WINUSB] Opened exclusively!");
                        result = h;
                    } else {
                        AppLog("[WINUSB] CreateFile failed: " + std::to_string(GetLastError()));
                    }
                }
            }
            free(det);
        }
        SetupDiDestroyDeviceInfoList(di2);
    }
    SetupDiDestroyDeviceInfoList(di);
    return result;
}

// Initialize WinUSB handle and discover IN/OUT interrupt pipes.
static bool InitWinUSBIface(HANDLE hDev) {
    if (!WinUsb_Initialize(hDev, &g_winusbHandle)) {
        AppLog("[WINUSB] WinUsb_Initialize failed: " + std::to_string(GetLastError()));
        return false;
    }
    USB_INTERFACE_DESCRIPTOR ifDesc = {};
    WinUsb_QueryInterfaceSettings(g_winusbHandle, 0, &ifDesc);
    AppLog("[WINUSB] Interface: " + std::to_string(ifDesc.bNumEndpoints) + " endpoints");

    g_pipeIn = 0; g_pipeOut = 0;
    for (BYTE e = 0; e < ifDesc.bNumEndpoints; e++) {
        WINUSB_PIPE_INFORMATION pipe = {};
        WinUsb_QueryPipe(g_winusbHandle, 0, e, &pipe);
        std::ostringstream oss;
        oss << "[WINUSB]   Pipe 0x" << std::hex << (int)pipe.PipeId
            << " type=" << (int)pipe.PipeType << " maxPkt=" << pipe.MaximumPacketSize;
        AppLog(oss.str());
        if (pipe.PipeType == UsbdPipeTypeInterrupt || pipe.PipeType == UsbdPipeTypeBulk) {
            if (pipe.PipeId & 0x80) g_pipeIn  = pipe.PipeId;
            else                    g_pipeOut = pipe.PipeId;
        }
    }
    if (!g_pipeIn || !g_pipeOut) {
        AppLog("[WINUSB] Could not find IN/OUT pipes!");
        return false;
    }
    AppLog("[WINUSB] PipeIn=0x" + [&]{ std::ostringstream o; o<<std::hex<<(int)g_pipeIn; return o.str(); }()
         + " PipeOut=0x" + [&]{ std::ostringstream o; o<<std::hex<<(int)g_pipeOut; return o.str(); }());
    return true;
}

// Write one packet to the OUT pipe synchronously (used during init).
static void WinUSBSendPkt(const uint8_t* data, size_t len) {
    std::vector<uint8_t> buf(64, 0);
    memcpy(buf.data(), data, std::min(len, (size_t)64));
    ULONG written = 0;
    if (!WinUsb_WritePipe(g_winusbHandle, g_pipeOut, buf.data(), 64, &written, nullptr))
        AppLog("[WINUSB] WritePipe failed: " + std::to_string(GetLastError()));
}

// Nintendo Switch 2 Pro Controller USB init sequence (from procon2-driver/controller.go)
// followed by subcommands to activate full 0x30 input reports + IMU.
static void InitProControllerWinUSB() {
    // 17-packet init sequence
    static const std::vector<std::vector<uint8_t>> INIT_SEQ = {
        {0x03,0x91,0x00,0x0d,0x00,0x08,0x00,0x00,0x01,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
        {0x07,0x91,0x00,0x01,0x00,0x00,0x00,0x00},
        {0x16,0x91,0x00,0x01,0x00,0x00,0x00,0x00},
        {0x15,0x91,0x00,0x01,0x00,0x0e,0x00,0x00,0x00,0x02,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
        {0x15,0x91,0x00,0x02,0x00,0x11,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
        {0x15,0x91,0x00,0x03,0x00,0x01,0x00,0x00,0x00},
        {0x09,0x91,0x00,0x07,0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
        {0x0c,0x91,0x00,0x02,0x00,0x04,0x00,0x00,0x27,0x00,0x00,0x00},
        {0x11,0x91,0x00,0x03,0x00,0x00,0x00,0x00},
        {0x0a,0x91,0x00,0x08,0x00,0x14,0x00,0x00,0x01,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x35,0x00,0x46,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
        {0x0c,0x91,0x00,0x04,0x00,0x04,0x00,0x00,0x27,0x00,0x00,0x00},
        {0x03,0x91,0x00,0x0a,0x00,0x04,0x00,0x00,0x09,0x00,0x00,0x00},
        {0x10,0x91,0x00,0x01,0x00,0x00,0x00,0x00},
        {0x01,0x91,0x00,0x0c,0x00,0x00,0x00,0x00},
        {0x03,0x91,0x00,0x01,0x00,0x00,0x00},
        {0x0a,0x91,0x00,0x02,0x00,0x04,0x00,0x00,0x03,0x00,0x00},
        {0x09,0x91,0x00,0x07,0x00,0x08,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    };

    AppLog("[WINUSB] Sending 17-packet init sequence...");
    for (size_t i = 0; i < INIT_SEQ.size(); i++) {
        WinUSBSendPkt(INIT_SEQ[i].data(), INIT_SEQ[i].size());
        Sleep(15);
    }

    // Standard subcommands: set input report mode to 0x30 + enable IMU
    static uint8_t subcmdCtr = 0;
    AppLog("[WINUSB] Setting input mode 0x30...");
    uint8_t modeCmd[] = {0x01, subcmdCtr++, 0x00,0x01,0x40,0x40,0x00,0x01,0x40,0x40, 0x03, 0x30};
    WinUSBSendPkt(modeCmd, sizeof(modeCmd));
    Sleep(100);

    AppLog("[WINUSB] Enabling IMU...");
    uint8_t imuCmd[] = {0x01, subcmdCtr++, 0x00,0x01,0x40,0x40,0x00,0x01,0x40,0x40, 0x40, 0x01};
    WinUSBSendPkt(imuCmd, sizeof(imuCmd));
    Sleep(100);

    AppLog("[WINUSB] Init complete — controller should now send 0x30 reports");
}

// ─── USB HID helpers ─────────────────────────────────────────────────────────

// Queried from HID descriptor at open time
static USHORT g_hidInputReportLen  = 64;
static USHORT g_hidOutputReportLen = 64;

static HANDLE FindProController() {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevs(&hidGuid, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        AppLog("[USB] SetupDiGetClassDevs failed, error=" + std::to_string(GetLastError()));
        return INVALID_HANDLE_VALUE;
    }

    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifData); i++) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;

        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(needed);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, detail, needed, nullptr, nullptr)) {
            // Try exclusive read first, fall back to shared.
            // Exclusive succeeds only if no other app has the device open.
            HANDLE h = CreateFile(detail->DevicePath,
                                   GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            bool exclusiveOk = (h != INVALID_HANDLE_VALUE);
            if (!exclusiveOk) {
                h = CreateFile(detail->DevicePath,
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            }
            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attrs = {};
                attrs.Size = sizeof(attrs);
                if (HidD_GetAttributes(h, &attrs)) {
                    std::ostringstream oss;
                    oss << "[USB] HID found: VID=0x" << std::hex << std::setw(4) << std::setfill('0') << attrs.VendorID
                        << " PID=0x" << std::setw(4) << attrs.ProductID;
                    AppLog(oss.str());
                    if (attrs.VendorID == SWITCH_VID && attrs.ProductID == PRO_CTRL_PID) {
                        AppLog(exclusiveOk
                            ? "[USB] Opened with EXCLUSIVE read — Forza cannot access real HID"
                            : "[USB] Opened with SHARED read — close Chrome/Steam first for exclusive access");
                        // Query report sizes from HID descriptor
                        PHIDP_PREPARSED_DATA preparsed = nullptr;
                        if (HidD_GetPreparsedData(h, &preparsed)) {
                            HIDP_CAPS caps = {};
                            if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
                                g_hidInputReportLen  = caps.InputReportByteLength;
                                g_hidOutputReportLen = caps.OutputReportByteLength;
                                AppLog("[USB] HID caps: InputLen=" + std::to_string(caps.InputReportByteLength)
                                    + " OutputLen=" + std::to_string(caps.OutputReportByteLength)
                                    + " Usage=0x" + [&]{ std::ostringstream o; o << std::hex << caps.Usage; return o.str(); }());
                            }
                            HidD_FreePreparsedData(preparsed);
                        }
                        AppLog("[USB] Switch 2 Pro Controller matched!");
                        found = h;
                        free(detail);
                        break;
                    }
                }
                CloseHandle(h);
            }
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

static uint8_t g_subcmdTimer = 0;

// Allocate a zeroed output buffer matching the device's reported output report size
static std::vector<uint8_t> MakeOutBuf() {
    return std::vector<uint8_t>(g_hidOutputReportLen, 0);
}

// Write a Nintendo USB command (report ID 0x80, subcommand in byte 1)
static bool SendUsbCmd(HANDLE hDev, uint8_t cmd) {
    auto buf = MakeOutBuf();
    buf[0] = 0x80;
    buf[1] = cmd;
    BOOL ok = HidD_SetOutputReport(hDev, buf.data(), (ULONG)buf.size());
    if (!ok)
        AppLog("[USB] SendUsbCmd 0x" + [&]{ std::ostringstream o; o << std::hex << (int)cmd; return o.str(); }()
            + " failed, error=" + std::to_string(GetLastError()));
    return ok != FALSE;
}

// Send a Nintendo subcommand (report ID 0x01, subcommand byte at offset 10)
static bool SendSubcmd(HANDLE hDev, uint8_t subCmdId, const uint8_t* data, size_t dataLen) {
    auto buf = MakeOutBuf();
    buf[0] = 0x01;
    buf[1] = g_subcmdTimer++;
    // bytes 2-9: rumble data (leave zero)
    if (buf.size() > 10) buf[10] = subCmdId;
    if (data && dataLen > 0 && buf.size() > 11)
        memcpy(buf.data() + 11, data, std::min(dataLen, buf.size() - 11));
    BOOL ok = HidD_SetOutputReport(hDev, buf.data(), (ULONG)buf.size());
    if (!ok)
        AppLog("[USB] SendSubcmd 0x" + [&]{ std::ostringstream o; o << std::hex << (int)subCmdId; return o.str(); }()
            + " failed, error=" + std::to_string(GetLastError()));
    return ok != FALSE;
}

// Try to activate the controller by sending a haptic wakeup via report 0x02.
// The Switch 2 Pro Controller requires some form of USB output before it starts
// sending input. The haptic format is from procon2-driver (haptics.go).
static bool TrySendActivation(HANDLE hDev, USHORT outLen) {
    if (outLen < 64) { AppLog("[INIT] outLen too small for haptic"); return false; }

    const uint8_t frame[5] = {0x93, 0x35, 0x36, 0x1c, 0x0d};
    for (int i = 0; i < 3; i++) {
        std::vector<uint8_t> buf(outLen, 0);
        buf[0] = 0x02;
        buf[1] = 0x50 | (uint8_t)(i & 0x0F);
        memcpy(&buf[2], frame, 5);
        buf[17] = buf[1];
        memcpy(&buf[18], frame, 5);
        OVERLAPPED ov = {}; ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        BOOL ok = WriteFile(hDev, buf.data(), outLen, nullptr, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            DWORD d = 0; GetOverlappedResult(hDev, &ov, &d, TRUE); ok = TRUE;
        }
        CloseHandle(ov.hEvent);
        AppLog("[INIT] haptic frame " + std::to_string(i) + " -> " + (ok ? "OK" : "FAIL err=" + std::to_string(GetLastError())));
        Sleep(4);
    }
    // Stop
    std::vector<uint8_t> stop(outLen, 0);
    stop[0] = 0x02; stop[1] = 0x50; stop[17] = 0x50;
    OVERLAPPED ov2 = {}; ov2.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    WriteFile(hDev, stop.data(), outLen, nullptr, &ov2);
    if (GetLastError() == ERROR_IO_PENDING) { DWORD d = 0; GetOverlappedResult(hDev, &ov2, &d, TRUE); }
    CloseHandle(ov2.hEvent);
    return true;
}

// Wait up to timeoutMs for a single input report. Returns true if data arrived.
static bool WaitForInput(HANDLE hDev, USHORT inLen, int timeoutMs) {
    DWORD bufSz = std::max((USHORT)64, inLen);
    std::vector<uint8_t> buf(bufSz);
    OVERLAPPED ov = {}; ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = ReadFile(hDev, buf.data(), bufSz, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) { CloseHandle(ov.hEvent); return false; }
    bool got = (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0);
    CancelIo(hDev);
    CloseHandle(ov.hEvent);
    return got;
}

// ─── Update policy ───────────────────────────────────────────────────────────

using SteadyClock = std::chrono::steady_clock;
using TimePoint   = SteadyClock::time_point;

static std::chrono::microseconds PolicyInterval(UpdatePolicy p) {
    switch(p) {
        case UpdatePolicy::Balanced120Hz: return std::chrono::microseconds(8333);
        case UpdatePolicy::Legacy60Hz:    return std::chrono::microseconds(16667);
        default:                          return std::chrono::microseconds(0);
    }
}

static bool ShouldEmit(UpdatePolicy p, TimePoint& last, TimePoint now) {
    auto iv = PolicyInterval(p);
    if (iv.count() == 0 || last == TimePoint{} || now - last >= iv) { last = now; return true; }
    return false;
}

// ─── Read thread ─────────────────────────────────────────────────────────────

static void ReadLoop(HANDLE hDev, PVIGEM_CLIENT vigem, PVIGEM_TARGET ds4) {
    AppLog(g_useWinUSB
        ? "[READ] Thread started — WinUSB mode (0x30 reports)"
        : "[READ] Thread started — HID mode (0x09 reports), InputLen=" + std::to_string(g_hidInputReportLen));

    const DWORD readBufSize = g_useWinUSB ? 64 : std::max((USHORT)64, g_hidInputReportLen);
    std::vector<uint8_t> buf(readBufSize, 0);
    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    TimePoint lastEmit{};
    int loggedReports = 0;
    int unknownLog    = 0;
    int noDataCount   = 0;

    while (g_running.load(std::memory_order_acquire)) {
        ResetEvent(ov.hEvent);
        std::fill(buf.begin(), buf.end(), 0);
        BOOL pending;
        if (g_useWinUSB) {
            ULONG dummy = 0;
            pending = !WinUsb_ReadPipe(g_winusbHandle, g_pipeIn, buf.data(), readBufSize, nullptr, &ov);
        } else {
            pending = !ReadFile(hDev, buf.data(), readBufSize, nullptr, &ov);
        }

        if (pending && GetLastError() != ERROR_IO_PENDING) {
            AppLog("[READ] ReadFile error: " + std::to_string(GetLastError()));
            break;
        }

        DWORD waitRes = WaitForSingleObject(ov.hEvent, 200);
        if (waitRes == WAIT_TIMEOUT) {
            noDataCount++;
            if (noDataCount == 10) { // 2 seconds with no data
                AppLog("[READ] No data for 2s — controller may need activation via procon2tool");
                g_statusMsg = "No input. Open procon2tool (handheldlegend.github.io/procon2tool), activate controller, then this app will work.";
            }
            continue;
        }
        if (waitRes != WAIT_OBJECT_0) break;

        DWORD bytesRead = 0;
        if (!GetOverlappedResult(hDev, &ov, &bytesRead, FALSE) || bytesRead == 0) continue;
        noDataCount = 0;

        uint8_t reportId = buf[0];

        if (loggedReports < 3) {
            std::ostringstream oss;
            oss << "[READ] #" << ++loggedReports << " ID=0x" << std::hex << (int)reportId
                << " len=" << std::dec << bytesRead << " | ";
            for (DWORD k = 0; k < std::min(bytesRead, (DWORD)16); k++)
                oss << std::hex << std::setw(2) << std::setfill('0') << (int)buf[k] << " ";
            AppLog(oss.str());
        }

        // Accept 0x09 (standard HID) and 0x30/0x31 (Nintendo full format)
        bool validReport = (reportId == 0x09 || reportId == 0x30 || reportId == 0x31);
        if (!validReport) {
            if (unknownLog++ < 5)
                AppLog("[READ] Skipping report ID=0x" + [&]{ std::ostringstream o; o << std::hex << (int)reportId; return o.str(); }());
            continue;
        }

        auto now = SteadyClock::now();
        if (!ShouldEmit(g_updatePolicy, lastEmit, now)) continue;

        std::vector<uint8_t> report(buf.begin(), buf.begin() + bytesRead);

        XUSB_REPORT xreport;
        if (reportId == 0x30 || reportId == 0x31) {
            report.resize(0x3C, 0);
            xreport = GenerateProControllerXboxReport(report);
        } else {
            // 0x09: standard HID gamepad format (works for both HID and WinUSB)
            xreport = GenerateSwitch2ProXboxReport(report);
        }
        vigem_target_x360_update(vigem, ds4, xreport);
    }

    CloseHandle(ov.hEvent);
    AppLog("[READ] Thread stopped");
}

// ─── ViGEm helpers ───────────────────────────────────────────────────────────

static bool InitViGEm() {
    if (g_vigem) return true;
    g_vigem = vigem_alloc();
    if (!g_vigem) { AppLog("[VIGEM] vigem_alloc() returned null — ViGEmBus not installed?"); return false; }
    VIGEM_ERROR err = vigem_connect(g_vigem);
    if (!VIGEM_SUCCESS(err)) {
        AppLog("[VIGEM] vigem_connect() failed, code=" + std::to_string((int)err)
            + " — Is ViGEmBus installed and running?");
        vigem_free(g_vigem); g_vigem = nullptr; return false;
    }
    AppLog("[VIGEM] Connected OK");
    return true;
}

static PVIGEM_TARGET AddXbox360() {
    auto t = vigem_target_x360_alloc();
    if (!t) { AppLog("[VIGEM] vigem_target_x360_alloc() failed"); return nullptr; }
    VIGEM_ERROR err = vigem_target_add(g_vigem, t);
    if (!VIGEM_SUCCESS(err)) {
        AppLog("[VIGEM] vigem_target_add() failed, code=" + std::to_string((int)err));
        vigem_target_free(t); return nullptr;
    }
    AppLog("[VIGEM] Xbox 360 virtual controller added");
    return t;
}

// ─── Connection lifecycle ────────────────────────────────────────────────────

static void DoConnect() {
    g_connectError.clear();
    g_screen = AppScreen::Connecting;

    std::thread([]() {
        AppLog("[CONNECT] Searching for Switch 2 Pro Controller (VID=057E PID=2069)...");

        if (!InitViGEm()) {
            g_connectError = "ViGEmBus not installed or not running. Install ViGEmBus driver.";
            g_screen = AppScreen::Setup; return;
        }

        // ── Try WinUSB first (requires Zadig driver installed) ──────────────
        HANDLE hDev = FindProControllerWinUSB();
        if (hDev != INVALID_HANDLE_VALUE) {
            AppLog("[CONNECT] WinUSB device opened — initialising Nintendo protocol...");
            if (!InitWinUSBIface(hDev)) {
                g_connectError = "WinUSB initialisation failed. Try reinstalling Zadig driver.";
                CloseHandle(hDev); g_screen = AppScreen::Setup; return;
            }
            InitProControllerWinUSB();
            g_useWinUSB = true;
            g_statusMsg = "Connected (WinUSB — exclusive, Forza cannot see raw HID)";
            AppLog("[CONNECT] WinUSB mode active");
        } else {
            // ── Fall back to standard HID ─────────────────────────────────
            AppLog("[CONNECT] WinUSB not found (Zadig not installed?) — falling back to HID...");
            hDev = FindProController();
            if (hDev == INVALID_HANDLE_VALUE) {
                g_connectError = "Controller not found. Connect via USB. For exclusive access install Zadig (winusb).";
                g_screen = AppScreen::Setup; return;
            }
            AppLog("[CONNECT] HID device opened");
            TrySendActivation(hDev, g_hidOutputReportLen);
            bool activated = WaitForInput(hDev, g_hidInputReportLen, 2000);
            if (!activated) {
                AppLog("[CONNECT] No input — controller may need activation");
                g_statusMsg = "HID mode — no input yet. Try procon2tool once if stuck.";
            } else {
                g_statusMsg = "Connected (HID shared — Forza may still see raw controller)";
            }
            g_useWinUSB = false;
        }

        g_ds4 = AddXbox360();
        if (!g_ds4) {
            g_connectError = "Failed to create DS4 virtual controller (ViGEmBus issue).";
            CloseHandle(hDev); g_screen = AppScreen::Setup; return;
        }

        if (g_dsuEnabled) {
            g_dsuServer.Start();
            g_dsuServer.SetControllerConnected(0);
        }

        g_hidDevice = hDev;
        g_running.store(true, std::memory_order_release);
        g_readThread = std::thread(ReadLoop, hDev, g_vigem, g_ds4);

        AppLog("[CONNECT] Switch 2 Pro Controller running!");
        g_screen = AppScreen::Running;
    }).detach();
}

static void DoDisconnect() {
    g_running.store(false, std::memory_order_release);
    if (g_useWinUSB && g_winusbHandle)
        WinUsb_AbortPipe(g_winusbHandle, g_pipeIn);
    if (g_readThread.joinable()) g_readThread.join();

    if (g_winusbHandle) { WinUsb_Free(g_winusbHandle); g_winusbHandle = nullptr; }
    g_useWinUSB = false;

    if (g_hidDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hidDevice);
        g_hidDevice = INVALID_HANDLE_VALUE;
    }
    if (g_ds4 && g_vigem) {
        vigem_target_remove(g_vigem, g_ds4);
        vigem_target_free(g_ds4);
        g_ds4 = nullptr;
    }
    g_dsuServer.Stop();
    AppLog("[CONNECT] Disconnected");
    g_screen = AppScreen::Setup;
}

// ─── D3D11 ───────────────────────────────────────────────────────────────────

static ID3D11Device*           g_pd3dDevice       = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain        = nullptr;
static ID3D11RenderTargetView* g_mainRTV           = nullptr;
static HWND                    g_hwnd              = nullptr;

static void CreateRTV() {
    ID3D11Texture2D* bb = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_pd3dDevice->CreateRenderTargetView(bb, nullptr, &g_mainRTV);
    bb->Release();
}
static void CleanupRTV() { if (g_mainRTV){ g_mainRTV->Release(); g_mainRTV=nullptr; } }

static bool CreateDX11(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount=2; sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator=60; sd.BufferDesc.RefreshRate.Denominator=1;
    sd.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=hwnd;
    sd.SampleDesc.Count=1; sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext)))
        return false;
    CreateRTV(); return true;
}
static void CleanupDX11() {
    CleanupRTV();
    if (g_pSwapChain)        { g_pSwapChain->Release();       g_pSwapChain=nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext=nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice=nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch(msg) {
        case WM_SIZE:
            if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
                CleanupRTV();
                g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRTV();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ─── GUI ─────────────────────────────────────────────────────────────────────

static void DrawLog() {
    if (ImGui::CollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("##log", {-1, 180}, true);
        std::lock_guard<std::mutex> lk(g_logMutex);
        for (auto& line : g_logLines)
            ImGui::TextUnformatted(line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
    }
}

static void DrawSetupScreen() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##setup", nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse);

    ImGui::Spacing();
    const char* title = "Switch 2 Pro Controller";
    ImGui::SetCursorPosX((io.DisplaySize.x - ImGui::CalcTextSize(title).x) * 0.5f);
    ImGui::TextColored({0.4f,0.8f,1.f,1.f}, "%s", title);
    ImGui::Separator(); ImGui::Spacing();

    ImGui::TextWrapped("Connect your Switch 2 Pro Controller via USB, then click Connect.");
    ImGui::Spacing();

    if (!g_connectError.empty()) {
        ImGui::TextColored({1.f,0.3f,0.3f,1.f}, "%s", g_connectError.c_str());
        ImGui::Spacing();
    }

    if (ImGui::CollapsingHeader("Settings")) {
        ImGui::Indent(10); ImGui::Spacing();
        const char* policies[] = {"Low Latency (immediate)", "Balanced 120 Hz", "Legacy 60 Hz"};
        int pol = (int)g_updatePolicy;
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("Update Policy", &pol, policies, 3))
            g_updatePolicy = (UpdatePolicy)pol;
        ImGui::Checkbox("Enable DSU Server (Cemuhook / gyro forwarding)", &g_dsuEnabled);
        ImGui::Unindent(10); ImGui::Spacing();
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    float btnW = 160.f;
    ImGui::SetCursorPosX((io.DisplaySize.x - btnW) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Button,        {0.2f,0.6f,0.2f,1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.3f,0.75f,0.3f,1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.15f,0.5f,0.15f,1.f});
    if (ImGui::Button("Connect", {btnW, 36})) DoConnect();
    ImGui::PopStyleColor(3);
    ImGui::Spacing();

    DrawLog();
    ImGui::End();
}

static void DrawConnectingScreen() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##connecting", nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove);

    ImGui::Spacing();
    const char* title = "Connecting...";
    ImGui::SetCursorPosX((io.DisplaySize.x - ImGui::CalcTextSize(title).x) * 0.5f);
    ImGui::TextColored({0.4f,0.8f,1.f,1.f}, "%s", title);
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    static float spin = 0.f;
    spin += ImGui::GetIO().DeltaTime * 3.f;
    const char* spinners[] = {"|", "/", "-", "\\"};
    ImGui::Text("[%s] Initializing Switch 2 Pro Controller over USB...", spinners[(int)(spin) % 4]);
    ImGui::Spacing();

    DrawLog();
    ImGui::End();
}

static void DrawRunningScreen() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##running", nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove);

    ImGui::TextColored({0.4f,0.8f,1.f,1.f}, "Switch 2 Pro Controller");
    ImGui::SameLine(io.DisplaySize.x - 140.f);
    ImGui::PushStyleColor(ImGuiCol_Button,        {0.6f,0.2f,0.2f,1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.75f,0.3f,0.3f,1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.5f,0.15f,0.15f,1.f});
    if (ImGui::Button("Disconnect", {120, 0})) DoDisconnect();
    ImGui::PopStyleColor(3);
    ImGui::Separator(); ImGui::Spacing();

    if (g_statusMsg == "Connected!")
        ImGui::TextColored({0.3f,1.f,0.3f,1.f}, "Connected  —  emulating as Xbox 360 (XInput) via ViGEmBus");
    else
        ImGui::TextColored({1.f,0.8f,0.2f,1.f}, "%s", g_statusMsg.c_str());
    if (g_dsuServer.IsRunning())
        ImGui::TextColored({0.3f,1.f,0.3f,1.f}, "DSU Server active (UDP port 26760)");
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Settings")) {
        ImGui::Indent(10);
        const char* policies[] = {"Low Latency", "Balanced 120 Hz", "Legacy 60 Hz"};
        int pol = (int)g_updatePolicy;
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("Update Policy##run", &pol, policies, 3))
            g_updatePolicy = (UpdatePolicy)pol;
        ImGui::Unindent(10); ImGui::Spacing();
    }

    DrawLog();
    ImGui::End();
}

// ─── WinMain ─────────────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    AllocConsole();
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    SetConsoleTitleW(L"Switch 2 Pro Controller — debug log");
    printf("[INIT] Switch 2 Pro Controller starting (VID=057E PID=2069)\n");

    LoadCalibrationProfiles("calibration.json");

    WNDCLASSEXW wc{sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L, hInst,
                   nullptr, nullptr, nullptr, nullptr, L"sw2proctl", nullptr};
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowW(L"sw2proctl", L"Switch 2 Pro Controller",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           740, 520, nullptr, nullptr, hInst, nullptr);

    if (!CreateDX11(g_hwnd)) {
        DestroyWindow(g_hwnd); UnregisterClassW(wc.lpszClassName, hInst); return 1;
    }
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding  = 6.f;
    style.FrameRounding   = 4.f;
    style.GrabRounding    = 4.f;
    style.WindowBorderSize = 0.f;
    style.Colors[ImGuiCol_WindowBg] = {0.08f, 0.08f, 0.10f, 1.f};
    style.Colors[ImGuiCol_Header]   = {0.18f, 0.35f, 0.55f, 1.f};

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        switch (g_screen) {
            case AppScreen::Setup:      DrawSetupScreen();      break;
            case AppScreen::Connecting: DrawConnectingScreen(); break;
            case AppScreen::Running:    DrawRunningScreen();    break;
        }

        ImGui::Render();
        const float cc[4] = {0.06f, 0.06f, 0.08f, 1.f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRTV, cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    DoDisconnect();
    if (g_vigem) { vigem_disconnect(g_vigem); vigem_free(g_vigem); g_vigem = nullptr; }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDX11();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
