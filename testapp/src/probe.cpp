// probe.cpp — Switch 2 Pro Controller USB diagnostic tool
// Enumerates all HID interfaces, dumps report IDs, tries all write methods

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <setupapi.h>

#include <mmsystem.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

constexpr USHORT SWITCH_VID   = 0x057E;
constexpr USHORT PRO_CTRL_PID = 0x2069;

static void log(const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    vprintf(fmt, a); va_end(a);
    printf("\n"); fflush(stdout);
}

// ─── Enumerate all matching HID interfaces ────────────────────────────────────

struct Iface {
    HANDLE hDev      = INVALID_HANDLE_VALUE;
    HANDLE hDevRO    = INVALID_HANDLE_VALUE; // read-only handle
    char   path[512] = {};
    USHORT inLen=0, outLen=0, featLen=0;
    USHORT usagePage=0, usage=0;
    USHORT numInBtnCaps=0, numOutBtnCaps=0;
    USHORT numInValCaps=0, numOutValCaps=0;
    std::vector<USHORT> outputReportIDs;
    std::vector<USHORT> inputReportIDs;
};

static std::vector<Iface> FindAll() {
    std::vector<Iface> result;
    GUID g; HidD_GetHidGuid(&g);

    HDEVINFO di = SetupDiGetClassDevs(&g, nullptr, nullptr, DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) { log("[!] SetupDiGetClassDevs failed %lu", GetLastError()); return result; }

    SP_DEVICE_INTERFACE_DATA id = {}; id.cbSize = sizeof(id);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, nullptr, &g, i, &id); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetail(di, &id, nullptr, 0, &need, nullptr);
        if (!need) continue;
        auto* det = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(need);
        det->cbSize = sizeof(*det);
        if (!SetupDiGetDeviceInterfaceDetail(di, &id, det, need, nullptr, nullptr)) { free(det); continue; }

        // Open read-write (for output commands)
        HANDLE hrw = CreateFile(det->DevicePath, GENERIC_READ|GENERIC_WRITE,
            FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        // Open read-only (as fallback for reading)
        HANDLE hro = CreateFile(det->DevicePath, GENERIC_READ,
            FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);

        HANDLE hCheck = (hrw != INVALID_HANDLE_VALUE) ? hrw : hro;
        if (hCheck != INVALID_HANDLE_VALUE) {
            HIDD_ATTRIBUTES attrs = {}; attrs.Size = sizeof(attrs);
            if (HidD_GetAttributes(hCheck, &attrs) &&
                attrs.VendorID == SWITCH_VID && attrs.ProductID == PRO_CTRL_PID) {

                Iface iface;
                iface.hDev   = hrw;
                iface.hDevRO = hro;
                strncpy_s(iface.path, det->DevicePath, 511);

                PHIDP_PREPARSED_DATA pp = nullptr;
                if (HidD_GetPreparsedData(hCheck, &pp)) {
                    HIDP_CAPS caps = {};
                    if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                        iface.inLen       = caps.InputReportByteLength;
                        iface.outLen      = caps.OutputReportByteLength;
                        iface.featLen     = caps.FeatureReportByteLength;
                        iface.usagePage   = caps.UsagePage;
                        iface.usage       = caps.Usage;
                        iface.numInBtnCaps  = caps.NumberInputButtonCaps;
                        iface.numOutBtnCaps = caps.NumberOutputButtonCaps;
                        iface.numInValCaps  = caps.NumberInputValueCaps;
                        iface.numOutValCaps = caps.NumberOutputValueCaps;

                        // Enumerate input report IDs via button caps
                        if (caps.NumberInputButtonCaps > 0) {
                            std::vector<HIDP_BUTTON_CAPS> bc(caps.NumberInputButtonCaps);
                            USHORT n = caps.NumberInputButtonCaps;
                            HidP_GetButtonCaps(HidP_Input, bc.data(), &n, pp);
                            for (auto& c : bc) iface.inputReportIDs.push_back(c.ReportID);
                        }
                        if (caps.NumberInputValueCaps > 0) {
                            std::vector<HIDP_VALUE_CAPS> vc(caps.NumberInputValueCaps);
                            USHORT n = caps.NumberInputValueCaps;
                            HidP_GetValueCaps(HidP_Input, vc.data(), &n, pp);
                            for (auto& c : vc) iface.inputReportIDs.push_back(c.ReportID);
                        }
                        // Enumerate output report IDs
                        if (caps.NumberOutputButtonCaps > 0) {
                            std::vector<HIDP_BUTTON_CAPS> bc(caps.NumberOutputButtonCaps);
                            USHORT n = caps.NumberOutputButtonCaps;
                            HidP_GetButtonCaps(HidP_Output, bc.data(), &n, pp);
                            for (auto& c : bc) iface.outputReportIDs.push_back(c.ReportID);
                        }
                        if (caps.NumberOutputValueCaps > 0) {
                            std::vector<HIDP_VALUE_CAPS> vc(caps.NumberOutputValueCaps);
                            USHORT n = caps.NumberOutputValueCaps;
                            HidP_GetValueCaps(HidP_Output, vc.data(), &n, pp);
                            for (auto& c : vc) iface.outputReportIDs.push_back(c.ReportID);
                        }
                        // Deduplicate
                        std::sort(iface.inputReportIDs.begin(), iface.inputReportIDs.end());
                        iface.inputReportIDs.erase(std::unique(iface.inputReportIDs.begin(), iface.inputReportIDs.end()), iface.inputReportIDs.end());
                        std::sort(iface.outputReportIDs.begin(), iface.outputReportIDs.end());
                        iface.outputReportIDs.erase(std::unique(iface.outputReportIDs.begin(), iface.outputReportIDs.end()), iface.outputReportIDs.end());
                    }
                    HidD_FreePreparsedData(pp);
                }
                result.push_back(std::move(iface));
            } else {
                if (hrw != INVALID_HANDLE_VALUE) CloseHandle(hrw);
                if (hro != INVALID_HANDLE_VALUE) CloseHandle(hro);
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(di);
    return result;
}

// ─── Read helper ──────────────────────────────────────────────────────────────

static void ReadMs(HANDLE hDev, USHORT inLen, int ms, const char* phase) {
    if (hDev == INVALID_HANDLE_VALUE) { log("[READ:%s] invalid handle", phase); return; }
    log("[READ:%s] listening %dms (inLen=%u)...", phase, ms, inLen);

    DWORD bufSz = std::max((USHORT)64, inLen);
    std::vector<uint8_t> buf(bufSz);
    OVERLAPPED ov = {}; ov.hEvent = CreateEvent(nullptr,TRUE,FALSE,nullptr);

    std::unordered_map<int,int> seen;
    int total = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);

    while (std::chrono::steady_clock::now() < deadline) {
        ResetEvent(ov.hEvent);
        fill(buf.begin(), buf.end(), 0);
        ReadFile(hDev, buf.data(), bufSz, nullptr, &ov);
        DWORD err = GetLastError();
        if (err != 0 && err != ERROR_IO_PENDING) {
            log("[READ:%s] ReadFile error %lu", phase, err); break;
        }
        auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (rem <= 0) break;
        if (WaitForSingleObject(ov.hEvent, (DWORD)std::min(rem, (long long)100)) != WAIT_OBJECT_0) continue;

        DWORD got = 0;
        if (!GetOverlappedResult(hDev, &ov, &got, FALSE) || got == 0) continue;

        uint8_t rid = buf[0];
        seen[(int)rid]++;
        total++;
        if (seen[(int)rid] <= 2) {
            char hex[128] = {};
            int hpos = 0;
            for (DWORD k = 0; k < std::min(got, (DWORD)24) && hpos < 120; k++)
                hpos += sprintf_s(hex + hpos, 128-hpos, "%02x ", buf[k]);
            log("[READ:%s] ID=0x%02x len=%lu  %s", phase, rid, got, hex);
        }
    }
    CancelIo(hDev);
    CloseHandle(ov.hEvent);

    log("[READ:%s] total=%d IDs:", phase, total);
    for (auto& kv : seen) log("  0x%02x x%d", kv.first, kv.second);
    if (total == 0) log("  (none)");
}

