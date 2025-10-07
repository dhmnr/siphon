#include "shared_memory.h"
#include <MinHook.h>
#include <Psapi.h>
#include <Windows.h>
#include <cstdint>

SharedMemory g_sharedMem;

typedef void *(__fastcall *GetTargetedNpcFunc)(void *param_1);
GetTargetedNpcFunc g_originalFunc = nullptr;

uintptr_t PatternScan(const char *pattern, const char *mask) {
    MODULEINFO moduleInfo;
    GetModuleInformation(GetCurrentProcess(), GetModuleHandle(nullptr), &moduleInfo,
                         sizeof(MODULEINFO));

    uintptr_t base = (uintptr_t)moduleInfo.lpBaseOfDll;
    size_t size = moduleInfo.SizeOfImage;
    size_t patternLen = strlen(mask);

    for (size_t i = 0; i < size - patternLen; i++) {
        bool found = true;
        for (size_t j = 0; j < patternLen; j++) {
            if (mask[j] != '?' && pattern[j] != *(char *)(base + i + j)) {
                found = false;
                break;
            }
        }
        if (found) {
            return base + i;
        }
    }

    return 0;
}

void *__fastcall DetourGetTargetedNpc(void *param_1) {
    void *npcPointer = g_originalFunc(param_1);

    if (g_sharedMem.data) {
        g_sharedMem.data->npcPointer = npcPointer;
    }

    return npcPointer;
}

DWORD WINAPI InitThread(LPVOID param) {
    Sleep(2000);

    if (!g_sharedMem.CreateShared()) {
        MessageBoxA(nullptr, "Failed to create shared memory", "Error", MB_OK);
        return 1;
    }

    const char pattern[] = "\x48\x8B\x48\x08\x49\x89\x8D";
    const char mask[] = "xxxxxxx";

    uintptr_t patternAddr = PatternScan(pattern, mask);
    if (!patternAddr) {
        MessageBoxA(nullptr, "Pattern not found!", "Error", MB_OK);
        return 1;
    }

    uintptr_t callAddr = patternAddr - 5;
    int32_t relativeOffset = *(int32_t *)(callAddr + 1);
    uintptr_t functionAddr = patternAddr + relativeOffset;

    if (MH_Initialize() != MH_OK) {
        MessageBoxA(nullptr, "MinHook init failed", "Error", MB_OK);
        return 1;
    }

    if (MH_CreateHook((LPVOID)functionAddr, &DetourGetTargetedNpc,
                      reinterpret_cast<LPVOID *>(&g_originalFunc)) != MH_OK) {
        MessageBoxA(nullptr, "Failed to create hook", "Error", MB_OK);
        MH_Uninitialize();
        return 1;
    }

    if (MH_EnableHook((LPVOID)functionAddr) != MH_OK) {
        MessageBoxA(nullptr, "Failed to enable hook", "Error", MB_OK);
        MH_Uninitialize();
        return 1;
    }

    MessageBoxA(nullptr, "Hook installed successfully!", "Success", MB_OK);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_sharedMem.Close();
        break;
    }
    return TRUE;
}