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
extern "C" DWORD WINAPI CM_Unregister_Notification(
    HCMNOTIFICATION NotifyContext
);
#endif

#ifndef DEVPROP_TYPE_GUID
#define DEVPROP_TYPE_GUID 0x0000000D
#endif

static const DEVPROPKEY kDevPKey_Device_IsConnected = {
    { 0x83da6326, 0x97a6, 0x4088, { 0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29 } }, 15
};
static const DEVPROPKEY kDevPKey_Device_ContainerId = {
    { 0x8c7ed206, 0x3f8a, 0x4827, { 0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c } }, 2
};

static HANDLE g_hWakeEvent = NULL;
static HANDLE g_hStopDaemon = NULL;
static wchar_t g_dllPath[MAX_PATH] = {0};
static DWORD_PTR g_batteryVarRva = 0;
static DWORD_PTR g_containerVarRva = 0;
static DWORD_PTR g_macVarRva = 0;
static volatile LONG g_currentBattery = -1;
static volatile bool g_isControllerConnected = false;
static volatile bool g_isServiceEnabled = true;
static volatile LONG g_forceTargetScan = 0;
static volatile LONG g_uiWarmUntilTick = 0;

// Thread-safe cached HID device state to avoid repeated SetupDi enumerations
static CRITICAL_SECTION g_cacheLock;
static wchar_t g_cachedDevicePath[512] = {0};
static DEVINST g_cachedDevInst = 0;
static wchar_t g_cachedContainerId[64] = {0};
static wchar_t g_cachedMacHex[32] = {0};

static void InvalidateDeviceCache() {
    EnterCriticalSection(&g_cacheLock);
    g_cachedDevicePath[0] = L'\0';
    g_cachedDevInst = 0;
    g_cachedContainerId[0] = L'\0';
    g_cachedMacHex[0] = L'\0';
    LeaveCriticalSection(&g_cacheLock);
}

static bool IsHidDevInstConnected(DEVINST devInst) {
    if (devInst == 0) return false;
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, devInst, 0) != CR_SUCCESS) {
        return false;
    }

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

static bool IsHexChar(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

static void ExtractDevNodeContainerAndMac(DEVINST devInst, wchar_t* outCid, size_t cidMax, wchar_t* outMac, size_t macMax) {
    if (outCid && cidMax > 0) outCid[0] = L'\0';
    if (outMac && macMax > 0) outMac[0] = L'\0';
    if (devInst == 0) return;

    DEVINST cur = devInst;
    for (int depth = 0; depth < 3 && cur != 0; depth++) {
        if (outCid && outCid[0] == L'\0' && cidMax >= 37) {
            DEVPROPTYPE propType = 0;
            GUID cidGuid = {0};
            ULONG bufSize = sizeof(cidGuid);
            if (CM_Get_DevNode_PropertyW(cur, &kDevPKey_Device_ContainerId, &propType, (PBYTE)&cidGuid, &bufSize, 0) == CR_SUCCESS &&
                propType == DEVPROP_TYPE_GUID && bufSize == sizeof(GUID)) {
                wsprintfW(outCid, L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                          cidGuid.Data1, cidGuid.Data2, cidGuid.Data3,
                          cidGuid.Data4[0], cidGuid.Data4[1], cidGuid.Data4[2], cidGuid.Data4[3],
                          cidGuid.Data4[4], cidGuid.Data4[5], cidGuid.Data4[6], cidGuid.Data4[7]);
            }
        }

        if (outMac && outMac[0] == L'\0' && macMax >= 13) {
            wchar_t devId[512] = {0};
            if (CM_Get_Device_IDW(cur, devId, 512, 0) == CR_SUCCESS) {
                if (_wcsnicmp(devId, L"BTHENUM\\", 8) == 0) {
                    // Match "&<12 hex chars>_" in BTHENUM instance IDs
                    for (const wchar_t* p = devId; *p; p++) {
                        if (*p == L'&') {
                            const wchar_t* cand = p + 1;
                            bool ok = true;
                            for (int k = 0; k < 12; k++) {
                                if (!cand[k] || !IsHexChar(cand[k])) {
                                    ok = false;
                                    break;
                                }
                            }
                            if (ok && (cand[12] == L'_' || cand[12] == L'\\' || cand[12] == L'\0')) {
                                for (int k = 0; k < 12; k++) {
                                    wchar_t c = cand[k];
                                    if (c >= L'A' && c <= L'F') c += 32;
                                    outMac[k] = c;
                                }
                                outMac[12] = L'\0';
                                break;
                            }
                        }
                    }
                }
            }
        }

        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS) break;
        cur = parent;
    }
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
        InvalidateDeviceCache();
        if (g_hWakeEvent) SetEvent(g_hWakeEvent);
    }
    return ERROR_SUCCESS;
}

