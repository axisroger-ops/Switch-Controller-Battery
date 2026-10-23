// SwitchControllerBatteryHook.cpp - Ultra-lightweight WinRT ABI Hook DLL (~14 KB, Zero dependencies)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winstring.h>
#include <stdlib.h>
#include <wchar.h>

#pragma comment(lib, "RuntimeObject.lib")

// Exported shared state updated directly by SwitchControllerBattery.exe via WriteProcessMemory
extern "C" __declspec(dllexport) volatile LONG g_SwitchBatteryLevel = -1;

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
static HANDLE g_watcherThread = NULL;

static bool IsProController(void* pThis) {
    if (!pThis || IsBadReadPtr(pThis, sizeof(void*))) return false;

    // Fast cache: avoid re-allocating HSTRING for repeated calls on the same device object
    static void* s_cachedThis = nullptr;
    static bool  s_cachedResult = false;
    static DWORD s_cachedTick = 0;
    DWORD now = GetTickCount();
    if (pThis == s_cachedThis && (now - s_cachedTick < 1500)) {
        return s_cachedResult;
    }

    void** vtable = *(void***)pThis;
    if (!vtable || IsBadReadPtr(vtable, sizeof(void*) * 10)) return false;

    auto fnGetId = (IDeviceBase_get_Id_t)vtable[6];
    if (!fnGetId) return false;

    HSTRING hStr = NULL;
    if (SUCCEEDED(fnGetId(pThis, &hStr)) && hStr) {
        UINT32 len = 0;
        const wchar_t* raw = WindowsGetStringRawBuffer(hStr, &len);
        bool match = false;
        if (raw && len > 0) {
            if (wcsstr(raw, L"b3834368") || wcsstr(raw, L"B3834368") ||
                wcsstr(raw, L"DC68EBF2289D") || wcsstr(raw, L"dc68ebf2289d") ||
                wcsstr(raw, L"Pro Controller") ||
                wcsstr(raw, L"057e") || wcsstr(raw, L"057E")) {
                match = true;
            }
        }
        WindowsDeleteString(hStr);

        s_cachedThis = pThis;
        s_cachedResult = match;
        s_cachedTick = now;
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

static DWORD WINAPI WatcherThread(LPVOID) {
    for (;;) {
        HMODULE hDM = GetModuleHandleW(L"DeviceFlows.DataModel.dll");
        if (hDM) {
            TryHookDataModel(hDM);
            break;
        }
        if (WaitForSingleObject(g_stopEvent, 100) != WAIT_TIMEOUT) break;
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        g_watcherThread = CreateThread(NULL, 0, WatcherThread, NULL, 0, NULL);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        if (g_stopEvent) SetEvent(g_stopEvent);
        if (g_watcherThread) {
            WaitForSingleObject(g_watcherThread, 1000);
            CloseHandle(g_watcherThread);
        }
        if (g_stopEvent) CloseHandle(g_stopEvent);
        g_hookShowBattery.Uninstall();
        g_hookBatteryLife.Uninstall();
    }
    return TRUE;
}
