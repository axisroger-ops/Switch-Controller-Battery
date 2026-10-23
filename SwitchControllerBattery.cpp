// SwitchControllerBattery.cpp - Native Windows 11 Battery Indicator & Control Panel
// Clean, Ultra-lightweight (0% CPU, ~1.5 MB RAM, 0 external dependencies)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <cfgmgr32.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <stdlib.h>
#include <wchar.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DEVPROP_TYPE_BOOLEAN
typedef ULONG DEVPROPTYPE, *PDEVPROPTYPE;
typedef CHAR DEVPROP_BOOLEAN, *PDEVPROP_BOOLEAN;
#define DEVPROP_TYPE_BOOLEAN 0x00000011
#define DEVPROP_TRUE ((DEVPROP_BOOLEAN)-1)
#define DEVPROP_FALSE ((DEVPROP_BOOLEAN)0)
typedef struct _DEVPROPKEY {
    GUID fmtid;
    ULONG pid;
} DEVPROPKEY;
#endif

#ifndef CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE
typedef PVOID HCMNOTIFICATION, *PHCMNOTIFICATION;
typedef enum _CM_NOTIFY_FILTER_TYPE {
    CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE = 0,
    CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE,
    CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE
} CM_NOTIFY_FILTER_TYPE;
typedef enum _CM_NOTIFY_ACTION {
    CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL = 0,
    CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL
} CM_NOTIFY_ACTION;
typedef struct _CM_NOTIFY_EVENT_DATA {
    CM_NOTIFY_FILTER_TYPE FilterType;
    DWORD Reserved;
    union {
        struct {
            GUID ClassGuid;
            WCHAR SymbolicLink[ANYSIZE_ARRAY];
        } DeviceInterface;
    } u;
} CM_NOTIFY_EVENT_DATA, *PCM_NOTIFY_EVENT_DATA;
typedef struct _CM_NOTIFY_FILTER {
    DWORD cbSize;
    DWORD Flags;
    CM_NOTIFY_FILTER_TYPE FilterType;
    DWORD Reserved;
    union {
        struct {
            GUID ClassGuid;
        } DeviceInterface;
    } u;
} CM_NOTIFY_FILTER, *PCM_NOTIFY_FILTER;
typedef DWORD (CALLBACK *PCM_NOTIFY_CALLBACK)(
    HCMNOTIFICATION hNotify,
    PVOID Context,
    CM_NOTIFY_ACTION Action,
    PCM_NOTIFY_EVENT_DATA EventData,
    DWORD EventDataSize
);
extern "C" DWORD WINAPI CM_Register_Notification(
    PCM_NOTIFY_FILTER pFilter,
    PVOID pContext,
    PCM_NOTIFY_CALLBACK pCallback,
    PHCMNOTIFICATION pNotifyContext
);
#endif

static const DEVPROPKEY kDevPKey_Device_IsConnected = {
    { 0x83da6326, 0x97a6, 0x4088, { 0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29 } }, 15
};

static HANDLE g_hWakeEvent = NULL;
static HANDLE g_hStopDaemon = NULL;
static wchar_t g_dllPath[MAX_PATH] = {0};
static DWORD_PTR g_batteryVarRva = 0;
static volatile LONG g_currentBattery = -1;
static volatile bool g_isControllerConnected = false;
static volatile bool g_isServiceEnabled = true;

// Cached HID device path to avoid repeated SetupDi enumerations
static wchar_t g_cachedDevicePath[512] = {0};
static DEVINST g_cachedDevInst = 0;

static bool IsHidDevInstConnected(DEVINST devInst) {
    DEVINST cur = devInst;
    for (int depth = 0; depth < 3 && cur != 0; depth++) {
        DEVPROPTYPE propType = 0;
        DEVPROP_BOOLEAN isConn = DEVPROP_TRUE;
        ULONG bufSize = sizeof(isConn);
        if (CM_Get_DevNode_PropertyW(cur, &kDevPKey_Device_IsConnected, &propType, (PBYTE)&isConn, &bufSize, 0) == CR_SUCCESS) {
            if (propType == DEVPROP_TYPE_BOOLEAN) {
                return (isConn != DEVPROP_FALSE);
            }
        }
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS) break;
        cur = parent;
    }
    return true;
}