static bool FindOrVerifySwitchPro(wchar_t* outPath, size_t outPathMax, DEVINST* outDevInst,
                                  wchar_t* outCid, size_t cidMax, wchar_t* outMac, size_t macMax) {
    wchar_t cachedPath[512] = {0};
    DEVINST cachedInst = 0;
    wchar_t cachedCid[64] = {0};
    wchar_t cachedMac[32] = {0};

    EnterCriticalSection(&g_cacheLock);
    wcsncpy(cachedPath, g_cachedDevicePath, 511);
    cachedPath[511] = L'\0';
    cachedInst = g_cachedDevInst;
    wcsncpy(cachedCid, g_cachedContainerId, 63);
    cachedCid[63] = L'\0';
    wcsncpy(cachedMac, g_cachedMacHex, 31);
    cachedMac[31] = L'\0';
    LeaveCriticalSection(&g_cacheLock);

    // 1. Fast path: verify cached device node is still connected
    if (cachedPath[0] != L'\0' && cachedInst != 0) {
        if (IsHidDevInstConnected(cachedInst)) {
            if (outPath) { wcsncpy(outPath, cachedPath, outPathMax - 1); outPath[outPathMax - 1] = L'\0'; }
            if (outDevInst) *outDevInst = cachedInst;
            if (outCid) { wcsncpy(outCid, cachedCid, cidMax - 1); outCid[cidMax - 1] = L'\0'; }
            if (outMac) { wcsncpy(outMac, cachedMac, macMax - 1); outMac[macMax - 1] = L'\0'; }
            return true;
        }
        InvalidateDeviceCache();
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
                    wchar_t cidBuf[64] = {0};
                    wchar_t macBuf[32] = {0};
                    ExtractDevNodeContainerAndMac(devInfoData.DevInst, cidBuf, 64, macBuf, 32);

                    EnterCriticalSection(&g_cacheLock);
                    wcsncpy(g_cachedDevicePath, detailData->DevicePath, 511);
                    g_cachedDevicePath[511] = L'\0';
                    g_cachedDevInst = devInfoData.DevInst;
                    wcsncpy(g_cachedContainerId, cidBuf, 63);
                    g_cachedContainerId[63] = L'\0';
                    wcsncpy(g_cachedMacHex, macBuf, 31);
                    g_cachedMacHex[31] = L'\0';
                    LeaveCriticalSection(&g_cacheLock);

                    if (outPath) { wcsncpy(outPath, detailData->DevicePath, outPathMax - 1); outPath[outPathMax - 1] = L'\0'; }
                    if (outDevInst) *outDevInst = devInfoData.DevInst;
                    if (outCid) { wcsncpy(outCid, cidBuf, cidMax - 1); outCid[cidMax - 1] = L'\0'; }
                    if (outMac) { wcsncpy(outMac, macBuf, macMax - 1); outMac[macMax - 1] = L'\0'; }
                    found = true;
                    break;
                }
            }
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

static int ParseBatteryPctFromByte(BYTE batByte) {
    BYTE nibble = (batByte >> 4) & 0x0F;
    BYTE level = nibble & 0x0E;
    switch (level) {
        case 8: return 100;
        case 6: return 75;
        case 4: return 50;
        case 2: return 25;
        case 0: return 10;
        default: return 50;
    }
}