// ─── Write helper — tries 3 methods with a given buffer ───────────────────────

static void TryWrite(HANDLE hDev, const char* label, const uint8_t* data, USHORT len) {
    std::vector<uint8_t> buf(data, data+len);

    // Method A: HidD_SetOutputReport
    BOOL okA = HidD_SetOutputReport(hDev, buf.data(), len);
    DWORD errA = okA ? 0 : GetLastError();

    // Method B: HidD_SetFeature
    BOOL okB = HidD_SetFeature(hDev, buf.data(), len);
    DWORD errB = okB ? 0 : GetLastError();

    // Method C: WriteFile (overlapped)
    OVERLAPPED ov = {}; ov.hEvent = CreateEvent(nullptr,TRUE,FALSE,nullptr);
    BOOL okC = WriteFile(hDev, buf.data(), len, nullptr, &ov);
    DWORD errC = 0;
    if (!okC) {
        errC = GetLastError();
        if (errC == ERROR_IO_PENDING) {
            DWORD done=0;
            okC = GetOverlappedResult(hDev, &ov, &done, TRUE);
            errC = okC ? 0 : GetLastError();
        }
    }
    CloseHandle(ov.hEvent);

    log("  %-42s  SetOutput=%s  SetFeature=%s  WriteFile=%s",
        label,
        okA ? "OK  " : (char*)((std::string("err=")+std::to_string(errA)).c_str()),
        okB ? "OK  " : (char*)((std::string("err=")+std::to_string(errB)).c_str()),
        okC ? "OK  " : (char*)((std::string("err=")+std::to_string(errC)).c_str()));
}