static DWORD WINAPI PnpNotifyCallback(
    HCMNOTIFICATION hNotify,
    PVOID Context,
    CM_NOTIFY_ACTION Action,
    PCM_NOTIFY_EVENT_DATA EventData,
    DWORD EventDataSize)
{
    (void)hNotify; (void)Context; (void)EventData; (void)EventDataSize;
    if (Action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL ||
        Action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
        // Invalidate cached path on device arrival or removal
        g_cachedDevicePath[0] = L'\0';
        g_cachedDevInst = 0;
        if (g_hWakeEvent) SetEvent(g_hWakeEvent);
    }
    return ERROR_SUCCESS;
}

static bool FindOrVerifySwitchPro(wchar_t* outPath, size_t outPathMax, DEVINST* outDevInst) {
    // 1. Fast path: check cached device path first
    if (g_cachedDevicePath[0] != L'\0' && g_cachedDevInst != 0) {
        if (IsHidDevInstConnected(g_cachedDevInst)) {
            if (outPath) wcsncpy(outPath, g_cachedDevicePath, outPathMax);
            if (outDevInst) *outDevInst = g_cachedDevInst;
            return true;
        }
        // Device node reports disconnected -> invalidate cache
        g_cachedDevicePath[0] = L'\0';
        g_cachedDevInst = 0;
    }

    // 2. Slow path: single enumeration of HID devices only when cache is cold
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return false;

    SP_DEVICE_INTERFACE_DATA devInterfaceData;
    devInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    bool found = false;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &devInterfaceData); i++) {
        wchar_t buf[512];
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf;
        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devInfoData = {0};
        devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &devInterfaceData, detailData, sizeof(buf), NULL, &devInfoData)) {
            if (wcsstr(detailData->DevicePath, L"057e") || wcsstr(detailData->DevicePath, L"057E")) {
                if (IsHidDevInstConnected(devInfoData.DevInst)) {
                    wcsncpy(g_cachedDevicePath, detailData->DevicePath, 512);
                    g_cachedDevInst = devInfoData.DevInst;
                    if (outPath) wcsncpy(outPath, detailData->DevicePath, outPathMax);
                    if (outDevInst) *outDevInst = devInfoData.DevInst;
                    found = true;
                    break;
                }
            }
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

static bool ReadSwitchProBatteryHID(int* outPct, bool* outConnected) {
    *outConnected = false;
    wchar_t devPath[512] = {0};
    DEVINST devInst = 0;
    if (!FindOrVerifySwitchPro(devPath, 512, &devInst)) {
        return false;
    }

    HANDLE hDev = CreateFileW(devPath,
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (hDev == INVALID_HANDLE_VALUE) {
        hDev = CreateFileW(devPath,
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    }
    if (hDev == INVALID_HANDLE_VALUE) {
        g_cachedDevicePath[0] = L'\0';
        g_cachedDevInst = 0;
        return false;
    }

    *outConnected = true;
    BYTE buf[512];
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD startTick = GetTickCount();
    static BYTE s_pktNum = 0;
    bool success = false;

    while (GetTickCount() - startTick < 1500) {
        memset(buf, 0, sizeof(buf));
        ResetEvent(ov.hEvent);
        DWORD bytesRead = 0;
        BOOL r = ReadFile(hDev, buf, sizeof(buf), &bytesRead, &ov);
        if (!r) {
            DWORD err = GetLastError();
            if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_GEN_FAILURE) {
                *outConnected = false;
                g_cachedDevicePath[0] = L'\0';
                g_cachedDevInst = 0;
                break;
            }
            if (err == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ov.hEvent, 250) == WAIT_OBJECT_0) {
                    if (!GetOverlappedResult(hDev, &ov, &bytesRead, FALSE)) {
                        DWORD err2 = GetLastError();
                        if (err2 == ERROR_DEVICE_NOT_CONNECTED || err2 == ERROR_GEN_FAILURE) {
                            *outConnected = false;
                            g_cachedDevicePath[0] = L'\0';
                            g_cachedDevInst = 0;
                            break;
                        }
                    }
                } else {
                    CancelIo(hDev);
                    DWORD dummy = 0;
                    GetOverlappedResult(hDev, &ov, &dummy, TRUE);
                    BYTE subcmd[64] = {0};
                    subcmd[0] = 0x01;
                    subcmd[1] = (++s_pktNum) & 0x0F;
                    subcmd[2] = 0x00; subcmd[3] = 0x01; subcmd[4] = 0x40; subcmd[5] = 0x40;
                    subcmd[6] = 0x00; subcmd[7] = 0x01; subcmd[8] = 0x40; subcmd[9] = 0x40;
                    subcmd[10] = 0x03; // Subcmd 0x03: Set Input Report Mode
                    subcmd[11] = 0x30; // 0x30 = Standard full report (battery, IMU, buttons)
                    OVERLAPPED ovW = {0};
                    ovW.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
                    DWORD bw = 0;
                    if (!WriteFile(hDev, subcmd, 49, &bw, &ovW)) {
                        if (GetLastError() == ERROR_IO_PENDING) {
                            if (WaitForSingleObject(ovW.hEvent, 50) != WAIT_OBJECT_0) {
                                CancelIo(hDev);
                                GetOverlappedResult(hDev, &ovW, &bw, TRUE);
                            }
                        }
                    }
                    CloseHandle(ovW.hEvent);
                    continue;
                }
            }
        }
        if (bytesRead >= 1 && buf[0] == 0x3F) {
            // Controller is in Simple Mode after boot/reconnect; wake it into Standard Report Mode
            BYTE subcmd[64] = {0};
            subcmd[0] = 0x01;
            subcmd[1] = (++s_pktNum) & 0x0F;
            subcmd[2] = 0x00; subcmd[3] = 0x01; subcmd[4] = 0x40; subcmd[5] = 0x40;
            subcmd[6] = 0x00; subcmd[7] = 0x01; subcmd[8] = 0x40; subcmd[9] = 0x40;
            subcmd[10] = 0x03; // Subcmd 0x03: Set Input Report Mode
            subcmd[11] = 0x30; // 0x30 = Standard full report
            OVERLAPPED ovW = {0};
            ovW.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            DWORD bw = 0;
            if (!WriteFile(hDev, subcmd, 49, &bw, &ovW)) {
                if (GetLastError() == ERROR_IO_PENDING) {
                    WaitForSingleObject(ovW.hEvent, 100);
                }
            }
            CloseHandle(ovW.hEvent);
            continue;
        }
        if (bytesRead >= 3 && (buf[0] == 0x30 || buf[0] == 0x21)) {
            BYTE batByte = buf[2];
            BYTE nibble = (batByte >> 4) & 0x0F;
            BYTE level = nibble & 0x0E;
            int pct = 50;
            switch (level) {
                case 8: pct = 100; break;
                case 6: pct = 75;  break;
                case 4: pct = 50;  break;
                case 2: pct = 25;  break;
                case 0: pct = 10;  break;
            }
            *outPct = pct;
            success = true;
            break;
        }
    }
    CloseHandle(ov.hEvent);
    CloseHandle(hDev);
    return success;
}

