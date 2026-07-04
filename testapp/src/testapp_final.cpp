// testapp_final.cpp — connectyourpro: Nintendo Pro Controller → PC bridge
//
// Supports:
//  - Switch 2 Pro Controller (USB): built-in procon2tool "step 1" — opens the
//    vendor bulk interface (MI_01) via WinUSB and sends the exact init sequence
//    from handheldlegend.github.io/procon2tool. No browser needed.
//  - Switch 1 Pro Controller (USB, beta): classic Nintendo HID handshake.
//  - Output as Xbox 360 (XInput) or DualShock 4 via ViGEmBus.
//  - Controller-mockup button remapping, on/off toggle, settings persisted
//    to connectyourpro.ini.
//
// The previous experimental version is preserved unchanged in testapp.cpp
// (built as testapp_old.exe).

#define NOMINMAX
#include <Windows.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <winusb.h>
#include <usb.h>
#include <setupapi.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
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

constexpr USHORT SWITCH_VID    = 0x057E;
constexpr USHORT PROCON2_PID   = 0x2069;   // Switch 2 Pro Controller (USB)
constexpr USHORT PROCON1_PID   = 0x2009;   // Switch 1 Pro Controller (USB)

// ─── App state ───────────────────────────────────────────────────────────────

enum class AppScreen       { Setup, Connecting, Running };
enum class ControllerModel { ProCon2, ProCon1 };
enum class ConnectionMode  { Usb, Bluetooth };
enum class EmulationTarget { Xbox360, DualShock4 };
// Hid    = HID class device (hid.dll ReadFile)
// RawHid = HID interface (MI_00) bound to WinUSB (e.g. by Zadig) — interrupt pipe
// Bulk   = vendor interface (MI_01) bulk IN pipe
// Ble    = Bluetooth LE GATT notifications (no read thread — event driven)
enum class InputSource     { None, Hid, RawHid, Bulk, Ble };

static AppScreen       g_screen     = AppScreen::Setup;
static ControllerModel g_model      = ControllerModel::ProCon2;
static ConnectionMode  g_connMode   = ConnectionMode::Usb;
static EmulationTarget g_target     = EmulationTarget::Xbox360;
static bool            g_dsuEnabled = false;

static PVIGEM_CLIENT g_vigem = nullptr;
static PVIGEM_TARGET g_pad   = nullptr;
static std::mutex    g_padMutex;   // guards g_pad/g_target swaps vs. input threads
static DsuServer     g_dsuServer;

static HANDLE            g_hidDevice = INVALID_HANDLE_VALUE;
static std::atomic<bool> g_running       {false};
static std::atomic<bool> g_emulationOn   {true};
static std::thread       g_readThread;
static std::string       g_connectError;
static std::string       g_statusMsg = "Ready.";
static InputSource       g_inputSource = InputSource::None;

static ImFont* g_fontBig = nullptr;

// ─── Button remapping ────────────────────────────────────────────────────────
// Logical buttons remappable on both output pads. D-pad, sticks and triggers
// pass through unchanged.

enum LogicalBtn { LB_A, LB_B, LB_X, LB_Y, LB_LB, LB_RB, LB_MINUS, LB_PLUS, LB_LS, LB_RS, LB_COUNT };

static const char* LOGICAL_NAMES[LB_COUNT] =
    { "A", "B", "X", "Y", "L (LB)", "R (RB)", "Minus (Back)", "Plus (Start)", "L-Stick click", "R-Stick click" };
static const char* SHORT_NAMES[LB_COUNT] =
    { "A", "B", "X", "Y", "L", "R", "-", "+", "L3", "R3" };

static const USHORT XUSB_MASKS[LB_COUNT] = {
    XUSB_GAMEPAD_A, XUSB_GAMEPAD_B, XUSB_GAMEPAD_X, XUSB_GAMEPAD_Y,
    XUSB_GAMEPAD_LEFT_SHOULDER, XUSB_GAMEPAD_RIGHT_SHOULDER,
    XUSB_GAMEPAD_BACK, XUSB_GAMEPAD_START,
    XUSB_GAMEPAD_LEFT_THUMB, XUSB_GAMEPAD_RIGHT_THUMB,
};
// DS4 equivalents (A=Cross, B=Circle, X=Square, Y=Triangle). High-12 bits only,
// so the d-pad hat nibble is never touched.
static const USHORT DS4_MASKS[LB_COUNT] = {
    DS4_BUTTON_CROSS, DS4_BUTTON_CIRCLE, DS4_BUTTON_SQUARE, DS4_BUTTON_TRIANGLE,
    DS4_BUTTON_SHOULDER_LEFT, DS4_BUTTON_SHOULDER_RIGHT,
    DS4_BUTTON_SHARE, DS4_BUTTON_OPTIONS,
    DS4_BUTTON_THUMB_LEFT, DS4_BUTTON_THUMB_RIGHT,
};

static int g_remap[LB_COUNT] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

static void RemapReset()      { for (int i = 0; i < LB_COUNT; i++) g_remap[i] = i; }
static void RemapNintendoAB() { RemapReset(); g_remap[LB_A] = LB_B; g_remap[LB_B] = LB_A; g_remap[LB_X] = LB_Y; g_remap[LB_Y] = LB_X; }
static bool RemapIsDefault()  { for (int i = 0; i < LB_COUNT; i++) if (g_remap[i] != i) return false; return true; }

template <typename MaskT>
static USHORT ApplyRemapMasks(USHORT buttons, const MaskT* masks) {
    USHORT out = buttons;
    for (int i = 0; i < LB_COUNT; i++) out &= ~masks[i];          // clear remappable bits
    for (int i = 0; i < LB_COUNT; i++)
        if (buttons & masks[i]) out |= masks[g_remap[i]];         // set mapped bits
    return out;
}

static void ApplyRemapX(XUSB_REPORT& r)      { if (!RemapIsDefault()) r.wButtons = ApplyRemapMasks(r.wButtons, XUSB_MASKS); }
static void ApplyRemapDS4(DS4_REPORT_EX& r)  { if (!RemapIsDefault()) r.Report.wButtons = ApplyRemapMasks(r.Report.wButtons, DS4_MASKS); }

// ─── Live input state (drives the mockup highlight/test view) ────────────────
// Captured from every decoded report BEFORE remapping, so the schematic shows
// the physical button being pressed.

constexpr uint32_t LIVE_DPAD_UP    = 1u << 10;
constexpr uint32_t LIVE_DPAD_DOWN  = 1u << 11;
constexpr uint32_t LIVE_DPAD_LEFT  = 1u << 12;
constexpr uint32_t LIVE_DPAD_RIGHT = 1u << 13;

struct LiveInput {
    std::atomic<uint32_t> buttons{0};
    std::atomic<int> lx{0}, ly{0}, rx{0}, ry{0};   // -32767..32767, Y up = positive
    std::atomic<int> lt{0}, rt{0};                 // 0..255
};
static LiveInput g_live;

static void ClearLive() {
    g_live.buttons = 0;
    g_live.lx = g_live.ly = g_live.rx = g_live.ry = 0;
    g_live.lt = g_live.rt = 0;
}

static void UpdateLiveX(const XUSB_REPORT& r) {
    uint32_t b = 0;
    for (int i = 0; i < LB_COUNT; i++)
        if (r.wButtons & XUSB_MASKS[i]) b |= 1u << i;
    if (r.wButtons & XUSB_GAMEPAD_DPAD_UP)    b |= LIVE_DPAD_UP;
    if (r.wButtons & XUSB_GAMEPAD_DPAD_DOWN)  b |= LIVE_DPAD_DOWN;
    if (r.wButtons & XUSB_GAMEPAD_DPAD_LEFT)  b |= LIVE_DPAD_LEFT;
    if (r.wButtons & XUSB_GAMEPAD_DPAD_RIGHT) b |= LIVE_DPAD_RIGHT;
    g_live.buttons = b;
    g_live.lx = r.sThumbLX; g_live.ly = r.sThumbLY;
    g_live.rx = r.sThumbRX; g_live.ry = r.sThumbRY;
    g_live.lt = r.bLeftTrigger; g_live.rt = r.bRightTrigger;
}

static void UpdateLiveDS4(const DS4_REPORT_EX& r) {
    uint32_t b = 0;
    for (int i = 0; i < LB_COUNT; i++)
        if (r.Report.wButtons & DS4_MASKS[i]) b |= 1u << i;
    switch (r.Report.wButtons & 0xF) {   // hat nibble
        case DS4_BUTTON_DPAD_NORTH:     b |= LIVE_DPAD_UP; break;
        case DS4_BUTTON_DPAD_NORTHEAST: b |= LIVE_DPAD_UP | LIVE_DPAD_RIGHT; break;
        case DS4_BUTTON_DPAD_EAST:      b |= LIVE_DPAD_RIGHT; break;
        case DS4_BUTTON_DPAD_SOUTHEAST: b |= LIVE_DPAD_DOWN | LIVE_DPAD_RIGHT; break;
        case DS4_BUTTON_DPAD_SOUTH:     b |= LIVE_DPAD_DOWN; break;
        case DS4_BUTTON_DPAD_SOUTHWEST: b |= LIVE_DPAD_DOWN | LIVE_DPAD_LEFT; break;
        case DS4_BUTTON_DPAD_WEST:      b |= LIVE_DPAD_LEFT; break;
        case DS4_BUTTON_DPAD_NORTHWEST: b |= LIVE_DPAD_UP | LIVE_DPAD_LEFT; break;
        default: break;
    }
    g_live.buttons = b;
    g_live.lx = ((int)r.Report.bThumbLX - 128) * 257;
    g_live.ly = (128 - (int)r.Report.bThumbLY) * 257;
    g_live.rx = ((int)r.Report.bThumbRX - 128) * 257;
    g_live.ry = (128 - (int)r.Report.bThumbRY) * 257;
    g_live.lt = r.Report.bTriggerL; g_live.rt = r.Report.bTriggerR;
}

// ─── Settings persistence (connectyourpro.ini next to the exe) ──────────────

static const char* INI_PATH = "connectyourpro.ini";

static void SaveSettings() {
    std::ofstream f(INI_PATH, std::ios::trunc);
    if (!f) return;
    f << "model="  << (int)g_model    << "\n";
    f << "conn="   << (int)g_connMode << "\n";
    f << "target=" << (int)g_target   << "\n";
    f << "dsu="    << (g_dsuEnabled ? 1 : 0) << "\n";
    f << "remap=";
    for (int i = 0; i < LB_COUNT; i++) f << g_remap[i] << (i + 1 < LB_COUNT ? "," : "\n");
}

