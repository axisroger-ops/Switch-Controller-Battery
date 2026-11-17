// SwitchControllerBatteryHook.cpp - Ultra-lightweight WinRT ABI Hook DLL (~14 KB, Zero dependencies)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <wchar.h>

#ifndef _HSTRING_DEFINED
DECLARE_HANDLE(HSTRING);
#endif

typedef PCWSTR (WINAPI *WindowsGetStringRawBuffer_t)(HSTRING string, UINT32 *length);
typedef HRESULT (WINAPI *WindowsDeleteString_t)(HSTRING string);

static PCWSTR SafeGetStringRawBuffer(HSTRING hStr, UINT32* pLen) {
    static WindowsGetStringRawBuffer_t fn = nullptr;
    if (!fn) {
        HMODULE hMod = GetModuleHandleW(L"combase.dll");
        if (!hMod) hMod = LoadLibraryW(L"api-ms-win-core-winrt-string-l1-1-0.dll");
        if (hMod) fn = (WindowsGetStringRawBuffer_t)GetProcAddress(hMod, "WindowsGetStringRawBuffer");
    }
    return fn ? fn(hStr, pLen) : nullptr;
}

static void SafeDeleteString(HSTRING hStr) {
    static WindowsDeleteString_t fn = nullptr;
    if (!fn) {
        HMODULE hMod = GetModuleHandleW(L"combase.dll");
        if (!hMod) hMod = LoadLibraryW(L"api-ms-win-core-winrt-string-l1-1-0.dll");
        if (hMod) fn = (WindowsDeleteString_t)GetProcAddress(hMod, "WindowsDeleteString");
    }
    if (fn) fn(hStr);
}

// Exported shared state updated directly by SwitchControllerBattery.exe via WriteProcessMemory
extern "C" {
__declspec(dllexport) volatile LONG g_SwitchBatteryLevel = -1;
__declspec(dllexport) wchar_t g_TargetContainerId[64] = {0};
__declspec(dllexport) wchar_t g_TargetMacHex[32] = {0};
}

static wchar_t AsciiLower(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c;
}

static bool WcsContainsI(const wchar_t* haystack, const wchar_t* needle) {
    if (!haystack || !needle || !needle[0]) return false;
    for (const wchar_t* h = haystack; *h; h++) {
        const wchar_t* hp = h;
        const wchar_t* np = needle;
        while (*hp && *np && AsciiLower(*hp) == AsciiLower(*np)) {
            hp++;
            np++;
        }
        if (*np == L'\0') return true;
    }
    return false;
}

static bool MatchMacFlexible(const wchar_t* haystack, const wchar_t* macHex) {
    if (!haystack || !macHex || !macHex[0]) return false;
    if (WcsContainsI(haystack, macHex)) return true;

    // Also match colon-separated MAC form (xx:xx:xx:xx:xx:xx) if 12 hex digits
    size_t len = wcslen(macHex);
    if (len == 12) {
        wchar_t colonMac[18];
        int out = 0;
        for (int i = 0; i < 12; i += 2) {
            if (i > 0) colonMac[out++] = L':';
            colonMac[out++] = macHex[i];
            colonMac[out++] = macHex[i + 1];
        }
        colonMac[out] = L'\0';
        if (WcsContainsI(haystack, colonMac)) return true;
    }
    return false;
}

typedef HRESULT (__fastcall *IDeviceBase_get_ShowBatteryStatus_t)(void* pThis, bool* outShow);
typedef HRESULT (__fastcall *IDeviceBase_get_BatteryLife_t)(void* pThis, int* outBat);
typedef HRESULT (__fastcall *IDeviceBase_get_Id_t)(void* pThis, HSTRING* outStr);

static IDeviceBase_get_ShowBatteryStatus_t orig_get_ShowBatteryStatus = nullptr;
static IDeviceBase_get_BatteryLife_t orig_get_BatteryLife = nullptr;

struct InlineHook20 {
    BYTE* target = nullptr;
    BYTE* trampoline = nullptr;
    BYTE origBytes[20] = {0};
    bool installed = false;