// Target process tracking with handle and address caching
struct TargetProcess {
    DWORD pid;
    HANDLE hProcess;
    BYTE* remoteBase;
    LONG lastBatteryWritten;
};

#define MAX_TARGETS 8
static TargetProcess g_targets[MAX_TARGETS] = {0};
static DWORD g_lastProcessScanTick = 0;

static void CleanupDeadTargets() {
    for (int i = 0; i < MAX_TARGETS; i++) {
        if (g_targets[i].pid != 0) {
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(g_targets[i].hProcess, &exitCode) || exitCode != STILL_ACTIVE) {
                if (g_targets[i].hProcess) CloseHandle(g_targets[i].hProcess);
                memset(&g_targets[i], 0, sizeof(TargetProcess));
            }
        }
    }
}

static BYTE* FindRemoteModuleBase(DWORD pid, const wchar_t* modName) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    BYTE* base = nullptr;
    if (Module32FirstW(hSnap, &me)) {
        do {
            if (_wcsicmp(me.szModule, modName) == 0) {
                base = me.modBaseAddr;
                break;
            }
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);
    return base;
}

static void InjectTargetHook(TargetProcess* target) {
    if (!target || !target->hProcess || target->remoteBase) return;
    size_t pathBytes = (wcslen(g_dllPath) + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(target->hProcess, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteMem) {
        if (WriteProcessMemory(target->hProcess, remoteMem, g_dllPath, pathBytes, NULL)) {
            HANDLE hThread = CreateRemoteThread(target->hProcess, NULL, 0,
                (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"),
                remoteMem, 0, NULL);
            if (hThread) {
                WaitForSingleObject(hThread, 300);
                CloseHandle(hThread);
            }
        }
        VirtualFreeEx(target->hProcess, remoteMem, 0, MEM_RELEASE);
    }
    target->remoteBase = FindRemoteModuleBase(target->pid, L"SwitchControllerBatteryHook.dll");
    target->lastBatteryWritten = -999;
}

static void RefreshTargetProcesses(bool force) {
    DWORD now = GetTickCount();
    CleanupDeadTargets();

    // Check if we already have an active ShellHost handle
    bool hasShellHost = false;
    for (int i = 0; i < MAX_TARGETS; i++) {
        if (g_targets[i].pid != 0 && g_targets[i].remoteBase != nullptr) {
            hasShellHost = true;
            break;
        }
    }

    // Only scan OS processes if forced, or missing ShellHost, or every 5 seconds (to detect newly opened Settings)
    if (!force && hasShellHost && (now - g_lastProcessScanTick < 5000)) {
        return;
    }
    g_lastProcessScanTick = now;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            bool isTarget = (_wcsicmp(pe.szExeFile, L"ShellHost.exe") == 0 ||
                             _wcsicmp(pe.szExeFile, L"SystemSettings.exe") == 0);
            if (!isTarget) continue;

            bool alreadyKnown = false;
            for (int i = 0; i < MAX_TARGETS; i++) {
                if (g_targets[i].pid == pe.th32ProcessID) {
                    alreadyKnown = true;
                    break;
                }
            }
            if (alreadyKnown) continue;

            for (int i = 0; i < MAX_TARGETS; i++) {
                if (g_targets[i].pid == 0) {
                    HANDLE hProc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
                                               PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        g_targets[i].pid = pe.th32ProcessID;
                        g_targets[i].hProcess = hProc;
                        g_targets[i].remoteBase = FindRemoteModuleBase(pe.th32ProcessID, L"SwitchControllerBatteryHook.dll");
                        g_targets[i].lastBatteryWritten = -999;

                        if (!g_targets[i].remoteBase) {
                            InjectTargetHook(&g_targets[i]);
                        }
                    }
                    break;
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
}

static void BroadcastBatteryToTargets(LONG batteryLevel, bool forceRescan) {
    RefreshTargetProcesses(forceRescan);

    for (int i = 0; i < MAX_TARGETS; i++) {
        if (g_targets[i].pid != 0 && g_targets[i].hProcess) {
            if (!g_targets[i].remoteBase) {
                InjectTargetHook(&g_targets[i]);
            }
            if (g_targets[i].remoteBase && g_batteryVarRva != 0) {
                // Write ONLY if value actually changed or never synced
                if (g_targets[i].lastBatteryWritten != batteryLevel) {
                    SIZE_T written = 0;
                    if (WriteProcessMemory(g_targets[i].hProcess, g_targets[i].remoteBase + g_batteryVarRva, &batteryLevel, sizeof(LONG), &written)) {
                        g_targets[i].lastBatteryWritten = batteryLevel;
                    }
                }
            }
        }
    }
}

static DWORD WINAPI DaemonThread(LPVOID) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    CM_NOTIFY_FILTER filter = {0};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = hidGuid;
    HCMNOTIFICATION hNotify = NULL;
    CM_Register_Notification(&filter, NULL, PnpNotifyCallback, &hNotify);

    // Initial injection into ShellHost so hooks are primed immediately
    BroadcastBatteryToTargets(-1, true);

    int lastPct = -1;
    DWORD lastHidPollTick = 0;

    while (WaitForSingleObject(g_hStopDaemon, 0) == WAIT_TIMEOUT) {
        wchar_t devPath[512] = {0};
        DEVINST devInst = 0;
        bool connected = FindOrVerifySwitchPro(devPath, 512, &devInst);
        g_isControllerConnected = connected;

        if (!connected) {
            if (lastPct != -1) {
                lastPct = -1;
                g_currentBattery = -1;
                BroadcastBatteryToTargets(-1, false);
            }
            // Controller not connected: deep sleep. Instant wake via Windows PnP event on plug/connect!
            HANDLE waits[2] = { g_hStopDaemon, g_hWakeEvent };
            DWORD r = WaitForMultipleObjects(2, waits, FALSE, 4000);
            if (r == WAIT_OBJECT_0) break;
            continue;
        }

        // Controller connected: read battery every 15s (or immediately on first connect)
        DWORD now = GetTickCount();
        if (lastPct < 0 || (now - lastHidPollTick >= 15000)) {
            int newPct = -1;
            bool hidConn = false;
            if (ReadSwitchProBatteryHID(&newPct, &hidConn)) {
                lastPct = newPct;
                g_currentBattery = newPct;
                lastHidPollTick = GetTickCount();
            } else if (!hidConn) {
                lastPct = -1;
                g_currentBattery = -1;
                g_cachedDevicePath[0] = L'\0';
                g_cachedDevInst = 0;
            }
        }

        if (lastPct >= 0 && g_isServiceEnabled) {
            BroadcastBatteryToTargets((LONG)lastPct, false);
        } else if (!g_isServiceEnabled && lastPct >= 0) {
            BroadcastBatteryToTargets(-1, false);
        }

        // Wait 1000 ms before next check, or instant wake on user refresh / PnP event
        HANDLE waits[2] = { g_hStopDaemon, g_hWakeEvent };
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, 1000);
        if (r == WAIT_OBJECT_0) break;
    }

    // Cleanup target handles
    for (int i = 0; i < MAX_TARGETS; i++) {
        if (g_targets[i].hProcess) {
            CloseHandle(g_targets[i].hProcess);
            g_targets[i].hProcess = NULL;
        }
    }

    return 0;
}

// ============================================================================
// Windows 11 Native Control Panel UI (C++ Win32, Dark Mode, Minimal footprint)
// ============================================================================

#define IDC_LABEL_STATUS  1001
#define IDC_LABEL_BATTERY 1002
#define IDC_CHK_ENABLED   1003
#define IDC_CHK_STARTUP   1004
#define IDC_BTN_REFRESH   1005

static HWND g_hStatusLabel = NULL;
static HWND g_hBatteryLabel = NULL;
static HWND g_hChkEnabled = NULL;
static HWND g_hChkStartup = NULL;

static bool IsRunOnStartup() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD size = 0;
        LONG res = RegQueryValueExW(hKey, L"SwitchControllerBattery", NULL, &type, NULL, &size);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS);
    }
    return false;
}