static bool SendSwitchSubcmd(HANDLE hDev, BYTE subcmdId, const BYTE* argData, DWORD argLen, BYTE* pPktNum) {
    BYTE subcmd[64] = {0};
    subcmd[0] = 0x01;
    subcmd[1] = (++(*pPktNum)) & 0x0F;
    subcmd[2] = 0x00; subcmd[3] = 0x01; subcmd[4] = 0x40; subcmd[5] = 0x40;
    subcmd[6] = 0x00; subcmd[7] = 0x01; subcmd[8] = 0x40; subcmd[9] = 0x40;
    subcmd[10] = subcmdId;
    if (argData && argLen > 0) {
        DWORD copyLen = (argLen > 52) ? 52 : argLen;
        memcpy(&subcmd[11], argData, copyLen);
    }

    OVERLAPPED ovW = {0};
    ovW.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ovW.hEvent) return false;

    DWORD bw = 0;
    BOOL ok = WriteFile(hDev, subcmd, 49, &bw, &ovW);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ovW.hEvent, 80) == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(hDev, &ovW, &bw, FALSE);
            } else {
                CancelIo(hDev);
                DWORD dummy = 0;
                GetOverlappedResult(hDev, &ovW, &dummy, TRUE);
                ok = FALSE;
            }
        }
    }
    CloseHandle(ovW.hEvent);
    return (ok != FALSE);
}

static bool ReadOneHidPacket(HANDLE hDev, BYTE* buf, DWORD bufSize, HANDLE hEvent, DWORD timeoutMs,
                             DWORD* outBytesRead, bool* outDisconnected) {
    *outBytesRead = 0;
    *outDisconnected = false;
    memset(buf, 0, bufSize);
    ResetEvent(hEvent);

    OVERLAPPED ov = {0};
    ov.hEvent = hEvent;
    DWORD bytesRead = 0;
    BOOL r = ReadFile(hDev, buf, bufSize, &bytesRead, &ov);
    if (!r) {
        DWORD err = GetLastError();
        if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_GEN_FAILURE) {
            *outDisconnected = true;
            return false;
        }
        if (err != ERROR_IO_PENDING) {
            return false;
        }
        if (WaitForSingleObject(hEvent, timeoutMs) == WAIT_OBJECT_0) {
            if (!GetOverlappedResult(hDev, &ov, &bytesRead, FALSE)) {
                DWORD err2 = GetLastError();
                if (err2 == ERROR_DEVICE_NOT_CONNECTED || err2 == ERROR_GEN_FAILURE) {
                    *outDisconnected = true;
                }
                return false;
            }
        } else {
            CancelIo(hDev);
            DWORD dummy = 0;
            GetOverlappedResult(hDev, &ov, &dummy, TRUE);
            return false;
        }
    }
    *outBytesRead = bytesRead;
    return (bytesRead > 0);
}