// ─── Try all output report IDs from 0x00 to 0xFF ─────────────────────────────

static void ScanOutputReportIDs(HANDLE hDev, USHORT outLen) {
    if (outLen == 0) { log("[SCAN] outLen=0, skipping"); return; }
    log("[SCAN] Scanning report IDs 0x00-0xFF with SetOutputReport (outLen=%u)...", outLen);
    std::vector<uint8_t> buf(outLen, 0);
    for (int rid = 0; rid <= 0xFF; rid++) {
        buf[0] = (uint8_t)rid;
        // put a recognizable payload
        if (outLen > 1) buf[1] = 0xAA;
        BOOL ok = HidD_SetOutputReport(hDev, buf.data(), outLen);
        if (ok) log("  SetOutputReport ID=0x%02x -> OK !", rid);
        // Only log non-87 errors (87 = invalid report ID, expected for most)
        else if (GetLastError() != 87) log("  SetOutputReport ID=0x%02x -> err=%lu", rid, GetLastError());
    }
    log("[SCAN] done");
}

// ─── Main ─────────────────────────────────────────────────────────────────────

static void TestWinMM() {
    printf("\n[WINMM] Testing WinMM joystick API...\n"); fflush(stdout);
    UINT numJoys = joyGetNumDevs();
    printf("[WINMM] joyGetNumDevs = %u\n", numJoys);
    for (UINT j = 0; j < std::min(numJoys, (UINT)16); j++) {
        JOYCAPSA caps = {};
        MMRESULT r = joyGetDevCapsA(j, &caps, sizeof(caps));
        if (r != JOYERR_NOERROR) continue;
        printf("[WINMM] Joystick %u: %s  VID=0x%04x PID=0x%04x  axes=%u buttons=%u\n",
            j, caps.szPname, caps.wMid, caps.wPid, caps.wNumAxes, caps.wNumButtons);

        // If this looks like our controller, listen for input
        if (caps.wMid == SWITCH_VID && caps.wPid == PRO_CTRL_PID) {
            printf("[WINMM] >>> This is our controller! Listening 5s, press A/B/X/Y (NOT Home) <<<\n");
            fflush(stdout);
            auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            JOYINFOEX prev = {}; prev.dwSize = sizeof(prev); prev.dwFlags = JOY_RETURNALL;
            joyGetPosEx(j, &prev);
            while (std::chrono::steady_clock::now() < dl) {
                JOYINFOEX info = {}; info.dwSize = sizeof(info); info.dwFlags = JOY_RETURNALL;
                if (joyGetPosEx(j, &info) == JOYERR_NOERROR) {
                    if (memcmp(&info.dwXpos, &prev.dwXpos, sizeof(DWORD)*6) != 0 ||
                        info.dwButtons != prev.dwButtons) {
                        printf("[WINMM] INPUT! buttons=0x%08lx  X=%ld Y=%ld Z=%ld R=%ld U=%ld V=%ld\n",
                            info.dwButtons, info.dwXpos, info.dwYpos, info.dwZpos,
                            info.dwRpos, info.dwUpos, info.dwVpos);
                        fflush(stdout);
                        prev = info;
                    }
                }
                Sleep(8);
            }
        }
    }
}

