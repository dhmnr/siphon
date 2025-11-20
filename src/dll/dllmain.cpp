#include "shared_memory.h"
#include <MinHook.h>
#include <Psapi.h>
#include <Windows.h>
#include <cstdint>
#include <stdio.h>

SharedMemory g_sharedMem;
static bool g_initialized = false;
static bool g_hookInstalled = false;

typedef void *(__fastcall *GetTargetedNpcFunc)(void *param_1);
GetTargetedNpcFunc g_originalFunc = nullptr;

// Hook function stays the same
void *__fastcall DetourGetTargetedNpc(void *param_1) {
    void *npcPointer = g_originalFunc(param_1);

    if (g_sharedMem.data) {
        g_sharedMem.data->npcPointer = npcPointer;
    }

    return npcPointer;
}

DWORD WINAPI InitThread(LPVOID param) {
    Sleep(1000);

    // Check if already initialized
    if (g_initialized) {
        // Already initialized, just wait for/reuse shared memory
        if (!g_sharedMem.data) {
            if (!g_sharedMem.OpenShared()) {
                MessageBoxA(nullptr, "Failed to open existing shared memory", "Error", MB_OK);
                return 1;
            }
        }
        return 0;
    }

    // Create shared memory
    if (!g_sharedMem.CreateShared()) {
        // If creation fails, try opening existing shared memory
        if (!g_sharedMem.OpenShared()) {
            MessageBoxA(nullptr, "Failed to create or open shared memory", "Error", MB_OK);
            return 1;
        }
    }

    // Wait for executable to provide the hook address
    char waitMsg[256];
    sprintf_s(waitMsg, "Waiting for hook address from executable...");

    for (int i = 0; i < 100; i++) { // Wait up to 10 seconds
        if (g_sharedMem.data->hookReady && g_sharedMem.data->hookAddress != 0) {
            break;
        }
        Sleep(100);
    }

    if (!g_sharedMem.data->hookReady || g_sharedMem.data->hookAddress == 0) {
        MessageBoxA(nullptr, "Executable didn't provide hook address in time", "Error", MB_OK);
        return 1;
    }

    // Get the address from shared memory
    uintptr_t functionAddr = g_sharedMem.data->hookAddress;

    // char msg[256];
    // sprintf_s(msg, "Received hook address: 0x%llX\nInstalling hook...", functionAddr);
    // MessageBoxA(nullptr, msg, "Info", MB_OK);

    // Initialize MinHook (check if already initialized)
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        MessageBoxA(nullptr, "MinHook init failed", "Error", MB_OK);
        return 1;
    }

    // Check if hook is already installed at this address
    if (g_hookInstalled) {
        return 0;
    }

    // Create hook
    if (MH_CreateHook((LPVOID)functionAddr, &DetourGetTargetedNpc,
                      reinterpret_cast<LPVOID *>(&g_originalFunc)) != MH_OK) {
        MessageBoxA(nullptr, "Failed to create hook", "Error", MB_OK);
        MH_Uninitialize();
        return 1;
    }

    // Enable hook
    if (MH_EnableHook((LPVOID)functionAddr) != MH_OK) {
        MessageBoxA(nullptr, "Failed to enable hook", "Error", MB_OK);
        MH_Uninitialize();
        return 1;
    }

    g_hookInstalled = true;
    g_initialized = true;

    // MessageBoxA(nullptr, "Hook installed successfully!", "Success", MB_OK);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // Only create init thread if not already initialized
        if (!g_initialized) {
            CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        }
        break;

    case DLL_PROCESS_DETACH:
        if (g_hookInstalled) {
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
        }
        g_sharedMem.Close();
        g_initialized = false;
        g_hookInstalled = false;
        break;
    }
    return TRUE;
}