    bool Install(BYTE* fnTarget, void* fnDetour, void** ppOrig) {
        if (!fnTarget || installed) return false;
        // Verify exact 20-byte position-independent prologue:
        // 48 89 5C 24 10 (mov [rsp+10h], rbx)
        // 57             (push rdi)
        // 48 83 EC 20    (sub rsp, 20h)
        // 48 8B FA       (mov rdi, rdx)
        // 80 B9 E4 00 00 00 00 (cmp byte ptr [rcx+0E4h], 0)
        if (fnTarget[0] != 0x48 || fnTarget[1] != 0x89 || fnTarget[2] != 0x5C || fnTarget[3] != 0x24 || fnTarget[4] != 0x10 ||
            fnTarget[5] != 0x57 || fnTarget[6] != 0x48 || fnTarget[7] != 0x83 || fnTarget[8] != 0xEC || fnTarget[9] != 0x20 ||
            fnTarget[10] != 0x48 || fnTarget[11] != 0x8B || fnTarget[12] != 0xFA ||
            fnTarget[13] != 0x80 || fnTarget[14] != 0xB9) {
            return false;
        }

        trampoline = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!trampoline) return false;

        memcpy(origBytes, fnTarget, 20);
        memcpy(trampoline, origBytes, 20);

        // Jump from trampoline+20 back to fnTarget+20: FF 25 00 00 00 00 <8-byte addr>
        trampoline[20] = 0xFF;
        trampoline[21] = 0x25;
        *(DWORD*)(trampoline + 22) = 0;
        *(ULONG_PTR*)(trampoline + 26) = (ULONG_PTR)(fnTarget + 20);
        FlushInstructionCache(GetCurrentProcess(), trampoline, 34);

        DWORD oldProt = 0;
        if (!VirtualProtect(fnTarget, 20, PAGE_EXECUTE_READWRITE, &oldProt)) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            trampoline = nullptr;
            return false;
        }

        BYTE patch[20];
        patch[0] = 0xFF;
        patch[1] = 0x25;
        *(DWORD*)(patch + 2) = 0;
        *(ULONG_PTR*)(patch + 6) = (ULONG_PTR)fnDetour;
        for (int i = 14; i < 20; i++) patch[i] = 0x90; // NOP padding

        memcpy(fnTarget, patch, 20);
        VirtualProtect(fnTarget, 20, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), fnTarget, 20);

        target = fnTarget;
        *ppOrig = (void*)trampoline;
        installed = true;
        return true;
    }

    void Uninstall() {
        if (!installed || !target) return;
        DWORD oldProt = 0;
        if (VirtualProtect(target, 20, PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy(target, origBytes, 20);
            VirtualProtect(target, 20, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), target, 20);
        }
        if (trampoline) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            trampoline = nullptr;
        }
        installed = false;
    }
};

static InlineHook20 g_hookShowBattery;
static InlineHook20 g_hookBatteryLife;
static volatile LONG g_hooksApplied = 0;
static HANDLE g_stopEvent = NULL;
static HANDLE g_dllLoadedEvent = NULL;
static HANDLE g_watcherThread = NULL;
static SRWLOCK g_cacheLock = SRWLOCK_INIT;

static void ReadTargetFilters(wchar_t* outCid, size_t cidMax, wchar_t* outMac, size_t macMax) {
    for (size_t i = 0; i < cidMax - 1; i++) {
        wchar_t ch = g_TargetContainerId[i];
        outCid[i] = ch;
        if (ch == L'\0') break;
    }
    outCid[cidMax - 1] = L'\0';

    for (size_t i = 0; i < macMax - 1; i++) {
        wchar_t ch = g_TargetMacHex[i];
        outMac[i] = ch;
        if (ch == L'\0') break;
    }
    outMac[macMax - 1] = L'\0';
}