static void TestRawInput() {
    printf("\n[RAWINPUT] Registering for Raw Input from HID gamepads...\n"); fflush(stdout);

    // Create a message-only window to receive WM_INPUT
    WNDCLASSEXA wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "ProbeRawInput";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, "ProbeRawInput", nullptr, 0, 0,0,0,0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { printf("[RAWINPUT] CreateWindow failed %lu\n", GetLastError()); return; }

    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;  // Generic Desktop
    rid.usUsage     = 0x05;  // Gamepad
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        printf("[RAWINPUT] RegisterRawInputDevices failed %lu\n", GetLastError());
        DestroyWindow(hwnd); return;
    }

    printf("[RAWINPUT] Registered. Listening 8s, press A/B/X/Y (NOT Home)...\n"); fflush(stdout);
    auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    int count = 0;
    while (std::chrono::steady_clock::now() < dl) {
        MSG msg;
        while (PeekMessageA(&msg, hwnd, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_INPUT) {
                UINT sz = 0;
                GetRawInputData((HRAWINPUT)msg.lParam, RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER));
                std::vector<uint8_t> buf(sz);
                GetRawInputData((HRAWINPUT)msg.lParam, RID_INPUT, buf.data(), &sz, sizeof(RAWINPUTHEADER));
                RAWINPUT* ri = (RAWINPUT*)buf.data();
                if (ri->header.dwType == RIM_TYPEHID) {
                    count++;
                    printf("[RAWINPUT] #%d size=%u  ", count, ri->data.hid.dwSizeHid);
                    UINT printBytes = ri->data.hid.dwSizeHid < 24 ? ri->data.hid.dwSizeHid : 24;
                    for (UINT k=0; k<printBytes; k++)
                        printf("%02x ", ri->data.hid.bRawData[k]);
                    printf("\n"); fflush(stdout);
                }
            }
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        Sleep(1);
    }
    printf("[RAWINPUT] total=%d reports\n", count); fflush(stdout);
    DestroyWindow(hwnd);
}