static void SetRunOnStartup(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            wchar_t cmd[MAX_PATH + 32];
            wsprintfW(cmd, L"\"%s\" --background", exePath);
            RegSetValueExW(hKey, L"SwitchControllerBattery", 0, REG_SZ, (const BYTE*)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"SwitchControllerBattery");
        }
        RegCloseKey(hKey);
    }
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HBRUSH hbrBkgnd = NULL;

    switch (msg) {
        case WM_CREATE: {
            hbrBkgnd = CreateSolidBrush(RGB(32, 32, 32));

            // Enable Windows 11 Dark Mode Title Bar
            BOOL darkMode = TRUE;
            DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

            HFONT hFontTitle = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable");
            HFONT hFontBody = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable");

            HWND hHeader = CreateWindowW(L"STATIC", L"🎮 Switch Controller Battery",
                                        WS_CHILD | WS_VISIBLE, 20, 18, 380, 24,
                                        hWnd, NULL, NULL, NULL);
            SendMessageW(hHeader, WM_SETFONT, (WPARAM)hFontTitle, TRUE);

            g_hStatusLabel = CreateWindowW(L"STATIC", L"Statut : Recherche de la manette...",
                                          WS_CHILD | WS_VISIBLE, 20, 56, 380, 22,
                                          hWnd, (HMENU)IDC_LABEL_STATUS, NULL, NULL);
            SendMessageW(g_hStatusLabel, WM_SETFONT, (WPARAM)hFontBody, TRUE);

            g_hBatteryLabel = CreateWindowW(L"STATIC", L"Batterie : --",
                                           WS_CHILD | WS_VISIBLE, 20, 84, 380, 26,
                                           hWnd, (HMENU)IDC_LABEL_BATTERY, NULL, NULL);
            SendMessageW(g_hBatteryLabel, WM_SETFONT, (WPARAM)hFontTitle, TRUE);

            g_hChkEnabled = CreateWindowW(L"BUTTON", L"Activer l'affichage natif de la batterie",
                                         WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 20, 122, 380, 24,
                                         hWnd, (HMENU)IDC_CHK_ENABLED, NULL, NULL);
            SendMessageW(g_hChkEnabled, WM_SETFONT, (WPARAM)hFontBody, TRUE);
            SendMessageW(g_hChkEnabled, BM_SETCHECK, g_isServiceEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

            g_hChkStartup = CreateWindowW(L"BUTTON", L"Lancer automatiquement au démarrage de Windows",
                                         WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 20, 152, 380, 24,
                                         hWnd, (HMENU)IDC_CHK_STARTUP, NULL, NULL);
            SendMessageW(g_hChkStartup, WM_SETFONT, (WPARAM)hFontBody, TRUE);
            if (IsRunOnStartup()) {
                SendMessageW(g_hChkStartup, BM_SETCHECK, BST_CHECKED, 0);
            }

            HWND hBtnRefresh = CreateWindowW(L"BUTTON", L"Actualiser",
                                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 192, 110, 32,
                                            hWnd, (HMENU)IDC_BTN_REFRESH, NULL, NULL);
            SendMessageW(hBtnRefresh, WM_SETFONT, (WPARAM)hFontBody, TRUE);

            SetTimer(hWnd, 1, 500, NULL);
            return 0;
        }

        case WM_TIMER: {
            if (wParam == 1) {
                if (g_isControllerConnected) {
                    SetWindowTextW(g_hStatusLabel, L"Statut : Connectée (Bluetooth / USB)");
                    int bat = g_currentBattery;
                    if (bat >= 0) {
                        wchar_t buf[64];
                        wsprintfW(buf, L"Batterie : %d%%", bat);
                        SetWindowTextW(g_hBatteryLabel, buf);
                    } else {
                        SetWindowTextW(g_hBatteryLabel, L"Batterie : Lecture en cours...");
                    }
                } else {
                    SetWindowTextW(g_hStatusLabel, L"Statut : Déconnectée / En veille");
                    SetWindowTextW(g_hBatteryLabel, L"Batterie : Non disponible");
                }
            }
            return 0;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId == IDC_CHK_ENABLED) {
                LRESULT checked = SendMessageW(g_hChkEnabled, BM_GETCHECK, 0, 0);
                g_isServiceEnabled = (checked == BST_CHECKED);
                if (g_hWakeEvent) SetEvent(g_hWakeEvent);
            } else if (wmId == IDC_CHK_STARTUP) {
                LRESULT checked = SendMessageW(g_hChkStartup, BM_GETCHECK, 0, 0);
                SetRunOnStartup(checked == BST_CHECKED);
            } else if (wmId == IDC_BTN_REFRESH) {
                g_cachedDevicePath[0] = L'\0';
                g_cachedDevInst = 0;
                g_lastProcessScanTick = 0;
                if (g_hWakeEvent) SetEvent(g_hWakeEvent);
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            SetTextColor(hdcStatic, RGB(240, 240, 240));
            SetBkColor(hdcStatic, RGB(32, 32, 32));
            return (LRESULT)hbrBkgnd;
        }

        case WM_CTLCOLORBTN: {
            return (LRESULT)hbrBkgnd;
        }

        case WM_DESTROY: {
            if (hbrBkgnd) DeleteObject(hbrBkgnd);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    bool isBackground = (wcsstr(lpCmdLine, L"--background") != NULL);

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\SwitchControllerBattery_Mutex");
    bool alreadyRunning = (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS);

    // If already running, prevent duplicate daemon thread and handle contention
    if (alreadyRunning) {
        if (isBackground) return 0;
        HWND existingWnd = FindWindowW(L"SwitchControllerBatteryWndClass", NULL);
        if (existingWnd) {
            ShowWindow(existingWnd, SW_RESTORE);
            SetForegroundWindow(existingWnd);
            return 0;
        }
    }

    // Resolve local and AppContainer-accessible DLL staging path
    wchar_t localDllPath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, localDllPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(localDllPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    wcscat(localDllPath, L"SwitchControllerBatteryHook.dll");

    // If installed in a custom or system directory, use local DLL directly
    if (wcsstr(localDllPath, L"\\Users\\") == NULL && GetFileAttributesW(localDllPath) != INVALID_FILE_ATTRIBUTES) {
        wcscpy(g_dllPath, localDllPath);
    } else {
        // Portable mode inside a user profile directory: stage to ProgramData for AppContainer safety
        CreateDirectoryW(L"C:\\ProgramData\\SwitchControllerBattery", NULL);
        wcscpy(g_dllPath, L"C:\\ProgramData\\SwitchControllerBattery\\SwitchControllerBatteryHook.dll");
        if (!CopyFileW(localDllPath, g_dllPath, FALSE)) {
            if (GetFileAttributesW(g_dllPath) == INVALID_FILE_ATTRIBUTES) {
                wcscpy(g_dllPath, localDllPath);
            }
        }
    }

    HMODULE hLocalDll = LoadLibraryExW(g_dllPath, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!hLocalDll) return 1;
    void* pExport = (void*)GetProcAddress(hLocalDll, "g_SwitchBatteryLevel");
    if (!pExport) return 2;
    g_batteryVarRva = (DWORD_PTR)((BYTE*)pExport - (BYTE*)hLocalDll);
    FreeLibrary(hLocalDll);

    g_hWakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_hStopDaemon = CreateEventW(NULL, TRUE, FALSE, NULL);

    HANDLE hDaemonThread = CreateThread(NULL, 0, DaemonThread, NULL, 0, NULL);

    if (isBackground) {
        // Run completely headless
        WaitForSingleObject(hDaemonThread, INFINITE);
        return 0;
    }

    // Initialize Win32 Common Controls
    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(32, 32, 32));
    wc.lpszClassName = L"SwitchControllerBatteryWndClass";

    RegisterClassExW(&wc);

    int w = 420, h = 260;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND hWnd = CreateWindowExW(
        0, wc.lpszClassName, L"Switch Controller Battery",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, w, h, NULL, NULL, hInstance, NULL
    );

    if (!hWnd) return 3;

    // Load custom app icon if present
    wchar_t icoPath[MAX_PATH];
    GetModuleFileNameW(NULL, icoPath, MAX_PATH);
    wchar_t* pSlash = wcsrchr(icoPath, L'\\');
    if (pSlash) *(pSlash + 1) = L'\0';
    wcscat(icoPath, L"app.ico");

    HICON hIconBig = (HICON)LoadImageW(NULL, icoPath, IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
    HICON hIconSmall = (HICON)LoadImageW(NULL, icoPath, IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
    if (!hIconBig) {
        hIconBig = (HICON)LoadImageW(NULL, L"C:\\ProgramData\\SwitchControllerBattery\\app.ico", IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
        hIconSmall = (HICON)LoadImageW(NULL, L"C:\\ProgramData\\SwitchControllerBattery\\app.ico", IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
    }
    if (hIconBig) SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    if (hIconSmall) SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);

    ShowWindow(hWnd, SW_SHOWNORMAL);
    SetForegroundWindow(hWnd);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Keep daemon running silently in background even after closing the configuration window
    WaitForSingleObject(hDaemonThread, INFINITE);
    CloseHandle(hDaemonThread);

    return 0;
}