static bool IsProController(void* pThis) {
    if (!pThis || IsBadReadPtr(pThis, sizeof(void*))) return false;

    wchar_t targetCid[64];
    wchar_t targetMac[32];
    ReadTargetFilters(targetCid, 64, targetMac, 32);

    // Fast thread-safe cache keyed on pointer, active target filter, and short 500ms TTL
    static void* s_cachedThis = nullptr;
    static bool  s_cachedResult = false;
    static DWORD s_cachedTick = 0;
    static wchar_t s_cachedCid[64] = {0};
    static wchar_t s_cachedMac[32] = {0};
    DWORD now = GetTickCount();

    AcquireSRWLockShared(&g_cacheLock);
    bool cacheHit = (pThis == s_cachedThis && (now - s_cachedTick < 500) &&
                     _wcsicmp(s_cachedCid, targetCid) == 0 &&
                     _wcsicmp(s_cachedMac, targetMac) == 0);
    bool cachedVal = s_cachedResult;
    ReleaseSRWLockShared(&g_cacheLock);
    if (cacheHit) {
        return cachedVal;
    }

    void** vtable = *(void***)pThis;
    if (!vtable || IsBadReadPtr(vtable, sizeof(void*) * 10)) return false;

    auto fnGetId = (IDeviceBase_get_Id_t)vtable[6];
    if (!fnGetId) return false;

    HSTRING hStr = NULL;
    if (SUCCEEDED(fnGetId(pThis, &hStr)) && hStr) {
        UINT32 len = 0;
        const wchar_t* raw = SafeGetStringRawBuffer(hStr, &len);
        bool match = false;
        if (raw && len > 0) {
            if ((targetCid[0] && WcsContainsI(raw, targetCid)) ||
                (targetMac[0] && MatchMacFlexible(raw, targetMac)) ||
                WcsContainsI(raw, L"Pro Controller") ||
                WcsContainsI(raw, L"057e")) {
                match = true;
            }
        }
        SafeDeleteString(hStr);

        AcquireSRWLockExclusive(&g_cacheLock);
        s_cachedThis = pThis;
        s_cachedResult = match;
        s_cachedTick = now;
        wcscpy(s_cachedCid, targetCid);
        wcscpy(s_cachedMac, targetMac);
        ReleaseSRWLockExclusive(&g_cacheLock);
        return match;
    }
    return false;
}

static HRESULT __fastcall Hook_get_ShowBatteryStatus(void* pThis, bool* outShow) {
    HRESULT hr = orig_get_ShowBatteryStatus(pThis, outShow);
    LONG bat = InterlockedCompareExchange(&g_SwitchBatteryLevel, 0, 0);
    if (bat >= 0 && outShow && IsProController(pThis)) {
        *outShow = true;
        return S_OK;
    }
    return hr;
}

static HRESULT __fastcall Hook_get_BatteryLife(void* pThis, int* outBat) {
    HRESULT hr = orig_get_BatteryLife(pThis, outBat);
    LONG bat = InterlockedCompareExchange(&g_SwitchBatteryLevel, 0, 0);
    if (bat >= 0 && outBat && IsProController(pThis)) {
        *outBat = (int)bat;
        return S_OK;
    }
    return hr;
}

static void TryHookDataModel(HMODULE hMod) {
    if (!hMod || InterlockedExchange(&g_hooksApplied, 1) != 0) return;
    BYTE* base = (BYTE*)hMod;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    BYTE* textStart = base + sec[0].VirtualAddress;
    size_t textSize = sec[0].Misc.VirtualSize;

    BYTE* fnBatteryLife = nullptr;
    BYTE* fnShowBattery = nullptr;

    for (size_t i = 0; i + 45 < textSize; i++) {
        BYTE* p = textStart + i;
        if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x5C && p[3] == 0x24 && p[4] == 0x10 &&
            p[5] == 0x57 && p[6] == 0x48 && p[7] == 0x83 && p[8] == 0xEC && p[9] == 0x20 &&
            p[29] == 0x33 && p[30] == 0xDB && p[31] == 0x89 && p[32] == 0x1A &&
            p[33] == 0x48 && p[34] == 0x8B && p[35] == 0x01 && p[36] == 0x48 && p[37] == 0x8B && p[38] == 0x80 &&
            *(DWORD*)(p + 39) == 0x00000210) {
            fnBatteryLife = p;
            break;
        }
    }

    for (size_t i = 0; i + 45 < textSize; i++) {
        BYTE* p = textStart + i;
        if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x5C && p[3] == 0x24 && p[4] == 0x10 &&
            p[5] == 0x57 && p[6] == 0x48 && p[7] == 0x83 && p[8] == 0xEC && p[9] == 0x20 &&
            p[29] == 0x33 && p[30] == 0xDB && p[31] == 0x88 && p[32] == 0x1A &&
            p[33] == 0x48 && p[34] == 0x8B && p[35] == 0x01 && p[36] == 0x48 && p[37] == 0x8B && p[38] == 0x80 &&
            *(DWORD*)(p + 39) == 0x00000180) {
            if (!fnShowBattery || (fnBatteryLife && llabs((long long)(p - fnBatteryLife)) < llabs((long long)(fnShowBattery - fnBatteryLife)))) {
                fnShowBattery = p;
            }
        }
    }

    if (fnShowBattery && fnBatteryLife) {
        g_hookShowBattery.Install(fnShowBattery, (void*)Hook_get_ShowBatteryStatus, (void**)&orig_get_ShowBatteryStatus);
        g_hookBatteryLife.Install(fnBatteryLife, (void*)Hook_get_BatteryLife, (void**)&orig_get_BatteryLife);
    }
}