int main() {
    printf("=== Switch 2 Pro Controller USB Probe v2 ===\n\n"); fflush(stdout);

    auto ifaces = FindAll();
    if (ifaces.empty()) {
        log("[!] No Switch 2 Pro Controller found (VID=057E PID=2069). Check USB connection.");
        getchar(); return 1;
    }

    log("[ENUM] Found %zu interface(s):", ifaces.size());
    for (size_t i = 0; i < ifaces.size(); i++) {
        auto& f = ifaces[i];
        log("  [%zu] UsagePage=0x%04x Usage=0x%04x In=%u Out=%u Feat=%u", i, f.usagePage, f.usage, f.inLen, f.outLen, f.featLen);
        log("       InBtnCaps=%u InValCaps=%u OutBtnCaps=%u OutValCaps=%u", f.numInBtnCaps, f.numInValCaps, f.numOutBtnCaps, f.numOutValCaps);
        if (!f.inputReportIDs.empty()) {
            printf("       Input report IDs:  "); for (auto r : f.inputReportIDs) printf("0x%02x ", r); printf("\n");
        } else log("       Input report IDs: (none declared in caps)");
        if (!f.outputReportIDs.empty()) {
            printf("       Output report IDs: "); for (auto r : f.outputReportIDs) printf("0x%02x ", r); printf("\n");
        } else log("       Output report IDs: (none declared in caps)");
        log("       rw_handle=%s ro_handle=%s", f.hDev==INVALID_HANDLE_VALUE?"FAIL":"OK", f.hDevRO==INVALID_HANDLE_VALUE?"FAIL":"OK");
    }

    // Test each interface
    for (size_t i = 0; i < ifaces.size(); i++) {
        auto& f = ifaces[i];
        log("\n====== Interface [%zu] UP=0x%04x U=0x%04x ======", i, f.usagePage, f.usage);

        // 1. Read before anything (rw and ro handles)
        log("--- Read-write handle ---");
        ReadMs(f.hDev, f.inLen, 1500, "RW_BEFORE");
        log("--- Read-only handle ---");
        ReadMs(f.hDevRO, f.inLen, 1500, "RO_BEFORE");

        // 2. Scan all output report IDs to find valid ones
        if (f.hDev != INVALID_HANDLE_VALUE)
            ScanOutputReportIDs(f.hDev, f.outLen);

        // 3. Try Nintendo Switch USB init with any output IDs we know about
        // Also try the known Switch 1 IDs AND zero-padded versions
        if (f.hDev != INVALID_HANDLE_VALUE && f.outLen > 0) {
            log("\n[INIT] Trying Nintendo USB init commands...");
            USHORT L = f.outLen;
            std::vector<uint8_t> buf(L, 0);

            auto trySend = [&](const char* label, std::initializer_list<uint8_t> bytes) {
                std::fill(buf.begin(), buf.end(), 0);
                size_t k = 0; for (auto b : bytes) { if (k < (size_t)L) buf[k++] = b; }
                TryWrite(f.hDev, label, buf.data(), L);
                Sleep(80);
            };

            // Switch 1 style (report ID 0x80)
            trySend("0x80 0x02 (USB handshake)",     {0x80, 0x02});
            trySend("0x80 0x03 (USB enable)",         {0x80, 0x03});
            trySend("0x80 0x04 (keepalive off)",       {0x80, 0x04});
            // Subcommand via report 0x01
            trySend("0x01 timer=0 subcmd=0x03 0x30",  {0x01,0x00,0,0,0,0,0,0,0,0, 0x03,0x30});
            trySend("0x01 timer=1 subcmd=0x40 0x01",  {0x01,0x01,0,0,0,0,0,0,0,0, 0x40,0x01});
            // Zero-padded: no report ID (device may not use IDs)
            trySend("0x00 0x80 0x02 (no-ID style)",   {0x00, 0x80, 0x02});
            trySend("0x00 0x01 0x00 subcmd=0x03 0x30",{0x00, 0x01,0x00,0,0,0,0,0,0,0,0, 0x03,0x30});
            // Try the valid output IDs found by scan
            for (auto rid : f.outputReportIDs) {
                char label[64]; sprintf_s(label, "known output ID=0x%02x", rid);
                buf[0] = (uint8_t)rid; buf[1] = 0x80; buf[2] = 0x02;
                TryWrite(f.hDev, label, buf.data(), L);
                Sleep(80);
            }
        }

        // 4. Try waking up via output report 0x02 (rumble/LED)
        if (f.hDev != INVALID_HANDLE_VALUE && f.outLen > 0 && !f.outputReportIDs.empty()) {
            log("\n[WAKE] Sending wake-up via report 0x02...");
            USHORT L = f.outLen;
            std::vector<uint8_t> wb(L, 0);
            // Try various wake payloads
            for (uint8_t payload : {0x01, 0x10, 0x20, 0x40, 0x80, 0xFF}) {
                std::fill(wb.begin(), wb.end(), 0);
                wb[0] = 0x02; wb[1] = payload;
                OVERLAPPED ov2 = {}; ov2.hEvent = CreateEvent(nullptr,TRUE,FALSE,nullptr);
                BOOL wok = WriteFile(f.hDev, wb.data(), L, nullptr, &ov2);
                if (!wok && GetLastError() == ERROR_IO_PENDING) {
                    DWORD done=0; GetOverlappedResult(f.hDev, &ov2, &done, TRUE); wok = TRUE;
                }
                CloseHandle(ov2.hEvent);
                log("  WriteFile report=0x02 payload=0x%02x -> %s", payload, wok?"OK":("FAIL err="+std::to_string(GetLastError())).c_str());
                if (wok) Sleep(100);
            }
        }

        // 5. Read after wake attempt — full detail on every received byte
        log("\n[READ:FULL_DETAIL] Press a button on the controller now! Listening 5s...");
        {
            HANDLE hRead = (f.hDev != INVALID_HANDLE_VALUE) ? f.hDev : f.hDevRO;
            DWORD bufSz = std::max((USHORT)64, f.inLen);
            std::vector<uint8_t> buf(bufSz);
            OVERLAPPED ov = {}; ov.hEvent = CreateEvent(nullptr,TRUE,FALSE,nullptr);
            auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(8000);
            int tot = 0;
            while (std::chrono::steady_clock::now() < dl) {
                ResetEvent(ov.hEvent);
                std::fill(buf.begin(), buf.end(), 0);
                ReadFile(hRead, buf.data(), bufSz, nullptr, &ov);
                DWORD err = GetLastError();
                if (err != 0 && err != ERROR_IO_PENDING) { log("[READ] error %lu", err); break; }
                auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(dl - std::chrono::steady_clock::now()).count();
                if (rem <= 0) break;
                if (WaitForSingleObject(ov.hEvent, (DWORD)std::min(rem,(long long)200)) != WAIT_OBJECT_0) continue;
                DWORD got=0;
                if (!GetOverlappedResult(hRead, &ov, &got, FALSE) || got==0) continue;
                tot++;
                // Print FULL hex dump for first 10 reports
                if (tot <= 10) {
                    printf("[READ] #%d ID=0x%02x len=%lu  ", tot, buf[0], got);
                    for (DWORD k=0; k<got; k++) {
                        printf("%02x ", buf[k]);
                        if ((k+1)%16==0) printf("\n  ");
                    }
                    printf("\n"); fflush(stdout);
                } else if (tot <= 50) {
                    // Just ID and first 8 bytes
                    char h[64]={};
                    int hp=0;
                    for (DWORD k=0; k<std::min(got,(DWORD)8); k++) hp+=sprintf_s(h+hp,64-hp,"%02x ",buf[k]);
                    log("[READ] #%d ID=0x%02x  %s", tot, buf[0], h);
                }
            }
            CancelIo(hRead);
            CloseHandle(ov.hEvent);
            log("[READ:FULL_DETAIL] total=%d reports", tot);
        }

        // 6. Synchronous read (non-overlapped handle) — last resort
        log("\n[READ:SYNC] Opening non-overlapped handle and blocking read (10s)...");
        log(">>> WCISNIJ DOWOLNY PRZYCISK (A/B/X/Y/LT/RT) - NIE HOME! <<<");
        {
            HANDLE hs = CreateFile(f.path, GENERIC_READ,
                FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (hs == INVALID_HANDLE_VALUE) {
                log("[READ:SYNC] open failed err=%lu", GetLastError());
            } else {
                // Set a read timeout via thread + event
                DWORD bufSz = std::max((USHORT)64, f.inLen);
                std::vector<uint8_t> buf(bufSz, 0);
                struct ReadResult { DWORD got=0; DWORD err=0; bool done=false; };
                auto rr = std::make_shared<ReadResult>();
                auto hs2 = hs;
                auto bufPtr = buf.data();

                std::thread t([hs2, bufPtr, bufSz, rr](){
                    DWORD got = 0;
                    BOOL ok = ReadFile(hs2, bufPtr, bufSz, &got, nullptr);
                    rr->got  = got;
                    rr->err  = ok ? 0 : GetLastError();
                    rr->done = true;
                });

                auto start = std::chrono::steady_clock::now();
                while (std::chrono::steady_clock::now() - start < std::chrono::seconds(8)) {
                    if (rr->done) break;
                    Sleep(100);
                }

                if (rr->done) {
                    if (rr->got > 0) {
                        printf("[READ:SYNC] GOT DATA! len=%lu\n  ", rr->got);
                        for (DWORD k=0; k<rr->got; k++) { printf("%02x ", buf[k]); if((k+1)%16==0) printf("\n  "); }
                        printf("\n"); fflush(stdout);
                    } else {
                        log("[READ:SYNC] err=%lu (0 bytes)", rr->err);
                    }
                } else {
                    log("[READ:SYNC] timed out after 8s — no data");
                    CancelIo(hs);
                }

                if (t.joinable()) t.join();
                CloseHandle(hs);
            }
        }

        log("====== Interface [%zu] done ======\n", i);
    }

    for (auto& f : ifaces) {
        if (f.hDev   != INVALID_HANDLE_VALUE) CloseHandle(f.hDev);
        if (f.hDevRO != INVALID_HANDLE_VALUE) CloseHandle(f.hDevRO);
    }

    // Test correct haptic format first (might wake up controller input mode)
    if (!ifaces.empty()) {
        auto& f = ifaces[0];
        if (f.hDev != INVALID_HANDLE_VALUE && f.outLen >= 64) {
            log("\n[HAPTIC] Sending correct haptic format (from procon2-driver) to wake up input...");
            // Format: {0x02, 0x50|counter, frame[5], zeros..., 0x50|counter, frame[5], zeros...}
            const uint8_t frames[3][5] = {
                {0x93, 0x35, 0x36, 0x1c, 0x0d},
                {0xa8, 0x29, 0xc5, 0xdc, 0x0c},
                {0x75, 0x21, 0xb5, 0x5d, 0x13}
            };
            for (int fi = 0; fi < 3; fi++) {
                std::vector<uint8_t> buf(f.outLen, 0);
                buf[0] = 0x02;
                buf[1] = 0x50 | (uint8_t)fi;
                memcpy(&buf[2], frames[fi], 5);
                buf[17] = buf[1];
                memcpy(&buf[18], frames[fi], 5);
                OVERLAPPED ov = {}; ov.hEvent = CreateEvent(nullptr,TRUE,FALSE,nullptr);
                BOOL ok = WriteFile(f.hDev, buf.data(), f.outLen, nullptr, &ov);
                if (!ok && GetLastError() == ERROR_IO_PENDING) {
                    DWORD d=0; GetOverlappedResult(f.hDev, &ov, &d, TRUE); ok = TRUE;
                }
                CloseHandle(ov.hEvent);
                log("  haptic frame %d -> %s", fi, ok ? "OK" : ("FAIL err="+std::to_string(GetLastError())).c_str());
                Sleep(4);
            }
            // Send stop
            std::vector<uint8_t> stop(f.outLen, 0);
            stop[0] = 0x02; stop[1] = 0x50; stop[17] = 0x50;
            OVERLAPPED ov2 = {}; ov2.hEvent = CreateEvent(nullptr,TRUE,FALSE,nullptr);
            WriteFile(f.hDev, stop.data(), f.outLen, nullptr, &ov2);
            if (GetLastError() == ERROR_IO_PENDING) { DWORD d=0; GetOverlappedResult(f.hDev, &ov2, &d, TRUE); }
            CloseHandle(ov2.hEvent);
            log("  haptic stop -> sent");

            // Now immediately read — does it activate input?
            log("[HAPTIC] Listening 5s for input after haptic. Press A/B/X/Y!");
            ReadMs(f.hDev, f.inLen, 5000, "AFTER_HAPTIC");
        }
    }

    // Brute-force scan: try all 256 single-byte payloads for report 0x02
    // and all 256 for 2-byte {cmd, 0x00} to find activation command
    if (!ifaces.empty()) {
        auto& f = ifaces[0];
        if (f.hDev != INVALID_HANDLE_VALUE && f.outLen > 0) {
            log("\n[BRUTE] Scanning all 256 payloads for output report 0x02...");
            log("[BRUTE] After each write, listening 80ms for input. Takes ~60s.");
            USHORT L = f.outLen;
            std::vector<uint8_t> wb(L, 0);
            std::vector<uint8_t> rb(std::max((USHORT)64, f.inLen), 0);
            bool found = false;

            for (int p = 0; p <= 0xFF && !found; p++) {
                // Write
                std::fill(wb.begin(), wb.end(), 0);
                wb[0] = 0x02; wb[1] = (uint8_t)p;
                {
                    OVERLAPPED ov = {}; ov.hEvent = CreateEvent(nullptr,TRUE,FALSE,nullptr);
                    WriteFile(f.hDev, wb.data(), L, nullptr, &ov);
                    if (GetLastError() == ERROR_IO_PENDING) WaitForSingleObject(ov.hEvent, 500);
                    CloseHandle(ov.hEvent);
                }

                // Read 80ms
                OVERLAPPED ov = {}; ov.hEvent = CreateEvent(nullptr,TRUE,FALSE,nullptr);
                ResetEvent(ov.hEvent);
                std::fill(rb.begin(), rb.end(), 0);
                ReadFile(f.hDev, rb.data(), (DWORD)rb.size(), nullptr, &ov);
                DWORD err = GetLastError();
                if (err != 0 && err != ERROR_IO_PENDING) { CloseHandle(ov.hEvent); continue; }
                if (WaitForSingleObject(ov.hEvent, 80) == WAIT_OBJECT_0) {
                    DWORD got = 0;
                    if (GetOverlappedResult(f.hDev, &ov, &got, FALSE) && got > 0) {
                        printf("[BRUTE] *** INPUT after payload=0x%02x! ID=0x%02x len=%lu  ", p, rb[0], got);
                        for (DWORD k=0; k<std::min(got,(DWORD)32); k++) printf("%02x ", rb[k]);
                        printf("***\n"); fflush(stdout);
                        found = true;
                    }
                }
                CancelIo(f.hDev);
                CloseHandle(ov.hEvent);

                if (p % 32 == 31) { printf("[BRUTE] scanned 0x%02x...\n", p); fflush(stdout); }
            }
            if (!found) log("[BRUTE] No activation found with single-byte payloads.");

            // Also try 2-byte combinations {0x02, cmd, sub}
            if (!found) {
                log("[BRUTE2] Trying 2-byte: {0x02, cmd=0..0xFF, sub=0x00}...");
                for (int p = 0; p <= 0xFF && !found; p++) {
                    std::fill(wb.begin(), wb.end(), 0);
                    wb[0] = 0x02; wb[1] = (uint8_t)p; wb[2] = 0x00;
                    {
                        OVERLAPPED ov2 = {}; ov2.hEvent = CreateEvent(nullptr,TRUE,FALSE,nullptr);
                        WriteFile(f.hDev, wb.data(), L, nullptr, &ov2);
                        if (GetLastError() == ERROR_IO_PENDING) WaitForSingleObject(ov2.hEvent, 500);
                        CloseHandle(ov2.hEvent);
                    }
                    OVERLAPPED ov = {}; ov.hEvent = CreateEvent(nullptr,TRUE,FALSE,nullptr);
                    std::fill(rb.begin(), rb.end(), 0);
                    ReadFile(f.hDev, rb.data(), (DWORD)rb.size(), nullptr, &ov);
                    if (GetLastError() == ERROR_IO_PENDING || GetLastError() == 0) {
                        if (WaitForSingleObject(ov.hEvent, 80) == WAIT_OBJECT_0) {
                            DWORD got = 0;
                            if (GetOverlappedResult(f.hDev, &ov, &got, FALSE) && got > 0) {
                                printf("[BRUTE2] *** INPUT after {0x02, 0x%02x, 0x00}! ID=0x%02x len=%lu  ", p, rb[0], got);
                                for (DWORD k=0; k<std::min(got,(DWORD)32); k++) printf("%02x ", rb[k]);
                                printf("***\n"); fflush(stdout);
                                found = true;
                            }
                        }
                    }
                    CancelIo(f.hDev);
                    CloseHandle(ov.hEvent);
                }
                if (!found) log("[BRUTE2] No activation found with 2-byte payloads either.");
            }
        }
    }

    // Try WinMM and Raw Input as alternative reading methods
    TestWinMM();
    TestRawInput();

    log("=== Probe complete ===");
    printf("Press Enter to exit...\n"); getchar();
    return 0;
}