static void LoadSettings() {
    std::ifstream f(INI_PATH);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if      (k == "model")  g_model      = (ControllerModel)std::clamp(std::stoi(v), 0, 1);
            else if (k == "conn")   g_connMode   = (ConnectionMode)std::clamp(std::stoi(v), 0, 1);
            else if (k == "target") g_target     = (EmulationTarget)std::clamp(std::stoi(v), 0, 1);
            else if (k == "dsu")    g_dsuEnabled = (v == "1");
            else if (k == "remap") {
                std::stringstream ss(v);
                std::string tok;
                for (int i = 0; i < LB_COUNT && std::getline(ss, tok, ','); i++)
                    g_remap[i] = std::clamp(std::stoi(tok), 0, LB_COUNT - 1);
            }
        } catch (...) {}
    }
}

// ─── WinUSB state (ProCon2 vendor bulk interface, MI_01) ────────────────────

static HANDLE                  g_usbFile      = INVALID_HANDLE_VALUE;
static WINUSB_INTERFACE_HANDLE g_usbDev       = nullptr;  // handle from WinUsb_Initialize
static WINUSB_INTERFACE_HANDLE g_usbIface     = nullptr;  // interface that owns the bulk pipes
static bool                    g_usbIfaceIsAssociated = false;
static BYTE                    g_pipeIn       = 0;
static BYTE                    g_pipeOut      = 0;

// WinUSB-bound HID interface (MI_00 with Zadig/WinUSB driver) — interrupt pipes.
// When Windows' HID stack doesn't own MI_00, input reports arrive here instead.
static HANDLE                  g_rawFile   = INVALID_HANDLE_VALUE;
static WINUSB_INTERFACE_HANDLE g_rawDev    = nullptr;
static BYTE                    g_rawPipeIn = 0;

// ─── Logging ─────────────────────────────────────────────────────────────────

static std::mutex               g_logMutex;
static std::vector<std::string> g_logLines;

static void AppLog(const std::string& s) {
    printf("[LOG] %s\n", s.c_str());
    fflush(stdout);
    OutputDebugStringA(("[connectyourpro] " + s + "\n").c_str());
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logLines.push_back(s);
    if (g_logLines.size() > 400) g_logLines.erase(g_logLines.begin());
}

static std::string HexDump(const uint8_t* d, size_t n, size_t maxBytes = 16) {
    std::ostringstream o;
    for (size_t i = 0; i < n && i < maxBytes; i++)
        o << std::hex << std::setw(2) << std::setfill('0') << (int)d[i] << " ";
    if (n > maxBytes) o << "...";
    return o.str();
}

// ─── WinUSB vendor interface discovery (ProCon2) ─────────────────────────────
//
// procon2tool works in Chrome because the controller's interface 1 (vendor,
// class 0xFF) is bound to winusb.sys. We find that interface natively:
//   A) read DeviceInterfaceGUID(s) from the registry of every present devnode
//      matching VID_057E&PID_2069 (covers MS-OS-descriptor installs and Zadig),
//   B) fall back to the generic USB device interface GUID.
// Every candidate path is opened and validated: it must expose bulk IN + OUT.

static bool ParseGUIDStr(const WCHAR* s, GUID& g) {
    unsigned a, b, c, d[8];
    if (swscanf_s(s, L"{%8X-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X}",
                  &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11)
        return false;
    g.Data1 = a; g.Data2 = (USHORT)b; g.Data3 = (USHORT)c;
    for (int i = 0; i < 8; i++) g.Data4[i] = (BYTE)d[i];
    return true;
}

// Locate bulk IN/OUT pipes on an interface handle. Returns true if both found.
static bool FindBulkPipes(WINUSB_INTERFACE_HANDLE iface, BYTE& pipeIn, BYTE& pipeOut) {
    USB_INTERFACE_DESCRIPTOR ifDesc = {};
    if (!WinUsb_QueryInterfaceSettings(iface, 0, &ifDesc)) return false;
    pipeIn = 0; pipeOut = 0;
    for (BYTE e = 0; e < ifDesc.bNumEndpoints; e++) {
        WINUSB_PIPE_INFORMATION pipe = {};
        if (!WinUsb_QueryPipe(iface, 0, e, &pipe)) continue;
        if (pipe.PipeType != UsbdPipeTypeBulk) continue;
        if (pipe.PipeId & 0x80) pipeIn = pipe.PipeId;
        else                    pipeOut = pipe.PipeId;
    }
    return pipeIn != 0 && pipeOut != 0;
}

// Locate an interrupt IN pipe (HID-style interface bound to WinUSB).
static bool FindInterruptInPipe(WINUSB_INTERFACE_HANDLE iface, BYTE& pipeIn) {
    USB_INTERFACE_DESCRIPTOR ifDesc = {};
    if (!WinUsb_QueryInterfaceSettings(iface, 0, &ifDesc)) return false;
    pipeIn = 0;
    for (BYTE e = 0; e < ifDesc.bNumEndpoints; e++) {
        WINUSB_PIPE_INFORMATION pipe = {};
        if (!WinUsb_QueryPipe(iface, 0, e, &pipe)) continue;
        if (pipe.PipeType == UsbdPipeTypeInterrupt && (pipe.PipeId & 0x80)) {
            pipeIn = pipe.PipeId;
            return true;
        }
    }
    return false;
}

// Try to open one device path as the vendor bulk interface.
// On success fills all g_usb* globals and returns true.
static bool TryOpenVendorPath(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        AppLog("[USB] CreateFile failed err=" + std::to_string(GetLastError()));
        return false;
    }
    WINUSB_INTERFACE_HANDLE dev = nullptr;
    if (!WinUsb_Initialize(h, &dev)) {
        AppLog("[USB] WinUsb_Initialize failed err=" + std::to_string(GetLastError()));
        CloseHandle(h);
        return false;
    }

    // Case 1: this handle IS the vendor function (MI_01 opened directly)
    BYTE in = 0, out = 0;
    if (FindBulkPipes(dev, in, out)) {
        g_usbFile = h; g_usbDev = dev; g_usbIface = dev;
        g_usbIfaceIsAssociated = false;
        g_pipeIn = in; g_pipeOut = out;
        return true;
    }

    // Case 2: whole composite device (e.g. Zadig on the parent) — walk
    // associated interfaces until we find the one with bulk pipes.
    for (UCHAR idx = 0; idx < 4; idx++) {
        WINUSB_INTERFACE_HANDLE assoc = nullptr;
        if (!WinUsb_GetAssociatedInterface(dev, idx, &assoc)) break;
        if (FindBulkPipes(assoc, in, out)) {
            g_usbFile = h; g_usbDev = dev; g_usbIface = assoc;
            g_usbIfaceIsAssociated = true;
            g_pipeIn = in; g_pipeOut = out;
            return true;
        }
        WinUsb_Free(assoc);
    }

    // Case 3: HID interface (MI_00) taken over by WinUSB/Zadig — has an
    // interrupt IN pipe. Windows HID won't see it, so input reports must be
    // read from here. Keep it as the raw-HID input candidate.
    BYTE intIn = 0;
    if (g_rawDev == nullptr && FindInterruptInPipe(dev, intIn)) {
        AppLog("[USB] Interrupt IN pipe found — keeping as raw HID input source (WinUSB driver on HID interface)");
        g_rawFile = h; g_rawDev = dev; g_rawPipeIn = intIn;
        return false; // keep searching for the vendor bulk interface
    }

    AppLog("[USB] Path opened but no usable pipes — skipping");
    WinUsb_Free(dev);
    CloseHandle(h);
    return false;
}