typedef struct _MY_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} MY_UNICODE_STRING;

typedef struct _MY_LDR_DLL_LOADED_NOTIFICATION_DATA {
    ULONG Flags;
    const MY_UNICODE_STRING* FullDllName;
    const MY_UNICODE_STRING* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} MY_LDR_DLL_LOADED_NOTIFICATION_DATA;

typedef union _MY_LDR_DLL_NOTIFICATION_DATA {
    MY_LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
    MY_LDR_DLL_LOADED_NOTIFICATION_DATA Unloaded;
} MY_LDR_DLL_NOTIFICATION_DATA;

typedef VOID (CALLBACK *PLDR_DLL_NOTIFICATION_FUNCTION)(
    ULONG NotificationReason,
    const MY_LDR_DLL_NOTIFICATION_DATA* NotificationData,
    PVOID Context
);
typedef LONG (NTAPI *LdrRegisterDllNotification_t)(
    ULONG Flags,
    PLDR_DLL_NOTIFICATION_FUNCTION NotificationFunction,
    PVOID Context,
    PVOID* Cookie
);
typedef LONG (NTAPI *LdrUnregisterDllNotification_t)(PVOID Cookie);

static VOID CALLBACK OnDllNotification(
    ULONG NotificationReason,
    const MY_LDR_DLL_NOTIFICATION_DATA* NotificationData,
    PVOID)
{
    if (NotificationReason != 1 || !NotificationData || !NotificationData->Loaded.BaseDllName) return;
    const MY_UNICODE_STRING* base = NotificationData->Loaded.BaseDllName;
    static const wchar_t kTarget[] = L"DeviceFlows.DataModel.dll";
    constexpr USHORT kTargetChars = 25;
    if (!base->Buffer || base->Length != kTargetChars * sizeof(wchar_t)) return;
    for (USHORT i = 0; i < kTargetChars; i++) {
        if (AsciiLower(base->Buffer[i]) != AsciiLower(kTarget[i])) return;
    }
    if (g_dllLoadedEvent) SetEvent(g_dllLoadedEvent);
}

static DWORD WINAPI WatcherThread(LPVOID) {
    HMODULE hDM = GetModuleHandleW(L"DeviceFlows.DataModel.dll");
    if (hDM) {
        TryHookDataModel(hDM);
        return 0;
    }

    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    auto fnReg = hNtdll ? (LdrRegisterDllNotification_t)GetProcAddress(hNtdll, "LdrRegisterDllNotification") : nullptr;
    auto fnUnreg = hNtdll ? (LdrUnregisterDllNotification_t)GetProcAddress(hNtdll, "LdrUnregisterDllNotification") : nullptr;

    PVOID cookie = nullptr;
    if (fnReg && fnUnreg && g_dllLoadedEvent && fnReg(0, OnDllNotification, NULL, &cookie) == 0 && cookie) {
        for (;;) {
            hDM = GetModuleHandleW(L"DeviceFlows.DataModel.dll");
            if (hDM) {
                TryHookDataModel(hDM);
                break;
            }
            HANDLE waits[2] = { g_stopEvent, g_dllLoadedEvent };
            DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0) break;
            if (!GetModuleHandleW(L"DeviceFlows.DataModel.dll")) {
                if (WaitForSingleObject(g_stopEvent, 5) != WAIT_TIMEOUT) break;
            }
        }
        fnUnreg(cookie);
        return 0;
    }

    // Fallback polling if LdrRegisterDllNotification is unavailable
    for (;;) {
        hDM = GetModuleHandleW(L"DeviceFlows.DataModel.dll");
        if (hDM) {
            TryHookDataModel(hDM);
            break;
        }
        if (WaitForSingleObject(g_stopEvent, 100) != WAIT_TIMEOUT) break;
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpvReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        g_dllLoadedEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_watcherThread = CreateThread(NULL, 0, WatcherThread, NULL, 0, NULL);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        if (g_stopEvent) SetEvent(g_stopEvent);
        if (lpvReserved == NULL) {
            if (g_watcherThread) {
                WaitForSingleObject(g_watcherThread, 1000);
                CloseHandle(g_watcherThread);
                g_watcherThread = NULL;
            }
            g_hookShowBattery.Uninstall();
            g_hookBatteryLife.Uninstall();
            if (g_dllLoadedEvent) CloseHandle(g_dllLoadedEvent);
            if (g_stopEvent) CloseHandle(g_stopEvent);
        }
    }
    return TRUE;
}