static bool ReadSwitchProBatteryHID(int* outPct, bool* outConnected,
                                    wchar_t* outCid, size_t cidMax,
                                    wchar_t* outMac, size_t macMax) {
    *outConnected = false;
    wchar_t devPath[512] = {0};
    DEVINST devInst = 0;
    if (!FindOrVerifySwitchPro(devPath, 512, &devInst, outCid, cidMax, outMac, macMax)) {
        return false;
    }

    bool hasWriteAccess = true;
    HANDLE hDev = CreateFileW(devPath,
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (hDev == INVALID_HANDLE_VALUE) {
        hasWriteAccess = false;
        hDev = CreateFileW(devPath,
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    }
    if (hDev == INVALID_HANDLE_VALUE) {
        InvalidateDeviceCache();
        return false;
    }

    *outConnected = true;
    HANDLE hReadEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!hReadEvent) {
        CloseHandle(hDev);
        return false;
    }

    BYTE buf[512];
    static BYTE s_pktNum = 0;
    bool success = false;
    bool sawFullStream = false;

    // 1. Short passive listen (~50ms): if controller is already in 0x30 mode (e.g. Steam/game), read without any write
    DWORD bytesRead = 0;
    bool disconnected = false;
    if (ReadOneHidPacket(hDev, buf, sizeof(buf), hReadEvent, 50, &bytesRead, &disconnected)) {
        if (bytesRead >= 3 && (buf[0] == 0x30 || buf[0] == 0x21)) {
            *outPct = ParseBatteryPctFromByte(buf[2]);
            success = true;
            sawFullStream = true;
        }
    }
    if (disconnected) {
        *outConnected = false;
        InvalidateDeviceCache();
    }

    // 2. Non-intrusive query via subcmd 0x02 (Request Device Info -> 0x21 reply containing battery)
    // Leaves controller in Windows' default 0x3F Simple Mode to avoid phantom stick/scroll interference in Settings
    if (!success && *outConnected && hasWriteAccess) {
        SendSwitchSubcmd(hDev, 0x02, NULL, 0, &s_pktNum);
        DWORD waitStart = GetTickCount();
        while (GetTickCount() - waitStart < 280) {
            if (ReadOneHidPacket(hDev, buf, sizeof(buf), hReadEvent, 90, &bytesRead, &disconnected)) {
                if (bytesRead >= 3 && (buf[0] == 0x30 || buf[0] == 0x21)) {
                    *outPct = ParseBatteryPctFromByte(buf[2]);
                    success = true;
                    break;
                }
            }
            if (disconnected) {
                *outConnected = false;
                InvalidateDeviceCache();
                break;
            }
        }
    }

    // 3. Fallback only if subcmd 0x02 didn't yield a reply: temporarily switch to 0x30, read once, and restore 0x3F
    if (!success && *outConnected && hasWriteAccess && !sawFullStream) {
        BYTE modeFull = 0x30;
        SendSwitchSubcmd(hDev, 0x03, &modeFull, 1, &s_pktNum);
        DWORD waitStart = GetTickCount();
        while (GetTickCount() - waitStart < 450) {
            if (ReadOneHidPacket(hDev, buf, sizeof(buf), hReadEvent, 150, &bytesRead, &disconnected)) {
                if (bytesRead >= 3 && (buf[0] == 0x30 || buf[0] == 0x21)) {
                    *outPct = ParseBatteryPctFromByte(buf[2]);
                    success = true;
                    break;
                }
            }
            if (disconnected) {
                *outConnected = false;
                InvalidateDeviceCache();
                break;
            }
        }
        if (*outConnected) {
            BYTE modeSimple = 0x3F;
            SendSwitchSubcmd(hDev, 0x03, &modeSimple, 1, &s_pktNum);
        }
    }

    CloseHandle(hReadEvent);
    CloseHandle(hDev);
    return success;
}

// Target process tracking with handle and address caching
struct TargetProcess {
    DWORD pid;
    HANDLE hProcess;
    BYTE* remoteBase;
    LONG lastBatteryWritten;
    wchar_t lastContainerWritten[64];
    wchar_t lastMacWritten[32];
    bool isShellHost;
    bool injectionAttempted;
};

#define MAX_TARGETS 16
static TargetProcess g_targets[MAX_TARGETS] = {0};
static DWORD g_lastProcessScanTick = 0;

typedef LONG (NTAPI *NtQuerySystemInformation_t)(ULONG SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength, PULONG ReturnLength);

static bool IsProcessUwpSuspended(DWORD targetPid) {
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    auto fnQSI = hNtdll ? (NtQuerySystemInformation_t)GetProcAddress(hNtdll, "NtQuerySystemInformation") : nullptr;
    if (!fnQSI) return false;

    ULONG bufSize = 262144;
    BYTE* buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, bufSize);
    if (!buf) return false;

    ULONG retLen = 0;
    LONG status = 0;
    for (int tries = 0; tries < 5; tries++) {
        status = fnQSI(5 /* SystemProcessInformation */, buf, bufSize, &retLen);
        if (status != (LONG)0xC0000004L /* STATUS_INFO_LENGTH_MISMATCH */) break;
        HeapFree(GetProcessHeap(), 0, buf);
        bufSize = retLen + 16384;
        buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, bufSize);
        if (!buf) return false;
    }

    bool suspended = false;
    if (status == 0) {
        ULONG offset = 0;
        while (offset + 256 <= bufSize) {
            BYTE* cur = buf + offset;
            ULONG nextOff = *(ULONG*)(cur + 0);
            ULONG numThreads = *(ULONG*)(cur + 4);
            ULONG_PTR pid = *(ULONG_PTR*)(cur + 80);

            if (pid == (ULONG_PTR)targetPid) {
                if (numThreads > 0 && offset + 256 + (SIZE_T)numThreads * 80 <= bufSize) {
                    ULONG suspCount = 0;
                    for (ULONG t = 0; t < numThreads; t++) {
                        BYTE* th = cur + 256 + (SIZE_T)t * 80;
                        ULONG threadState = *(ULONG*)(th + 68);
                        ULONG waitReason = *(ULONG*)(th + 72);
                        if (threadState == 5 && waitReason == 5) {
                            suspCount++;
                        }
                    }
                    suspended = (suspCount == numThreads);
                }
                break;
            }
            if (nextOff == 0) break;
            offset += nextOff;
        }
    }

    HeapFree(GetProcessHeap(), 0, buf);
    return suspended;
}

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