// Enumerate device interface paths for a given interface GUID, filtered to our
// VID/PID, and try each one.
static bool TryGuid(const GUID& ifGuid, const char* label) {
    HDEVINFO di = SetupDiGetClassDevs(&ifGuid, nullptr, nullptr,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return false;

    bool ok = false;
    SP_DEVICE_INTERFACE_DATA ifd = {}; ifd.cbSize = sizeof(ifd);
    for (DWORD j = 0; !ok && SetupDiEnumDeviceInterfaces(di, nullptr, &ifGuid, j, &ifd); j++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetail(di, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        auto* det = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(need);
        det->cbSize = sizeof(*det);
        if (SetupDiGetDeviceInterfaceDetail(di, &ifd, det, need, nullptr, nullptr)) {
            std::string p(det->DevicePath);
            std::string up = p;
            for (auto& c : up) c = (char)toupper((unsigned char)c);
            if (up.find("VID_057E") != std::string::npos &&
                up.find("PID_2069") != std::string::npos) {
                AppLog(std::string("[USB] Candidate (") + label + "): " + p.substr(0, 90));
                if (TryOpenVendorPath(det->DevicePath)) {
                    AppLog("[USB] Vendor bulk interface opened!");
                    ok = true;
                }
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(di);
    return ok;
}

// Main discovery entry point.
static bool OpenVendorInterface() {
    // A) Registry DeviceInterfaceGUID(s) of every present 057E:2069 devnode.
    //    This is how both MS OS descriptor installs and Zadig register paths.
    HDEVINFO di = SetupDiGetClassDevs(nullptr, "USB", nullptr,
                                      DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (di != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dd = {}; dd.cbSize = sizeof(dd);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
            char instId[512] = {};
            if (!SetupDiGetDeviceInstanceIdA(di, &dd, instId, sizeof(instId), nullptr)) continue;
            char upper[512]; strncpy_s(upper, instId, 511);
            for (char* p = upper; *p; p++) *p = (char)toupper((unsigned char)*p);
            if (!strstr(upper, "VID_057E") || !strstr(upper, "PID_2069")) continue;
            AppLog("[USB] Devnode: " + std::string(instId));

            HKEY key = SetupDiOpenDevRegKey(di, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (key == INVALID_HANDLE_VALUE) continue;
            WCHAR guidBuf[512] = {};
            DWORD type = 0, sz = sizeof(guidBuf);
            LSTATUS ls = RegQueryValueExW(key, L"DeviceInterfaceGUIDs", nullptr, &type, (LPBYTE)guidBuf, &sz);
            if (ls != ERROR_SUCCESS) {
                sz = sizeof(guidBuf);
                ls = RegQueryValueExW(key, L"DeviceInterfaceGUID", nullptr, &type, (LPBYTE)guidBuf, &sz);
            }
            RegCloseKey(key);
            if (ls != ERROR_SUCCESS) { AppLog("[USB]   (no DeviceInterfaceGUID)"); continue; }

            // May be REG_MULTI_SZ — walk every string in the buffer
            for (const WCHAR* g = guidBuf; *g; g += wcslen(g) + 1) {
                GUID ifGuid;
                char ga[80] = {};
                WideCharToMultiByte(CP_UTF8, 0, g, -1, ga, 80, nullptr, nullptr);
                AppLog("[USB]   DeviceInterfaceGUID = " + std::string(ga));
                if (ParseGUIDStr(g, ifGuid) && TryGuid(ifGuid, "registry")) {
                    SetupDiDestroyDeviceInfoList(di);
                    return true;
                }
                if (type != REG_MULTI_SZ) break;
            }
        }
        SetupDiDestroyDeviceInfoList(di);
    }

    // B) Generic USB device interface GUID (winusb.sys / hub-registered paths)
    static const GUID GUID_DEVINTERFACE_USB_DEVICE_LOCAL =
        {0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};
    if (TryGuid(GUID_DEVINTERFACE_USB_DEVICE_LOCAL, "usb-device")) return true;

    // C) Zadig "USBDevice" setup-class interface GUID
    static const GUID ZADIG_WINUSB_GUID =
        {0x88BAE032, 0x5A81, 0x49F0, {0xBC, 0x3D, 0xA4, 0xFF, 0x13, 0x82, 0x16, 0xD6}};
    if (TryGuid(ZADIG_WINUSB_GUID, "usbdevice-class")) return true;

    return false;
}

static void CloseVendorInterface() {
    if (g_usbIface && g_usbIfaceIsAssociated) WinUsb_Free(g_usbIface);
    g_usbIface = nullptr;
    g_usbIfaceIsAssociated = false;
    if (g_usbDev) { WinUsb_Free(g_usbDev); g_usbDev = nullptr; }
    if (g_usbFile != INVALID_HANDLE_VALUE) { CloseHandle(g_usbFile); g_usbFile = INVALID_HANDLE_VALUE; }
    g_pipeIn = g_pipeOut = 0;

    if (g_rawDev) { WinUsb_Free(g_rawDev); g_rawDev = nullptr; }
    if (g_rawFile != INVALID_HANDLE_VALUE) { CloseHandle(g_rawFile); g_rawFile = INVALID_HANDLE_VALUE; }
    g_rawPipeIn = 0;
}

// ─── procon2tool initialization sequence (built-in "step 1") ─────────────────
//
// Byte-for-byte the commands procon2tool sends over the bulk OUT endpoint when
// you click "Enable HID Output" in the browser. After command 1 the controller
// starts HID output at 4ms intervals. 0xFF blocks are console-MAC/LTK
// placeholders — the tool sends them as 0xFF and the controller accepts that.

static const std::vector<std::vector<uint8_t>> PROCON2_INIT_SEQ = {
    /* 1  Init 0x03 — starts HID output @4ms  */
    {0x03,0x91,0x00,0x0d,0x00,0x08, 0x00,0x00,0x01,0x00, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
    /* 2  Unknown 0x07                        */
    {0x07,0x91,0x00,0x01, 0x00,0x00,0x00,0x00},
    /* 3  Unknown 0x16                        */
    {0x16,0x91,0x00,0x01, 0x00,0x00,0x00,0x00},
    /* 4  Request controller MAC 0x15/0x01    */
    {0x15,0x91,0x00,0x01,0x00,0x0e, 0x00,0x00,0x00,0x02,
     0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF},
    /* 5  LTK request 0x15/0x02               */
    {0x15,0x91,0x00,0x02,0x00,0x11, 0x00,0x00,0x00,
     0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
    /* 6  Unknown 0x15/0x03                   */
    {0x15,0x91,0x00,0x03,0x00,0x01, 0x00,0x00,0x00},
    /* 7  Unknown 0x09                        */
    {0x09,0x91,0x00,0x07,0x00,0x08, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 8  IMU config 0x0C/0x02 (no ACK)       */
    {0x0c,0x91,0x00,0x02,0x00,0x04, 0x00,0x00,0x27, 0x00,0x00,0x00},
    /* 9  OUT unknown 0x11                    */
    {0x11,0x91,0x00,0x03, 0x00,0x00,0x00,0x00},
    /* 10 Unknown 0x0A                        */
    {0x0a,0x91,0x00,0x08,0x00,0x14, 0x00,0x00,0x01,
     0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff, 0x35,0x00,0x46, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 11 IMU config 0x0C/0x04                */
    {0x0c,0x91,0x00,0x04,0x00,0x04, 0x00,0x00,0x27, 0x00,0x00,0x00},
    /* 12 Enable haptics 0x03/0x0A            */
    {0x03,0x91,0x00,0x0a,0x00,0x04, 0x00,0x00,0x09, 0x00,0x00,0x00},
    /* 13 OUT unknown 0x10 (no ACK)           */
    {0x10,0x91,0x00,0x01, 0x00,0x00,0x00,0x00},
    /* 14 OUT unknown 0x01                    */
    {0x01,0x91,0x00,0x0c, 0x00,0x00,0x00,0x00},
    /* 15 OUT unknown 0x03                    */
    {0x03,0x91,0x00,0x01, 0x00,0x00,0x00},
    /* 16 OUT unknown 0x0A alt                */
    {0x0a,0x91,0x00,0x02,0x00,0x04, 0x00,0x00,0x03, 0x00,0x00},
    /* 17 Set player LED (player 1)           */
    {0x09,0x91,0x00,0x07,0x00,0x08, 0x00,0x00, 0x01, 0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

// Send the full sequence. Mirrors procon2tool: write → ~10ms → read response.
static bool SendProcon2InitSequence() {
    // Short read timeout so no-ACK commands don't stall the sequence
    ULONG timeout = 150;
    WinUsb_SetPipePolicy(g_usbIface, g_pipeIn, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);

    AppLog("[INIT] Sending procon2tool init sequence (" +
           std::to_string(PROCON2_INIT_SEQ.size()) + " commands)...");

    int sent = 0;
    for (size_t i = 0; i < PROCON2_INIT_SEQ.size(); i++) {
        const auto& cmd = PROCON2_INIT_SEQ[i];
        ULONG written = 0;
        if (!WinUsb_WritePipe(g_usbIface, g_pipeOut,
                              const_cast<uint8_t*>(cmd.data()), (ULONG)cmd.size(),
                              &written, nullptr)) {
            AppLog("[INIT] cmd " + std::to_string(i + 1) + " write FAILED err=" +
                   std::to_string(GetLastError()));
            continue;
        }
        sent++;
        Sleep(10);

        uint8_t resp[64] = {};
        ULONG got = 0;
        if (WinUsb_ReadPipe(g_usbIface, g_pipeIn, resp, sizeof(resp), &got, nullptr) && got > 0)
            AppLog("[INIT] cmd " + std::to_string(i + 1) + " -> ACK " +
                   std::to_string(got) + "B: " + HexDump(resp, got));
        else
            AppLog("[INIT] cmd " + std::to_string(i + 1) + " -> (no response)");
    }

    AppLog("[INIT] Init sequence done (" + std::to_string(sent) + "/" +
           std::to_string(PROCON2_INIT_SEQ.size()) + " sent) — controller should now stream input");
    return sent > 0;
}

// ─── HID interface ───────────────────────────────────────────────────────────

static USHORT g_hidInputReportLen  = 64;
static USHORT g_hidOutputReportLen = 64;

static HANDLE FindHidDevice(USHORT pid) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevs(&hidGuid, nullptr, nullptr,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        AppLog("[HID] SetupDiGetClassDevs failed err=" + std::to_string(GetLastError()));
        return INVALID_HANDLE_VALUE;
    }

    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifData = {}; ifData.cbSize = sizeof(ifData);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifData); i++) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, nullptr, 0, &needed, nullptr);
        if (!needed) continue;
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(needed);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, detail, needed, nullptr, nullptr)) {
            HANDLE h = CreateFile(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attrs = {}; attrs.Size = sizeof(attrs);
                if (HidD_GetAttributes(h, &attrs) &&
                    attrs.VendorID == SWITCH_VID && attrs.ProductID == pid) {
                    PHIDP_PREPARSED_DATA pp = nullptr;
                    if (HidD_GetPreparsedData(h, &pp)) {
                        HIDP_CAPS caps = {};
                        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                            g_hidInputReportLen  = caps.InputReportByteLength;
                            g_hidOutputReportLen = caps.OutputReportByteLength;
                            AppLog("[HID] Found controller (PID=0x" +
                                   [&]{ std::ostringstream o; o << std::hex << pid; return o.str(); }() +
                                   "). InputLen=" + std::to_string(caps.InputReportByteLength) +
                                   " OutputLen=" + std::to_string(caps.OutputReportByteLength));
                        }
                        HidD_FreePreparsedData(pp);
                    }
                    found = h;
                    free(detail);
                    break;
                }
                CloseHandle(h);
            }
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

// Wait up to timeoutMs for a single HID input report.
static bool WaitForHidInput(HANDLE hDev, int timeoutMs) {
    DWORD bufSz = std::max((USHORT)64, g_hidInputReportLen);
    std::vector<uint8_t> buf(bufSz);
    OVERLAPPED ov = {}; ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = ReadFile(hDev, buf.data(), bufSz, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) { CloseHandle(ov.hEvent); return false; }
    bool got = (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0);
    CancelIo(hDev);
    CloseHandle(ov.hEvent);
    return got;
}

// ─── ProCon1 (Switch 1) USB handshake — beta ─────────────────────────────────
//
// Classic, well-documented Nintendo protocol: USB commands via report 0x80,
// then subcommands via report 0x01 to enable full 0x30 reports + IMU.

static bool ProCon1WriteReport(HANDLE hDev, const uint8_t* data, size_t len) {
    std::vector<uint8_t> buf(std::max((USHORT)64, g_hidOutputReportLen), 0);
    memcpy(buf.data(), data, std::min(len, buf.size()));
    OVERLAPPED ov = {}; ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = WriteFile(hDev, buf.data(), (DWORD)buf.size(), nullptr, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        DWORD d = 0; ok = GetOverlappedResult(hDev, &ov, &d, TRUE);
    }
    CloseHandle(ov.hEvent);
    if (!ok) AppLog("[PC1] write failed err=" + std::to_string(GetLastError()));
    return ok != FALSE;
}

static void ProCon1Handshake(HANDLE hDev, bool overUsb) {
    AppLog(overUsb ? "[PC1] Sending Switch 1 Pro Controller USB handshake..."
                   : "[PC1] Sending Switch 1 Pro Controller subcommands (Bluetooth)...");
    static uint8_t timer = 0;
    auto subcmd = [&](uint8_t id, uint8_t arg) {
        // report 0x01, timer, neutral rumble x2, subcommand, argument
        uint8_t b[12] = {0x01, (uint8_t)(timer++ & 0x0F),
                         0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, id, arg};
        ProCon1WriteReport(hDev, b, sizeof(b));
        Sleep(50);
    };

    if (overUsb) {
        // 0x80-report commands exist only on the USB transport
        const uint8_t handshake[2] = {0x80, 0x02};
        const uint8_t hidOnly[2]   = {0x80, 0x04};
        ProCon1WriteReport(hDev, handshake, 2); Sleep(50);
        ProCon1WriteReport(hDev, hidOnly, 2);   Sleep(50);
    }

    subcmd(0x03, 0x30); // full input reports (0x30)
    subcmd(0x40, 0x01); // enable IMU
    subcmd(0x30, 0x01); // player 1 LED
    AppLog("[PC1] Handshake done — expecting 0x30 reports");
}

// ─── Bluetooth LE (ProCon2 wireless) ─────────────────────────────────────────
//
// Ported from the original joycon2cpp BLE implementation (git HEAD). The
// controller advertises Nintendo manufacturer data; input arrives as GATT
// notifications whose payload uses the same layout as the 0x30 report family
// (buttons at [3..8], sticks at [10..15], IMU further in) — so the existing
// decoders work unchanged.

namespace WBT = winrt::Windows::Devices::Bluetooth;
namespace WBA = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace WGA = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace WSS = winrt::Windows::Storage::Streams;

constexpr uint16_t NINTENDO_MFG_ID = 1363;
static const std::vector<uint8_t> NINTENDO_MFG_PREFIX = {0x01, 0x00, 0x03, 0x7E};
static const wchar_t* BLE_INPUT_UUID = L"ab7de9be-89fe-49ad-828f-118f09df7fd2";
static const wchar_t* BLE_WRITE_UUID = L"649d4ac9-8eb7-4e6c-af44-1ea54fe5f005";

static WBT::BluetoothLEDevice  g_bleDevice{nullptr};
static WGA::GattCharacteristic g_bleInput{nullptr};
static WGA::GattCharacteristic g_bleWrite{nullptr};
static winrt::event_token      g_bleToken{};
static std::atomic<bool>       g_bleNeutralSent{false};

// Scan for a Switch 2 controller in pairing mode and grab its GATT chars.
static bool BleConnect() {
    auto setStatus = [](const std::string& s) { g_statusMsg = s; AppLog("[BLE] " + s); };

    WBT::BluetoothLEDevice device{nullptr};
    bool found = false;
    WBA::BluetoothLEAdvertisementWatcher watcher;
    std::mutex mtx;
    std::condition_variable cv;

    watcher.Received([&](auto const&, auto const& args) {
        std::unique_lock<std::mutex> lk(mtx);
        if (found) return;
        auto mfg = args.Advertisement().ManufacturerData();
        for (uint32_t i = 0; i < mfg.Size(); i++) {
            auto sec = mfg.GetAt(i);
            if (sec.CompanyId() != NINTENDO_MFG_ID) continue;
            auto rdr = WSS::DataReader::FromBuffer(sec.Data());
            std::vector<uint8_t> d(rdr.UnconsumedBufferLength());
            rdr.ReadBytes(d);
            if (d.size() >= NINTENDO_MFG_PREFIX.size() &&
                std::equal(NINTENDO_MFG_PREFIX.begin(), NINTENDO_MFG_PREFIX.end(), d.begin())) {
                device = WBT::BluetoothLEDevice::FromBluetoothAddressAsync(args.BluetoothAddress()).get();
                if (!device) return;
                found = true;
                watcher.Stop();
                cv.notify_one();
            }
        }
    });
    watcher.ScanningMode(WBA::BluetoothLEScanningMode::Active);
    watcher.Start();
    setStatus("Scanning... hold the SYNC button on the controller");
    {
        std::unique_lock<std::mutex> lk(mtx);
        if (!cv.wait_for(lk, std::chrono::seconds(30), [&] { return found; })) {
            watcher.Stop();
            setStatus("Timed out — controller not found");
            return false;
        }
    }

    setStatus("Controller found, fetching GATT services...");
    WGA::GattDeviceServicesResult sr{nullptr};
    for (int att = 1; att <= 10; ++att) {
        sr = device.GetGattServicesAsync(WBT::BluetoothCacheMode::Uncached).get();
        if (sr.Status() == WGA::GattCommunicationStatus::Success) break;
        setStatus("Retrying GATT (" + std::to_string(att) + "/10)...");
        Sleep(500);
    }
    if (!sr || sr.Status() != WGA::GattCommunicationStatus::Success) {
        setStatus("Failed to get GATT services — remove the device in Windows Bluetooth settings and retry.");
        return false;
    }

    WGA::GattCharacteristic inputCh{nullptr}, writeCh{nullptr};
    for (auto svc : sr.Services()) {
        auto cr = svc.GetCharacteristicsAsync().get();
        if (cr.Status() != WGA::GattCommunicationStatus::Success) continue;
        for (auto ch : cr.Characteristics()) {
            if (ch.Uuid() == winrt::guid(BLE_INPUT_UUID)) inputCh = ch;
            if (ch.Uuid() == winrt::guid(BLE_WRITE_UUID)) writeCh = ch;
        }
    }
    if (!inputCh || !writeCh) {
        setStatus("Required GATT characteristics not found — re-pair the controller.");
        return false;
    }

    try {
        device.RequestPreferredConnectionParameters(
            WBT::BluetoothLEPreferredConnectionParameters::ThroughputOptimized());
    } catch (...) {}

    g_bleDevice = device;
    g_bleInput  = inputCh;
    g_bleWrite  = writeCh;
    setStatus("Bluetooth connected!");
    return true;
}

static void BleWriteCmd(const std::vector<uint8_t>& cmd) {
    WSS::DataWriter w;
    w.WriteBytes(cmd);
    g_bleWrite.WriteValueAsync(w.DetachBuffer(), WGA::GattWriteOption::WriteWithoutResponse).get();
    Sleep(50);
}

// IMU enable + player 1 LED (same commands the original BLE app sent).
static void BleInitController() {
    AppLog("[BLE] Enabling IMU + player LED...");
    BleWriteCmd({0x0c,0x91,0x01,0x02,0x00,0x04,0x00,0x00, 0xFF, 0x00,0x00,0x00}); Sleep(450);
    BleWriteCmd({0x0c,0x91,0x01,0x04,0x00,0x04,0x00,0x00, 0xFF, 0x00,0x00,0x00}); Sleep(450);
    BleWriteCmd({0x09,0x91,0x01,0x07,0x00,0x08,0x00,0x00, 0x01, 0x00,0x00,0x00,0x00,0x00,0x00,0x00});
}

static void BleDisconnect() {
    try {
        if (g_bleInput) {
            g_bleInput.ValueChanged(g_bleToken);
            g_bleInput.WriteClientCharacteristicConfigurationDescriptorAsync(
                WGA::GattClientCharacteristicConfigurationDescriptorValue::None).get();
        }
    } catch (...) {}
    try { if (g_bleDevice) g_bleDevice.Close(); } catch (...) {}
    g_bleInput  = nullptr;
    g_bleWrite  = nullptr;
    g_bleDevice = nullptr;
    g_bleToken  = {};
}

// ─── ProCon1 (Switch 1) 0x30 report decoding ─────────────────────────────────
//
// The Switch 1 layout differs from Switch 2: buttons are 3 bytes at [3..5],
// sticks are 12-bit packed at [6..8] (left) and [9..11] (right). Feeding these
// reports into the Switch 2 decoder produces garbage, hence dedicated decoders.
// Bit layout per the dekuNukem reverse-engineering docs:
//   [3]: 0x01 Y  0x02 X  0x04 B  0x08 A  0x40 R  0x80 ZR
//   [4]: 0x01 Minus 0x02 Plus 0x04 RStick 0x08 LStick 0x10 Home 0x20 Capture
//   [5]: 0x01 Down 0x02 Up 0x04 Right 0x08 Left 0x40 L 0x80 ZL

static void DecodeS1Stick(const uint8_t* d, int16_t& outX, int16_t& outY) {
    int x = d[0] | ((d[1] & 0x0F) << 8);
    int y = (d[1] >> 4) | (d[2] << 4);
    auto conv = [](int v) -> int16_t {
        float f = (v - 2048) / 2048.0f;
        if (std::abs(f) < 0.08f) return 0;             // deadzone (no per-pad calibration)
        f = std::clamp(f * 1.35f, -1.0f, 1.0f);        // reach the edges of the octagonal gate
        return (int16_t)(f * 32767.0f);
    };
    outX = conv(x);
    outY = conv(y);
}

static XUSB_REPORT DecodeProCon1Xbox(const std::vector<uint8_t>& b) {
    XUSB_REPORT r{};
    XUSB_REPORT_INIT(&r);
    if (b.size() < 12) return r;

    uint8_t b3 = b[3], b4 = b[4], b5 = b[5];
    if (b3 & 0x08) r.wButtons |= XUSB_GAMEPAD_A;
    if (b3 & 0x04) r.wButtons |= XUSB_GAMEPAD_B;
    if (b3 & 0x02) r.wButtons |= XUSB_GAMEPAD_X;
    if (b3 & 0x01) r.wButtons |= XUSB_GAMEPAD_Y;
    if (b3 & 0x40) r.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
    if (b3 & 0x80) r.bRightTrigger = 255;
    if (b4 & 0x01) r.wButtons |= XUSB_GAMEPAD_BACK;    // Minus
    if (b4 & 0x02) r.wButtons |= XUSB_GAMEPAD_START;   // Plus
    if (b4 & 0x04) r.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB;
    if (b4 & 0x08) r.wButtons |= XUSB_GAMEPAD_LEFT_THUMB;
    // Home/Capture not mapped (guide button breaks games)
    if (b5 & 0x01) r.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;
    if (b5 & 0x02) r.wButtons |= XUSB_GAMEPAD_DPAD_UP;
    if (b5 & 0x04) r.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;
    if (b5 & 0x08) r.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
    if (b5 & 0x40) r.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (b5 & 0x80) r.bLeftTrigger = 255;

    int16_t lx, ly, rx, ry;
    DecodeS1Stick(&b[6], lx, ly);
    DecodeS1Stick(&b[9], rx, ry);
    r.sThumbLX = lx; r.sThumbLY = ly;   // Switch raw Y: up = positive → matches XUSB
    r.sThumbRX = rx; r.sThumbRY = ry;
    return r;
}

static DS4_REPORT_EX DecodeProCon1DS4(const std::vector<uint8_t>& b) {
    DS4_REPORT_EX r{};
    DS4_REPORT_INIT(reinterpret_cast<PDS4_REPORT>(&r.Report));
    if (b.size() < 12) return r;

    uint8_t b3 = b[3], b4 = b[4], b5 = b[5];
    // Positional mapping (bottom→Cross, right→Circle, left→Square, top→Triangle)
    if (b3 & 0x04) r.Report.wButtons |= DS4_BUTTON_CROSS;     // B
    if (b3 & 0x08) r.Report.wButtons |= DS4_BUTTON_CIRCLE;    // A
    if (b3 & 0x01) r.Report.wButtons |= DS4_BUTTON_SQUARE;    // Y
    if (b3 & 0x02) r.Report.wButtons |= DS4_BUTTON_TRIANGLE;  // X
    if (b3 & 0x40) r.Report.wButtons |= DS4_BUTTON_SHOULDER_RIGHT;
    if (b3 & 0x80) { r.Report.bTriggerR = 255; r.Report.wButtons |= DS4_BUTTON_TRIGGER_RIGHT; }
    if (b4 & 0x01) r.Report.wButtons |= DS4_BUTTON_SHARE;     // Minus
    if (b4 & 0x02) r.Report.wButtons |= DS4_BUTTON_OPTIONS;   // Plus
    if (b4 & 0x04) r.Report.wButtons |= DS4_BUTTON_THUMB_RIGHT;
    if (b4 & 0x08) r.Report.wButtons |= DS4_BUTTON_THUMB_LEFT;
    if (b4 & 0x10) r.Report.bSpecial |= DS4_SPECIAL_BUTTON_PS;        // Home
    if (b4 & 0x20) r.Report.bSpecial |= DS4_SPECIAL_BUTTON_TOUCHPAD;  // Capture
    if (b5 & 0x40) r.Report.wButtons |= DS4_BUTTON_SHOULDER_LEFT;
    if (b5 & 0x80) { r.Report.bTriggerL = 255; r.Report.wButtons |= DS4_BUTTON_TRIGGER_LEFT; }

    bool up = b5 & 0x02, down = b5 & 0x01, left = b5 & 0x08, right = b5 & 0x04;
    uint8_t dpad = DS4_BUTTON_DPAD_NONE;
    if      (up && left)    dpad = DS4_BUTTON_DPAD_NORTHWEST;
    else if (up && right)   dpad = DS4_BUTTON_DPAD_NORTHEAST;
    else if (down && left)  dpad = DS4_BUTTON_DPAD_SOUTHWEST;
    else if (down && right) dpad = DS4_BUTTON_DPAD_SOUTHEAST;
    else if (up)            dpad = DS4_BUTTON_DPAD_NORTH;
    else if (down)          dpad = DS4_BUTTON_DPAD_SOUTH;
    else if (left)          dpad = DS4_BUTTON_DPAD_WEST;
    else if (right)         dpad = DS4_BUTTON_DPAD_EAST;
    DS4_SET_DPAD(reinterpret_cast<PDS4_REPORT>(&r.Report), (DS4_DPAD_DIRECTIONS)dpad);

    int16_t lx, ly, rx, ry;
    DecodeS1Stick(&b[6], lx, ly);
    DecodeS1Stick(&b[9], rx, ry);
    // DS4 axes: 0..255, center 128, Y grows downward → invert
    r.Report.bThumbLX = (uint8_t)(lx / 258 + 128);
    r.Report.bThumbLY = (uint8_t)(128 - ly / 258);
    r.Report.bThumbRX = (uint8_t)(rx / 258 + 128);
    r.Report.bThumbRY = (uint8_t)(128 - ry / 258);
    return r;
}

// ─── Read thread ─────────────────────────────────────────────────────────────

static void SendNeutral(PVIGEM_CLIENT vigem, PVIGEM_TARGET pad) {
    if (g_target == EmulationTarget::DualShock4) {
        DS4_REPORT_EX r{};
        DS4_REPORT_INIT(reinterpret_cast<PDS4_REPORT>(&r.Report));
        vigem_target_ds4_update_ex(vigem, pad, r);
    } else {
        XUSB_REPORT r{};
        XUSB_REPORT_INIT(&r);
        vigem_target_x360_update(vigem, pad, r);
    }
}

static void ReadLoop(HANDLE hDev, PVIGEM_CLIENT vigem, PVIGEM_TARGET pad) {
    const InputSource src = g_inputSource;
    const bool hidClass = (src == InputSource::Hid);
    AppLog(src == InputSource::Bulk   ? "[READ] Thread started — bulk mode (vendor pipe)" :
           src == InputSource::RawHid ? "[READ] Thread started — raw HID mode (interrupt pipe via WinUSB)"
                                      : "[READ] Thread started — HID mode, InputLen=" + std::to_string(g_hidInputReportLen));

    WINUSB_INTERFACE_HANDLE wuIface = (src == InputSource::Bulk) ? g_usbIface : g_rawDev;
    WINUSB_INTERFACE_HANDLE wuDev   = (src == InputSource::Bulk) ? g_usbDev   : g_rawDev;
    BYTE                    wuPipe  = (src == InputSource::Bulk) ? g_pipeIn   : g_rawPipeIn;

    const DWORD readBufSize = hidClass ? std::max((USHORT)64, g_hidInputReportLen) : 64;
    std::vector<uint8_t> buf(readBufSize, 0);
    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    int  loggedReports = 0;
    int  unknownLog    = 0;
    int  noDataCount   = 0;
    bool neutralSent   = false;

    while (g_running.load(std::memory_order_acquire)) {
        ResetEvent(ov.hEvent);
        std::fill(buf.begin(), buf.end(), 0);
        BOOL pending;
        if (hidClass) pending = !ReadFile(hDev, buf.data(), readBufSize, nullptr, &ov);
        else          pending = !WinUsb_ReadPipe(wuIface, wuPipe, buf.data(), readBufSize, nullptr, &ov);

        if (pending && GetLastError() != ERROR_IO_PENDING) {
            AppLog("[READ] read error: " + std::to_string(GetLastError()));
            break;
        }

        DWORD waitRes = WaitForSingleObject(ov.hEvent, 200);
        if (waitRes == WAIT_TIMEOUT) {
            if (hidClass) CancelIo(hDev);
            else          WinUsb_AbortPipe(wuIface, wuPipe);
            noDataCount++;
            if (noDataCount == 10)
                g_statusMsg = "No input for 2s — press a button on the controller.";
            continue;
        }
        if (waitRes != WAIT_OBJECT_0) break;

        DWORD bytesRead = 0;
        BOOL gotRes = hidClass
            ? GetOverlappedResult(hDev, &ov, &bytesRead, FALSE)
            : WinUsb_GetOverlappedResult(wuDev, &ov, &bytesRead, FALSE);
        if (!gotRes || bytesRead == 0) continue;
        noDataCount = 0;

        uint8_t reportId = buf[0];

        if (loggedReports < 3) {
            std::ostringstream oss;
            oss << "[READ] #" << ++loggedReports << " ID=0x" << std::hex << (int)reportId
                << " len=" << std::dec << bytesRead << " | " << HexDump(buf.data(), bytesRead);
            AppLog(oss.str());
        }

        bool validReport = (reportId == 0x09 || reportId == 0x30 || reportId == 0x31);
        if (!validReport) {
            if (unknownLog++ < 5)
                AppLog("[READ] Skipping report ID=0x" +
                       [&]{ std::ostringstream o; o << std::hex << (int)reportId; return o.str(); }());
            continue;
        }

        std::vector<uint8_t> report(buf.begin(), buf.begin() + bytesRead);

        // Decode first (also while paused — the mockup test view stays live),
        // then remap + forward to the virtual pad only when emulation is on.
        const bool emuOn     = g_emulationOn.load(std::memory_order_relaxed);
        const bool isProCon1 = (g_model == ControllerModel::ProCon1);
        std::lock_guard<std::mutex> padLk(g_padMutex);   // target may swap on the fly
        if (!g_pad) continue;
        const bool useDs4 = (g_target == EmulationTarget::DualShock4);
        if (useDs4) {
            DS4_REPORT_EX dr;
            if (reportId == 0x30 || reportId == 0x31) {
                report.resize(0x40, 0);
                dr = isProCon1 ? DecodeProCon1DS4(report)
                               : GenerateProControllerReport(report);
            } else {
                dr = GenerateSwitch2ProReport(report);
            }
            UpdateLiveDS4(dr);
            if (!emuOn) {
                if (!neutralSent) { SendNeutral(vigem, pad); neutralSent = true; }
                continue;
            }
            neutralSent = false;
            ApplyRemapDS4(dr);
            vigem_target_ds4_update_ex(vigem, pad, dr);
        } else {
            XUSB_REPORT xr;
            if (reportId == 0x30 || reportId == 0x31) {
                report.resize(0x40, 0);
                xr = isProCon1 ? DecodeProCon1Xbox(report)
                               : GenerateProControllerXboxReport(report);
            } else {
                xr = GenerateSwitch2ProXboxReport(report);
            }
            UpdateLiveX(xr);
            if (!emuOn) {
                if (!neutralSent) { SendNeutral(vigem, pad); neutralSent = true; }
                continue;
            }
            neutralSent = false;
            ApplyRemapX(xr);
            vigem_target_x360_update(vigem, pad, xr);
        }
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

static PVIGEM_TARGET AddVirtualPad() {
    PVIGEM_TARGET t = (g_target == EmulationTarget::DualShock4)
        ? vigem_target_ds4_alloc()
        : vigem_target_x360_alloc();
    if (!t) { AppLog("[VIGEM] target alloc failed"); return nullptr; }
    VIGEM_ERROR err = vigem_target_add(g_vigem, t);
    if (!VIGEM_SUCCESS(err)) {
        AppLog("[VIGEM] vigem_target_add() failed, code=" + std::to_string((int)err));
        vigem_target_free(t); return nullptr;
    }
    AppLog(g_target == EmulationTarget::DualShock4
        ? "[VIGEM] DualShock 4 virtual controller added"
        : "[VIGEM] Xbox 360 virtual controller added");
    return t;
}

// Swap the virtual pad type while connected (XInput <-> DualShock 4).
static void SwitchTargetLive(EmulationTarget t) {
    std::lock_guard<std::mutex> lk(g_padMutex);
    if (t == g_target && g_pad) return;
    if (g_pad && g_vigem) {
        vigem_target_remove(g_vigem, g_pad);
        vigem_target_free(g_pad);
        g_pad = nullptr;
    }
    g_target = t;
    g_pad = AddVirtualPad();
    SaveSettings();
    AppLog(t == EmulationTarget::DualShock4
        ? "[VIGEM] Switched output to DualShock 4"
        : "[VIGEM] Switched output to Xbox 360 (XInput)");
}

// ─── Connection lifecycle ────────────────────────────────────────────────────

static void DoConnect() {
    g_connectError.clear();
    g_screen = AppScreen::Connecting;
    SaveSettings();

    std::thread([]() {
        const bool pc2 = (g_model == ControllerModel::ProCon2);
        const bool bt  = (g_connMode == ConnectionMode::Bluetooth);
        AppLog(pc2 ? "[CONNECT] Looking for Switch 2 Pro Controller..."
                   : "[CONNECT] Looking for Switch 1 Pro Controller...");

        if (!InitViGEm()) {
            g_connectError = "ViGEmBus not installed or not running. Install the ViGEmBus driver.";
            g_screen = AppScreen::Setup; return;
        }

        HANDLE hDev = INVALID_HANDLE_VALUE;
        g_inputSource = InputSource::None;
        bool usbInitDone = false;

        // ── Bluetooth LE (ProCon2 wireless) ──────────────────────────────────
        if (pc2 && bt) {
            try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
            try {
                if (!BleConnect()) {
                    g_connectError = "Controller not found over Bluetooth. Hold the SYNC button "
                                     "(next to the USB port) until the LEDs sweep, then press Connect again.";
                    g_screen = AppScreen::Setup;
                    return;
                }
                BleInitController();

                g_pad = AddVirtualPad();
                if (!g_pad) {
                    g_connectError = "Failed to create the virtual controller (ViGEmBus issue).";
                    BleDisconnect();
                    g_screen = AppScreen::Setup;
                    return;
                }
                if (g_dsuEnabled) {
                    g_dsuServer.Start();
                    g_dsuServer.SetControllerConnected(0);
                }

                g_emulationOn.store(true);
                g_bleNeutralSent.store(false);
                g_running.store(true, std::memory_order_release);
                g_inputSource = InputSource::Ble;

                g_bleToken = g_bleInput.ValueChanged(
                    [](WGA::GattCharacteristic const&, WGA::GattValueChangedEventArgs const& a) {
                        if (!g_running.load(std::memory_order_acquire)) return;
                        auto rdr = WSS::DataReader::FromBuffer(a.CharacteristicValue());
                        std::vector<uint8_t> buf(rdr.UnconsumedBufferLength());
                        rdr.ReadBytes(buf);
                        if (buf.size() < 0x3C) return;

                        const bool emuOn = g_emulationOn.load(std::memory_order_relaxed);
                        std::lock_guard<std::mutex> padLk(g_padMutex);
                        if (!g_pad) return;
                        if (g_target == EmulationTarget::DualShock4) {
                            DS4_REPORT_EX dr = GenerateProControllerReport(buf);
                            UpdateLiveDS4(dr);
                            if (!emuOn) {
                                if (!g_bleNeutralSent.exchange(true)) SendNeutral(g_vigem, g_pad);
                                return;
                            }
                            g_bleNeutralSent.store(false);
                            ApplyRemapDS4(dr);
                            vigem_target_ds4_update_ex(g_vigem, g_pad, dr);
                        } else {
                            XUSB_REPORT xr = GenerateProControllerXboxReport(buf);
                            UpdateLiveX(xr);
                            if (!emuOn) {
                                if (!g_bleNeutralSent.exchange(true)) SendNeutral(g_vigem, g_pad);
                                return;
                            }
                            g_bleNeutralSent.store(false);
                            ApplyRemapX(xr);
                            vigem_target_x360_update(g_vigem, g_pad, xr);
                        }
                        if (g_dsuServer.IsRunning())
                            g_dsuServer.UpdateController(0, GenerateProControllerReport(buf, MotionProfile::SwitchEmu));
                    });
                g_bleInput.WriteClientCharacteristicConfigurationDescriptorAsync(
                    WGA::GattClientCharacteristicConfigurationDescriptorValue::Notify).get();

                g_statusMsg = "Connected via Bluetooth.";
                AppLog("[CONNECT] Controller running (Bluetooth)!");
                g_screen = AppScreen::Running;
            } catch (winrt::hresult_error const& e) {
                g_connectError = "Bluetooth error: " + winrt::to_string(e.message()) +
                                 " — remove the controller from Windows Bluetooth devices and retry.";
                g_running.store(false);
                BleDisconnect();
                g_screen = AppScreen::Setup;
            }
            return;
        }

        if (pc2) {
            // ── Built-in procon2tool step 1: open vendor interface + init ───
            if (OpenVendorInterface()) {
                usbInitDone = SendProcon2InitSequence();
            } else {
                AppLog("[CONNECT] Vendor bulk interface not found — controller may already be "
                       "initialized, or the WinUSB driver is missing on interface 1.");
            }

            // ── Read input: prefer HID class (what Windows/games see) ───────
            hDev = FindHidDevice(PROCON2_PID);
            if (hDev != INVALID_HANDLE_VALUE) {
                AppLog("[CONNECT] HID interface opened — waiting for input stream...");
                if (WaitForHidInput(hDev, usbInitDone ? 3000 : 1500)) {
                    g_inputSource = InputSource::Hid;
                    AppLog("[CONNECT] HID input is streaming!");
                } else {
                    AppLog("[CONNECT] No HID input yet.");
                }
            } else {
                AppLog("[CONNECT] HID class interface not found (WinUSB/Zadig driver on MI_00?).");
            }

            // ── 2nd choice: HID interface bound to WinUSB — interrupt pipe ──
            if (g_inputSource == InputSource::None && g_rawDev != nullptr) {
                AppLog("[CONNECT] Using raw HID input (interrupt pipe on MI_00 via WinUSB)...");
                ULONG timeout = 0; // infinite — read loop handles cancellation
                WinUsb_SetPipePolicy(g_rawDev, g_rawPipeIn, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
                g_inputSource = InputSource::RawHid;
            }

            // ── Last resort: vendor bulk pipe ────────────────────────────────
            if (g_inputSource == InputSource::None && usbInitDone) {
                AppLog("[CONNECT] Falling back to bulk-pipe input...");
                ULONG timeout = 0;
                WinUsb_SetPipePolicy(g_usbIface, g_pipeIn, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
                g_inputSource = InputSource::Bulk;
            }
        } else {
            // ── ProCon1 (Switch 1) — classic HID handshake ───────────────────
            // Over Bluetooth the controller shows up as a standard HID device
            // once paired in Windows settings; the 0x80 USB commands are skipped.
            hDev = FindHidDevice(PROCON1_PID);
            if (hDev == INVALID_HANDLE_VALUE) {
                g_connectError = bt
                    ? "Switch 1 Pro Controller not found. Pair it first in Windows Bluetooth settings "
                      "(hold the SYNC button), then press Connect."
                    : "Switch 1 Pro Controller not found. Connect it with a USB cable.";
                g_screen = AppScreen::Setup; return;
            }
            ProCon1Handshake(hDev, !bt);
            if (WaitForHidInput(hDev, 3000)) {
                g_inputSource = InputSource::Hid;
                AppLog("[CONNECT] ProCon1 input is streaming!");
            }
        }

        if (g_inputSource == InputSource::None) {
            if (hDev != INVALID_HANDLE_VALUE) CloseHandle(hDev);
            CloseVendorInterface();
            g_connectError = pc2
                ? "Controller found but no input stream. Unplug the USB cable, plug it back in "
                  "and press Connect again. If it keeps failing, use the procon2tool button below "
                  "once, then reconnect."
                : "ProCon1 handshake failed — no input stream. Unplug/replug the cable and retry.";
            g_screen = AppScreen::Setup;
            return;
        }

        g_pad = AddVirtualPad();
        if (!g_pad) {
            g_connectError = "Failed to create the virtual controller (ViGEmBus issue).";
            if (hDev != INVALID_HANDLE_VALUE) CloseHandle(hDev);
            CloseVendorInterface();
            g_screen = AppScreen::Setup; return;
        }

        if (g_dsuEnabled) {
            g_dsuServer.Start();
            g_dsuServer.SetControllerConnected(0);
        }

        g_hidDevice = hDev;
        switch (g_inputSource) {
            case InputSource::Hid:
                g_statusMsg = usbInitDone || !pc2
                    ? "Connected — controller activated automatically."
                    : "Connected — controller was already active.";
                break;
            case InputSource::RawHid:
                g_statusMsg = "Connected — activated automatically (WinUSB input path).";
                break;
            default:
                g_statusMsg = "Connected — bulk mode (vendor pipe).";
                break;
        }
        g_emulationOn.store(true);
        g_running.store(true, std::memory_order_release);
        g_readThread = std::thread(ReadLoop, hDev, g_vigem, g_pad);

        AppLog("[CONNECT] Controller running!");
        g_screen = AppScreen::Running;
    }).detach();
}

static void DoDisconnect() {
    g_running.store(false, std::memory_order_release);
    if (g_inputSource == InputSource::Ble)
        BleDisconnect();
    if (g_inputSource == InputSource::Bulk && g_usbIface)
        WinUsb_AbortPipe(g_usbIface, g_pipeIn);
    if (g_inputSource == InputSource::RawHid && g_rawDev)
        WinUsb_AbortPipe(g_rawDev, g_rawPipeIn);
    if (g_hidDevice != INVALID_HANDLE_VALUE)
        CancelIoEx(g_hidDevice, nullptr);
    if (g_readThread.joinable()) g_readThread.join();

    CloseVendorInterface();

    if (g_hidDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hidDevice);
        g_hidDevice = INVALID_HANDLE_VALUE;
    }
    {
        std::lock_guard<std::mutex> lk(g_padMutex);
        if (g_pad && g_vigem) {
            vigem_target_remove(g_vigem, g_pad);
            vigem_target_free(g_pad);
            g_pad = nullptr;
        }
    }
    g_dsuServer.Stop();
    g_inputSource = InputSource::None;
    ClearLive();
    SaveSettings();
    AppLog("[CONNECT] Disconnected");
    g_screen = AppScreen::Setup;
}

// ─── D3D11 ───────────────────────────────────────────────────────────────────

static ID3D11Device*           g_pd3dDevice        = nullptr;
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
static void CleanupRTV() { if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; } }

static bool CreateDX11(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60; sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext)))
        return false;
    CreateRTV(); return true;
}
static void CleanupDX11() {
    CleanupRTV();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
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

// ─── GUI helpers ─────────────────────────────────────────────────────────────

namespace ui {

static const ImVec4 ACCENT      = {0.26f, 0.59f, 1.00f, 1.f};
static const ImVec4 ACCENT_DIM  = {0.20f, 0.42f, 0.72f, 1.f};
static const ImVec4 GREEN       = {0.24f, 0.78f, 0.42f, 1.f};
static const ImVec4 RED         = {0.90f, 0.32f, 0.32f, 1.f};
static const ImVec4 ORANGE      = {0.95f, 0.65f, 0.20f, 1.f};
static const ImVec4 TEXT_MAIN   = {0.92f, 0.93f, 0.95f, 1.f};
static const ImVec4 TEXT_DIM    = {0.58f, 0.60f, 0.66f, 1.f};
static const ImVec4 CARD_BG     = {0.115f, 0.125f, 0.155f, 1.f};
static const ImVec4 CARD_BG_SEL = {0.135f, 0.185f, 0.280f, 1.f};
static const ImVec4 BTN_FILL    = {0.165f, 0.180f, 0.225f, 1.f};
static const ImVec4 BTN_HOVER   = {0.240f, 0.270f, 0.350f, 1.f};

// Width of the centered content column (set by BeginColumn each frame).
static float g_colW = 0.f;

// All screen content lives inside one centered, zero-padding child window —
// this is what keeps every element aligned to the same column.
static void BeginColumn(const char* id, float maxW) {
    ImGuiIO& io = ImGui::GetIO();
    g_colW = std::min(maxW, io.DisplaySize.x - 32.f);
    ImGui::SetCursorPosX((io.DisplaySize.x - g_colW) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild(id, {g_colW, 0}, false);
    ImGui::PopStyleVar();
}
static void EndColumn() { ImGui::EndChild(); }

static void CenterNext(float w) { ImGui::SetCursorPosX((g_colW - w) * 0.5f); }

static const ImVec4 LOGO_RED = {0.93f, 0.23f, 0.23f, 1.f};

// "connectyour" in white, "pro" in red.
static void Logo(bool centered) {
    if (g_fontBig) ImGui::PushFont(g_fontBig);
    if (centered)
        CenterNext(ImGui::CalcTextSize("connectyour").x + ImGui::CalcTextSize("pro").x);
    ImGui::TextColored(TEXT_MAIN, "connectyour");
    ImGui::SameLine(0, 0);
    ImGui::TextColored(LOGO_RED, "pro");
    if (g_fontBig) ImGui::PopFont();
}

static void Title(const char* subtitle) {
    ImGui::Spacing();
    Logo(true);
    if (subtitle && *subtitle) {
        CenterNext(ImGui::CalcTextSize(subtitle).x);
        ImGui::TextColored(TEXT_DIM, "%s", subtitle);
    }
    ImGui::Spacing();
}

// Selectable card: title + optional subtitle. Returns true when clicked.
static bool Card(const char* id, const char* title, const char* subtitle,
                 bool selected, float w, float h) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? CARD_BG_SEL : CARD_BG);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.f);
    ImGui::BeginChild(id, {w, h}, false, ImGuiWindowFlags_NoScrollbar);

    bool hasSub = subtitle && *subtitle;
    float titleH = ImGui::GetTextLineHeight();
    float blockH = hasSub ? titleH * 2 + 4.f : titleH;
    float y = (h - blockH) * 0.5f;

    ImGui::SetCursorPos({0, y});
    float tw = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX((w - tw) * 0.5f);
    ImGui::TextColored(selected ? ACCENT : TEXT_MAIN, "%s", title);
    if (hasSub) {
        float sw = ImGui::CalcTextSize(subtitle).x;
        ImGui::SetCursorPosX((w - sw) * 0.5f);
        ImGui::TextColored(TEXT_DIM, "%s", subtitle);
    }

    ImGui::SetCursorPos({0, 0});
    bool clicked = ImGui::InvisibleButton((std::string(id) + "_btn").c_str(), {w, h});
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    if (selected) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        dl->AddRect(mn, mx, ImGui::GetColorU32(ACCENT), 10.f, 0, 2.f);
    }
    return clicked;
}

// iOS-style toggle switch.
static bool Toggle(const char* id, bool* v) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = ImGui::GetFrameHeight() * 1.1f;
    float w = h * 1.9f;
    bool clicked = ImGui::InvisibleButton(id, {w, h});
    if (clicked) *v = !*v;
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImU32 bg = ImGui::GetColorU32(*v ? GREEN : ImVec4{0.32f, 0.34f, 0.40f, 1.f});
    dl->AddRectFilled(p, {p.x + w, p.y + h}, bg, h * 0.5f);
    float knobX = *v ? (p.x + w - h * 0.5f) : (p.x + h * 0.5f);
    dl->AddCircleFilled({knobX, p.y + h * 0.5f}, h * 0.5f - 3.f, IM_COL32(255, 255, 255, 255));
    return clicked;
}

static void StatusDot(bool on) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = ImGui::GetTextLineHeight() * 0.32f;
    dl->AddCircleFilled({p.x + r, p.y + ImGui::GetTextLineHeight() * 0.55f}, r,
                        ImGui::GetColorU32(on ? GREEN : RED));
    ImGui::Dummy({r * 2.5f, ImGui::GetTextLineHeight()});
    ImGui::SameLine();
}

} // namespace ui

static void DrawLog(bool defaultOpen) {
    if (ImGui::CollapsingHeader("Log", defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        ImGui::BeginChild("##log", {-1, 160}, true);
        std::lock_guard<std::mutex> lk(g_logMutex);
        for (auto& line : g_logLines)
            ImGui::TextUnformatted(line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
    }
}

// ─── Controller mockup (remapping) ───────────────────────────────────────────
//
// Schematic Pro Controller drawn with ImDrawList. Each remappable button is a
// clickable shape showing WHAT IT CURRENTLY OUTPUTS; clicking opens a popup to
// pick a different output. D-pad / sticks / triggers are decorative.

static void DrawPadMockup() {
    const float W = std::min(560.f, ui::g_colW);
    const float H = 260.f;
    ui::CenterNext(W);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##padmock", {W, H}, false, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    const float sx = W / 560.f;   // scale for narrow windows
    auto P = [&](float x, float y) { return ImVec2{o.x + x * sx, o.y + y}; };

    static int pendingBtn = -1;
    const uint32_t liveBtns = g_live.buttons.load(std::memory_order_relaxed);

    // Body
    dl->AddRectFilled(P(30, 48), P(530, 238), ImGui::GetColorU32(ui::CARD_BG), 60.f);

    // Live d-pad (left-bottom): arms light up when pressed
    ImU32 dim   = ImGui::GetColorU32(ImVec4{0.22f, 0.24f, 0.30f, 1.f});
    ImU32 press = ImGui::GetColorU32(ui::GREEN);
    dl->AddRectFilled(P(203, 140), P(227, 172), (liveBtns & LIVE_DPAD_UP)    ? press : dim, 5.f);
    dl->AddRectFilled(P(203, 172), P(227, 204), (liveBtns & LIVE_DPAD_DOWN)  ? press : dim, 5.f);
    dl->AddRectFilled(P(183, 160), P(207, 184), (liveBtns & LIVE_DPAD_LEFT)  ? press : dim, 5.f);
    dl->AddRectFilled(P(223, 160), P(247, 184), (liveBtns & LIVE_DPAD_RIGHT) ? press : dim, 5.f);
    dl->AddRectFilled(P(203, 160), P(227, 184), dim, 0.f);   // center cap

    // Live ZL/ZR trigger bars (top corners)
    auto triggerBar = [&](float x0, float x1, int val, bool alignRight) {
        ImVec2 mn = P(x0, 2), mx = P(x1, 9);
        dl->AddRectFilled(mn, mx, dim, 3.f);
        float frac = std::clamp(val / 255.f, 0.f, 1.f);
        if (frac > 0.01f) {
            float w = (mx.x - mn.x) * frac;
            ImVec2 fmn = alignRight ? ImVec2{mx.x - w, mn.y} : mn;
            ImVec2 fmx = alignRight ? mx : ImVec2{mn.x + w, mx.y};
            dl->AddRectFilled(fmn, fmx, press, 3.f);
        }
    };
    triggerBar(80, 200, g_live.lt.load(std::memory_order_relaxed), false);
    triggerBar(360, 480, g_live.rt.load(std::memory_order_relaxed), true);

    // Clickable button helper (highlights green while physically pressed)
    auto padButton = [&](int lb, ImVec2 mnRel, ImVec2 mxRel, bool circle) {
        ImVec2 mn = P(mnRel.x, mnRel.y), mx = P(mxRel.x, mxRel.y);
        ImGui::SetCursorScreenPos(mn);
        std::string bid = "##pb" + std::to_string(lb);
        bool clicked = ImGui::InvisibleButton(bid.c_str(), {mx.x - mn.x, mx.y - mn.y});
        bool hov = ImGui::IsItemHovered();
        bool remapped = (g_remap[lb] != lb);
        bool pressed  = (liveBtns & (1u << lb)) != 0;
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        ImU32 fill   = pressed ? press
                               : ImGui::GetColorU32(hov ? ui::BTN_HOVER : ui::BTN_FILL);
        ImU32 border = ImGui::GetColorU32(pressed ? ImVec4{1.f, 1.f, 1.f, 0.9f}
                                : remapped ? ui::ACCENT : ImVec4{0.36f, 0.38f, 0.46f, 1.f});
        float bw = (remapped || pressed) ? 2.5f : 1.5f;
        ImVec2 c = {(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f};
        float r = (mx.x - mn.x) * 0.5f;
        if (circle) {
            dl->AddCircleFilled(c, r, fill);
            dl->AddCircle(c, r, border, 0, bw);
        } else {
            dl->AddRectFilled(mn, mx, fill, 9.f);
            dl->AddRect(mn, mx, border, 9.f, 0, bw);
        }

        // Live stick position: small knob that follows the axis
        if (lb == LB_LS || lb == LB_RS) {
            float ax = (lb == LB_LS ? g_live.lx : g_live.rx).load(std::memory_order_relaxed) / 32767.f;
            float ay = (lb == LB_LS ? g_live.ly : g_live.ry).load(std::memory_order_relaxed) / 32767.f;
            bool moved = std::abs(ax) > 0.05f || std::abs(ay) > 0.05f;
            ImVec2 knob = {c.x + ax * r * 0.45f, c.y - ay * r * 0.45f};   // screen Y down
            dl->AddCircleFilled(knob, r * 0.30f,
                ImGui::GetColorU32(moved ? ui::GREEN : ImVec4{0.30f, 0.33f, 0.42f, 1.f}));
        }

        const char* out = SHORT_NAMES[g_remap[lb]];
        ImVec2 ts = ImGui::CalcTextSize(out);
        dl->AddText({c.x - ts.x * 0.5f, c.y - ts.y * 0.5f},
                    ImGui::GetColorU32(pressed ? ImVec4{0.05f, 0.10f, 0.06f, 1.f}
                                     : remapped ? ui::ACCENT : ui::TEXT_MAIN), out);

        if (hov) ImGui::SetTooltip("%s  outputs as  %s\nClick to change",
                                   LOGICAL_NAMES[lb], LOGICAL_NAMES[g_remap[lb]]);
        if (clicked) { pendingBtn = lb; ImGui::OpenPopup("##remapPopup"); }
    };

    // Shoulders
    padButton(LB_LB, {80, 12},  {200, 40},  false);
    padButton(LB_RB, {360, 12}, {480, 40},  false);
    // Minus / Plus
    padButton(LB_MINUS, {233, 74}, {259, 100}, true);
    padButton(LB_PLUS,  {301, 74}, {327, 100}, true);
    // Left stick (click = L3)
    padButton(LB_LS, {100, 80}, {166, 146}, true);
    // Right stick (click = R3)
    padButton(LB_RS, {312, 142}, {370, 200}, true);
    // ABXY diamond (Nintendo: X top, A right, B bottom, Y left)
    padButton(LB_X, {413, 62},  {447, 96},  true);
    padButton(LB_A, {445, 94},  {479, 128}, true);
    padButton(LB_B, {413, 126}, {447, 160}, true);
    padButton(LB_Y, {381, 94},  {415, 128}, true);

    // Remap popup
    if (ImGui::BeginPopup("##remapPopup")) {
        if (pendingBtn >= 0 && pendingBtn < LB_COUNT) {
            ImGui::TextColored(ui::TEXT_DIM, "%s outputs as:", LOGICAL_NAMES[pendingBtn]);
            ImGui::Separator();
            for (int i = 0; i < LB_COUNT; i++) {
                bool sel = (g_remap[pendingBtn] == i);
                if (ImGui::Selectable(LOGICAL_NAMES[i], sel)) {
                    g_remap[pendingBtn] = i;
                    SaveSettings();
                }
            }
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
}

// ─── Screens ─────────────────────────────────────────────────────────────────

static void DrawSetupScreen() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##setup", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ui::BeginColumn("##col_setup", 620.f);
    ui::Title("Step 1 of 2 — pick your controller and output");

    const float cardW = (ui::g_colW - 12.f) * 0.5f;
    const float cardH = 64.f;

    ImGui::TextColored(ui::TEXT_DIM, "CONTROLLER");
    if (ui::Card("##pc2", "Pro Controller 2", "Switch 2",
                 g_model == ControllerModel::ProCon2, cardW, cardH))
        { g_model = ControllerModel::ProCon2; SaveSettings(); }
    ImGui::SameLine(0, 12);
    if (ui::Card("##pc1", "Pro Controller 1", "Switch 1",
                 g_model == ControllerModel::ProCon1, cardW, cardH))
        { g_model = ControllerModel::ProCon1; SaveSettings(); }

    ImGui::Spacing();

    ImGui::TextColored(ui::TEXT_DIM, "CONNECTION");
    if (ui::Card("##usb", "USB", "Cable",
                 g_connMode == ConnectionMode::Usb, cardW, cardH))
        { g_connMode = ConnectionMode::Usb; SaveSettings(); }
    ImGui::SameLine(0, 12);
    const bool btExperimental = (g_model == ControllerModel::ProCon2);
    if (ui::Card("##bt", "Bluetooth", btExperimental ? "Wireless — EXPERIMENTAL" : "Wireless",
                 g_connMode == ConnectionMode::Bluetooth, cardW, cardH))
        { g_connMode = ConnectionMode::Bluetooth; SaveSettings(); }

    if (btExperimental && g_connMode == ConnectionMode::Bluetooth) {
        ImGui::PushTextWrapPos(ui::g_colW);
        ImGui::TextColored(ui::ORANGE,
            "Experimental: Pro Controller 2 over Bluetooth has noticeably higher input lag "
            "than USB. Use a cable for fast-paced games.");
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();

    ImGui::TextColored(ui::TEXT_DIM, "SHOW UP IN GAMES AS");
    if (ui::Card("##x360", "Xbox 360", "XInput",
                 g_target == EmulationTarget::Xbox360, cardW, cardH))
        { g_target = EmulationTarget::Xbox360; SaveSettings(); }
    ImGui::SameLine(0, 12);
    if (ui::Card("##ds4", "DualShock 4", "",
                 g_target == EmulationTarget::DualShock4, cardW, cardH))
        { g_target = EmulationTarget::DualShock4; SaveSettings(); }

    ImGui::Spacing();

    if (!g_connectError.empty()) {
        ImGui::PushTextWrapPos(ui::g_colW);
        ImGui::TextColored(ui::RED, "%s", g_connectError.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Open procon2tool in browser (fallback)")) {
            ShellExecuteA(nullptr, "open", "https://handheldlegend.github.io/procon2tool/",
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::Spacing();
    }

    ImGui::Spacing();

    const float btnW = 240.f;
    ui::CenterNext(btnW);
    ImGui::PushStyleColor(ImGuiCol_Button,        ui::ACCENT_DIM);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ACCENT);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ui::ACCENT_DIM);
    if (ImGui::Button("Connect", {btnW, 44})) DoConnect();
    ImGui::PopStyleColor(3);
    ImGui::Spacing();

    // Log only when something went wrong — keep the first screen clean
    if (!g_connectError.empty())
        DrawLog(true);
    ui::EndColumn();
    ImGui::End();
}

static void DrawConnectingScreen() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##connecting", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    ui::BeginColumn("##col_conn", 620.f);
    ui::Title("Step 2 of 2 — connecting");

    static float spin = 0.f;
    spin += ImGui::GetIO().DeltaTime * 3.f;
    const char* spinners[] = {"|", "/", "-", "\\"};
    std::string msg = std::string("[") + spinners[(int)(spin) % 4] + "]  " +
        (g_connMode == ConnectionMode::Bluetooth
            ? g_statusMsg
            : (g_model == ControllerModel::ProCon2
                ? "Activating controller (built-in procon2tool init)..."
                : "Sending Switch 1 Pro Controller handshake..."));
    ui::CenterNext(ImGui::CalcTextSize(msg.c_str()).x);
    ImGui::TextUnformatted(msg.c_str());
    if (g_connMode == ConnectionMode::Bluetooth) {
        const char* hint = "Hold the SYNC button (next to the USB port) until the LEDs sweep.";
        ui::CenterNext(ImGui::CalcTextSize(hint).x);
        ImGui::TextColored(ui::TEXT_DIM, "%s", hint);
    }
    ImGui::Spacing();

    DrawLog(true);
    ui::EndColumn();
    ImGui::End();
}

static void DrawRunningScreen() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##running", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    ui::BeginColumn("##col_run", 620.f);

    // Header row
    ImGui::Spacing();
    ui::Logo(false);
    ImGui::SameLine(ui::g_colW - 116.f);
    ImGui::PushStyleColor(ImGuiCol_Button,        {0.55f, 0.20f, 0.20f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::RED);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.45f, 0.15f, 0.15f, 1.f});
    bool disconnect = ImGui::Button("Disconnect", {116, 0});
    ImGui::PopStyleColor(3);
    if (disconnect) { DoDisconnect(); ui::EndColumn(); ImGui::End(); return; }
    ImGui::Spacing();

    // Status card
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::CARD_BG);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.f);
    ImGui::BeginChild("##status", {ui::g_colW, 118}, false, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos({16, 12});
    bool on = g_emulationOn.load();
    ui::StatusDot(on);
    ImGui::TextUnformatted(on ? "Emulation active" : "Emulation paused");
    ImGui::SetCursorPosX(16);
    ImGui::TextColored(ui::TEXT_DIM, "%s", g_statusMsg.c_str());
    ImGui::SetCursorPosX(16);
    ImGui::TextColored(ui::TEXT_DIM, "%s (%s)%s",
        g_model  == ControllerModel::ProCon2 ? "Pro Controller 2" : "Pro Controller 1",
        g_connMode == ConnectionMode::Bluetooth ? "Bluetooth" : "USB",
        g_dsuServer.IsRunning() ? "   |   DSU on :26760" : "");

    // Emulation on/off
    ImGui::SetCursorPos({ui::g_colW - 82.f, 16.f});
    bool tog = on;
    if (ui::Toggle("##emutoggle", &tog)) g_emulationOn.store(tog);

    // Output type — switching allowed only while emulation is paused
    {
        bool isX = (g_target == EmulationTarget::Xbox360);
        float bx = ui::g_colW - 16.f - 86.f - 62.f - 6.f;
        ImGui::SetCursorPos({bx, 76.f});
        ImGui::BeginDisabled(on);
        ImGui::PushStyleColor(ImGuiCol_Button, isX ? ui::ACCENT_DIM : ImVec4{0.165f, 0.180f, 0.225f, 1.f});
        if (ImGui::Button("XInput", {86, 0}) && !isX) SwitchTargetLive(EmulationTarget::Xbox360);
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 6);
        ImGui::PushStyleColor(ImGuiCol_Button, !isX ? ui::ACCENT_DIM : ImVec4{0.165f, 0.180f, 0.225f, 1.f});
        if (ImGui::Button("DS4", {62, 0}) && isX) SwitchTargetLive(EmulationTarget::DualShock4);
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (on && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Pause emulation (toggle above) to switch the output type");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // DSU on/off — works on the fly
    {
        bool dsu = g_dsuEnabled;
        if (ImGui::Checkbox("DSU server (gyro for emulators)", &dsu)) {
            g_dsuEnabled = dsu;
            SaveSettings();
            if (dsu) { g_dsuServer.Start(); g_dsuServer.SetControllerConnected(0); }
            else       g_dsuServer.Stop();
        }
    }
    ImGui::Spacing();

    // Controller mockup — always visible (remap + live test view)
    ImGui::TextColored(ui::TEXT_DIM, "BUTTONS — click to remap, press on the pad to test");
    ImGui::Spacing();
    DrawPadMockup();
    ImGui::Spacing();
    {
        float bw1 = 90.f, bw2 = 270.f;
        ui::CenterNext(bw1 + 8.f + bw2);
        if (ImGui::Button("Default", {bw1, 0})) { RemapReset(); SaveSettings(); }
        ImGui::SameLine(0, 8);
        if (ImGui::Button("Swap A<->B, X<->Y (Nintendo layout)", {bw2, 0})) { RemapNintendoAB(); SaveSettings(); }
    }
    ImGui::Spacing();

    DrawLog(false);
    ui::EndColumn();
    ImGui::End();
}

// ─── WinMain ─────────────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    AllocConsole();
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    SetConsoleTitleW(L"connectyourpro — debug log");
    printf("[INIT] connectyourpro (built-in procon2tool init)\n");

    LoadCalibrationProfiles("calibration.json");
    LoadSettings();

    WNDCLASSEXW wc{sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L, hInst,
                   nullptr, nullptr, nullptr, nullptr, L"connectyourpro", nullptr};
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowW(L"connectyourpro", L"connectyourpro",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           720, 780, nullptr, nullptr, hInst, nullptr);

    if (!CreateDX11(g_hwnd)) {
        DestroyWindow(g_hwnd); UnregisterClassW(wc.lpszClassName, hInst); return 1;
    }
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Modern look: Segoe UI + dark palette + generous rounding
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 19.0f))
        io.Fonts->AddFontDefault();
    g_fontBig = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 26.0f);
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding    = 0.f;
    style.ChildRounding     = 10.f;
    style.FrameRounding     = 6.f;
    style.GrabRounding      = 6.f;
    style.PopupRounding     = 8.f;
    style.FramePadding      = {10.f, 6.f};
    style.ItemSpacing       = {8.f, 8.f};
    style.WindowBorderSize  = 0.f;
    style.ChildBorderSize   = 0.f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]        = {0.070f, 0.075f, 0.095f, 1.f};
    c[ImGuiCol_ChildBg]         = {0.070f, 0.075f, 0.095f, 0.f};
    c[ImGuiCol_PopupBg]         = {0.100f, 0.108f, 0.135f, 1.f};
    c[ImGuiCol_FrameBg]         = {0.135f, 0.145f, 0.180f, 1.f};
    c[ImGuiCol_FrameBgHovered]  = {0.180f, 0.195f, 0.240f, 1.f};
    c[ImGuiCol_FrameBgActive]   = {0.200f, 0.220f, 0.270f, 1.f};
    c[ImGuiCol_Header]          = {0.135f, 0.145f, 0.180f, 1.f};
    c[ImGuiCol_HeaderHovered]   = {0.180f, 0.195f, 0.240f, 1.f};
    c[ImGuiCol_HeaderActive]    = {0.200f, 0.220f, 0.270f, 1.f};
    c[ImGuiCol_Button]          = {0.165f, 0.180f, 0.225f, 1.f};
    c[ImGuiCol_ButtonHovered]   = {0.220f, 0.240f, 0.300f, 1.f};
    c[ImGuiCol_ButtonActive]    = {0.190f, 0.210f, 0.265f, 1.f};
    c[ImGuiCol_CheckMark]       = ui::ACCENT;
    c[ImGuiCol_SliderGrab]      = ui::ACCENT_DIM;
    c[ImGuiCol_SliderGrabActive]= ui::ACCENT;

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
        const float cc[4] = {0.055f, 0.06f, 0.075f, 1.f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRTV, cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    DoDisconnect();
    SaveSettings();
    if (g_vigem) { vigem_disconnect(g_vigem); vigem_free(g_vigem); g_vigem = nullptr; }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDX11();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