static bool HasInjectedShellHost() {
    for (int i = 0; i < MAX_TARGETS; i++) {
        if (g_targets[i].pid != 0 && g_targets[i].isShellHost && g_targets[i].remoteBase != nullptr) {
            return true;
        }
    }
    return false;
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
    if (!target || !target->hProcess || target->remoteBase || target->injectionAttempted) return;
    // Wait until kernel32.dll is mapped in newly spawned processes before calling LoadLibraryW
    if (!FindRemoteModuleBase(target->pid, L"kernel32.dll")) {
        return;
    }
    target->injectionAttempted = true;

    size_t pathBytes = (wcslen(g_dllPath) + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(target->hProcess, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool safeToFree = true;
    if (remoteMem) {
        if (WriteProcessMemory(target->hProcess, remoteMem, g_dllPath, pathBytes, NULL)) {
            HANDLE hThread = CreateRemoteThread(target->hProcess, NULL, 0,
                (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"),
                remoteMem, 0, NULL);
            if (hThread) {
                DWORD waitRes = WaitForSingleObject(hThread, 2000);
                // Never free remoteMem while LoadLibraryW might still read it later (prevents 0xc0000005 in ntdll)
                if (waitRes != WAIT_OBJECT_0) {
                    safeToFree = false;
                }
                CloseHandle(hThread);
            }
        }
        if (safeToFree) {
            VirtualFreeEx(target->hProcess, remoteMem, 0, MEM_RELEASE);
        }
    }
    target->remoteBase = FindRemoteModuleBase(target->pid, L"SwitchControllerBatteryHook.dll");
    target->lastBatteryWritten = -999;
    target->lastContainerWritten[0] = L'\0';
    target->lastMacWritten[0] = L'\0';
}

static void RefreshTargetProcesses(bool force) {
    DWORD now = GetTickCount();
    CleanupDeadTargets();

    bool hasShellHost = HasInjectedShellHost();

    // Only skip scan when not forced, ShellHost is active, and cooldown hasn't elapsed
    if (!force && hasShellHost && (now - g_lastProcessScanTick < 3500)) {
        return;
    }
    g_lastProcessScanTick = now;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    // First pass check: if ShellHost.exe exists (Windows 11 24H2+), ignore ShellExperienceHost.exe
    bool osHasShellHost = false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"ShellHost.exe") == 0) {
                osHasShellHost = true;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }

    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            bool isShell = (_wcsicmp(pe.szExeFile, L"ShellHost.exe") == 0) ||
                           (!osHasShellHost && _wcsicmp(pe.szExeFile, L"ShellExperienceHost.exe") == 0);
            bool isSettings = (_wcsicmp(pe.szExeFile, L"SystemSettings.exe") == 0);
            if (!isShell && !isSettings) continue;

            bool alreadyKnown = false;
            for (int i = 0; i < MAX_TARGETS; i++) {
                if (g_targets[i].pid == pe.th32ProcessID) {
                    alreadyKnown = true;
                    break;
                }
            }
            if (alreadyKnown) continue;

            // Skip suspended UWP instances until resumed by Windows
            if (IsProcessUwpSuspended(pe.th32ProcessID)) continue;

            for (int i = 0; i < MAX_TARGETS; i++) {
                if (g_targets[i].pid == 0) {
                    HANDLE hProc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
                                               PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        memset(&g_targets[i], 0, sizeof(TargetProcess));
                        g_targets[i].pid = pe.th32ProcessID;
                        g_targets[i].hProcess = hProc;
                        g_targets[i].isShellHost = isShell;
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

static void BroadcastBatteryToTargets(LONG batteryLevel, bool forceRescan,
                                      const wchar_t* containerId, const wchar_t* macHex) {
    RefreshTargetProcesses(forceRescan);

    wchar_t cidToWrite[64] = {0};
    wchar_t macToWrite[32] = {0};
    if (containerId && containerId[0]) {
        wcsncpy(cidToWrite, containerId, 63);
    }
    if (macHex && macHex[0]) {
        wcsncpy(macToWrite, macHex, 31);
    }

    for (int i = 0; i < MAX_TARGETS; i++) {
        if (g_targets[i].pid != 0 && g_targets[i].hProcess) {
            if (!g_targets[i].remoteBase && !g_targets[i].injectionAttempted) {
                InjectTargetHook(&g_targets[i]);
            }
            if (!g_targets[i].remoteBase) continue;

            // Sync dynamic device filters before raising g_SwitchBatteryLevel >= 0
            if (cidToWrite[0] && g_containerVarRva != 0 && _wcsicmp(g_targets[i].lastContainerWritten, cidToWrite) != 0) {
                SIZE_T written = 0;
                if (WriteProcessMemory(g_targets[i].hProcess, g_targets[i].remoteBase + g_containerVarRva, cidToWrite, sizeof(cidToWrite), &written)) {
                    wcscpy(g_targets[i].lastContainerWritten, cidToWrite);
                }
            }
            if (macToWrite[0] && g_macVarRva != 0 && _wcsicmp(g_targets[i].lastMacWritten, macToWrite) != 0) {
                SIZE_T written = 0;
                if (WriteProcessMemory(g_targets[i].hProcess, g_targets[i].remoteBase + g_macVarRva, macToWrite, sizeof(macToWrite), &written)) {
                    wcscpy(g_targets[i].lastMacWritten, macToWrite);
                }
            }

            if (g_batteryVarRva != 0 && g_targets[i].lastBatteryWritten != batteryLevel) {
                SIZE_T written = 0;
                if (WriteProcessMemory(g_targets[i].hProcess, g_targets[i].remoteBase + g_batteryVarRva, &batteryLevel, sizeof(LONG), &written)) {
                    g_targets[i].lastBatteryWritten = batteryLevel;
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

    // Initial injection into ShellHost / SystemSettings so hooks are primed immediately
    BroadcastBatteryToTargets(-1, true, nullptr, nullptr);

    int lastPct = -1;
    DWORD lastHidPollTick = 0;
    wchar_t curCid[64] = {0};
    wchar_t curMac[32] = {0};

    while (WaitForSingleObject(g_hStopDaemon, 0) == WAIT_TIMEOUT) {
        wchar_t devPath[512] = {0};
        DEVINST devInst = 0;
        bool connected = FindOrVerifySwitchPro(devPath, 512, &devInst, curCid, 64, curMac, 32);
        g_isControllerConnected = connected;

        bool forceScan = (InterlockedExchange(&g_forceTargetScan, 0) != 0);
        LONG warmUntil = InterlockedCompareExchange(&g_uiWarmUntilTick, 0, 0);
        bool inWarmUiWindow = ((LONG)(GetTickCount() - (DWORD)warmUntil) < 0);

        if (!connected) {
            if (lastPct != -1) {
                lastPct = -1;
                g_currentBattery = -1;
                curCid[0] = L'\0';
                curMac[0] = L'\0';
                BroadcastBatteryToTargets(-1, forceScan, nullptr, nullptr);
            } else {
                CleanupDeadTargets();
                BroadcastBatteryToTargets(-1, forceScan || inWarmUiWindow, nullptr, nullptr);
            }
            // Keep hooks ready in newly launched Settings/ShellHost, otherwise sleep cleanly
            DWORD sleepMs = inWarmUiWindow ? 150 : 4000;
            HANDLE waits[2] = { g_hStopDaemon, g_hWakeEvent };
            DWORD r = WaitForMultipleObjects(2, waits, FALSE, sleepMs);
            if (r == WAIT_OBJECT_0) break;
            continue;
        }

        // Controller connected: read battery every 15s (or immediately on first connect)
        DWORD now = GetTickCount();
        if (lastPct < 0 || (now - lastHidPollTick >= 15000)) {
            int newPct = -1;
            bool hidConn = false;
            if (ReadSwitchProBatteryHID(&newPct, &hidConn, curCid, 64, curMac, 32)) {
                lastPct = newPct;
                g_currentBattery = newPct;
                lastHidPollTick = GetTickCount();
            } else if (!hidConn) {
                lastPct = -1;
                g_currentBattery = -1;
                curCid[0] = L'\0';
                curMac[0] = L'\0';
                InvalidateDeviceCache();
            }
        }

        if (lastPct >= 0 && g_isServiceEnabled) {
            BroadcastBatteryToTargets((LONG)lastPct, forceScan || inWarmUiWindow, curCid, curMac);
        } else if (!g_isServiceEnabled && lastPct >= 0) {
            BroadcastBatteryToTargets(-1, forceScan || inWarmUiWindow, curCid, curMac);
        } else if (forceScan || inWarmUiWindow || !HasInjectedShellHost()) {
            BroadcastBatteryToTargets(-1, forceScan || inWarmUiWindow, curCid, curMac);
        }

        // Retry fast (150ms) right after connect or UI launch; otherwise 1000ms idle cycle
        DWORD sleepMs = (lastPct < 0 || inWarmUiWindow) ? 150 : 1000;
        HANDLE waits[2] = { g_hStopDaemon, g_hWakeEvent };
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, sleepMs);
        if (r == WAIT_OBJECT_0) break;
    }

    if (hNotify) {
        CM_Unregister_Notification(hNotify);
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

static void UpdateUiLabels() {
    if (!g_hStatusLabel || !g_hBatteryLabel) return;
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

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if ((event != EVENT_SYSTEM_FOREGROUND && event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_SHOW) ||
        !hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) {
        return;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return;

    wchar_t imgPath[MAX_PATH] = {0};
    DWORD sz = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hProc, 0, imgPath, &sz);
    CloseHandle(hProc);
    if (!ok) return;

    const wchar_t* base = wcsrchr(imgPath, L'\\');
    base = base ? (base + 1) : imgPath;

    if (_wcsicmp(base, L"SystemSettings.exe") == 0 ||
        _wcsicmp(base, L"ApplicationFrameHost.exe") == 0 ||
        _wcsicmp(base, L"ShellHost.exe") == 0 ||
        _wcsicmp(base, L"ShellExperienceHost.exe") == 0) {
        InterlockedExchange(&g_uiWarmUntilTick, (LONG)(GetTickCount() + 3000));
        InterlockedExchange(&g_forceTargetScan, 1);
        if (g_hWakeEvent) SetEvent(g_hWakeEvent);
    }
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HBRUSH hbrBkgnd = NULL;
    static HFONT hFontTitle = NULL;
    static HFONT hFontBody = NULL;

    switch (msg) {
        case WM_CREATE: {
            hbrBkgnd = CreateSolidBrush(RGB(32, 32, 32));

            // Enable Windows 11 Dark Mode Title Bar
            BOOL darkMode = TRUE;
            DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

            hFontTitle = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable");
            hFontBody = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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

            return 0;
        }

        case WM_SHOWWINDOW: {
            if (wParam) {
                UpdateUiLabels();
                SetTimer(hWnd, 1, 500, NULL);
            } else {
                KillTimer(hWnd, 1);
            }
            return 0;
        }

        case WM_TIMER: {
            if (wParam == 1) {
                UpdateUiLabels();
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
                InvalidateDeviceCache();
                InterlockedExchange(&g_forceTargetScan, 1);
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

        case WM_CLOSE: {
            // Hide panel instead of exiting so daemon & single-instance reopening stay active with 0 UI timer ticks
            KillTimer(hWnd, 1);
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }

        case WM_DESTROY: {
            KillTimer(hWnd, 1);
            if (hFontTitle) { DeleteObject(hFontTitle); hFontTitle = NULL; }
            if (hFontBody) { DeleteObject(hFontBody); hFontBody = NULL; }
            if (hbrBkgnd) { DeleteObject(hbrBkgnd); hbrBkgnd = NULL; }
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    bool isBackground = (wcsstr(lpCmdLine, L"--background") != NULL);

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\SwitchControllerBattery_Mutex");
    bool alreadyRunning = (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS);

    // If already running, prevent duplicate daemon thread and pop open the existing panel if requested
    if (alreadyRunning) {
        if (isBackground) {
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }
        HWND existingWnd = NULL;
        for (int attempt = 0; attempt < 5; attempt++) {
            existingWnd = FindWindowW(L"SwitchControllerBatteryWndClass", NULL);
            if (existingWnd) break;
            Sleep(100);
        }
        if (existingWnd) {
            ShowWindow(existingWnd, SW_SHOW);
            ShowWindow(existingWnd, SW_RESTORE);
            SetForegroundWindow(existingWnd);
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
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
    if (!pExport) { FreeLibrary(hLocalDll); return 2; }
    g_batteryVarRva = (DWORD_PTR)((BYTE*)pExport - (BYTE*)hLocalDll);

    void* pCid = (void*)GetProcAddress(hLocalDll, "g_TargetContainerId");
    if (pCid) g_containerVarRva = (DWORD_PTR)((BYTE*)pCid - (BYTE*)hLocalDll);
    void* pMac = (void*)GetProcAddress(hLocalDll, "g_TargetMacHex");
    if (pMac) g_macVarRva = (DWORD_PTR)((BYTE*)pMac - (BYTE*)hLocalDll);
    FreeLibrary(hLocalDll);

    InitializeCriticalSection(&g_cacheLock);
    g_hWakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_hStopDaemon = CreateEventW(NULL, TRUE, FALSE, NULL);

    HANDLE hDaemonThread = CreateThread(NULL, 0, DaemonThread, NULL, 0, NULL);

    // Initialize Win32 Common Controls
    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    HBRUSH hWndBg = CreateSolidBrush(RGB(32, 32, 32));
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = hWndBg;
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

    if (!hWnd) {
        SetEvent(g_hStopDaemon);
        WaitForSingleObject(hDaemonThread, 3000);
        CloseHandle(hDaemonThread);
        DeleteCriticalSection(&g_cacheLock);
        return 3;
    }

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

    if (!isBackground) {
        ShowWindow(hWnd, SW_SHOWNORMAL);
        SetForegroundWindow(hWnd);
        UpdateWindow(hWnd);
    }

    HWINEVENTHOOK hWinFg = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
    HWINEVENTHOOK hWinCreate = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE,
        NULL, WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
    HWINEVENTHOOK hWinShow = SetWinEventHook(
        EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
        NULL, WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hWinFg) UnhookWinEvent(hWinFg);
    if (hWinCreate) UnhookWinEvent(hWinCreate);
    if (hWinShow) UnhookWinEvent(hWinShow);
    if (g_hStopDaemon) SetEvent(g_hStopDaemon);
    if (hDaemonThread) {
        WaitForSingleObject(hDaemonThread, 3000);
        CloseHandle(hDaemonThread);
    }
    if (hIconBig) DestroyIcon(hIconBig);
    if (hIconSmall) DestroyIcon(hIconSmall);
    if (hWndBg) DeleteObject(hWndBg);
    DeleteCriticalSection(&g_cacheLock);
    if (hMutex) CloseHandle(hMutex);

    return 0;
}
