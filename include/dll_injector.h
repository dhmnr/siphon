#pragma once
#include <TlHelp32.h>
#include <Windows.h>
#include <Psapi.h>
#include <string>


DWORD GetProcessIdByName(const wchar_t *processName) {
    DWORD processId = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W entry = {0};
    entry.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName) == 0) {
                processId = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return processId;
}

bool InjectDLL(DWORD processId, const char *dllPath) {
    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                      PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                                  FALSE, processId);

    if (!hProcess) {
        return false;
    }

    size_t dllPathSize = strlen(dllPath) + 1;
    LPVOID pDllPath =
        VirtualAllocEx(hProcess, nullptr, dllPathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!pDllPath) {
        CloseHandle(hProcess);
        return false;
    }

    if (!WriteProcessMemory(hProcess, pDllPath, dllPath, dllPathSize, nullptr)) {
        VirtualFreeEx(hProcess, pDllPath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) {
        VirtualFreeEx(hProcess, pDllPath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    LPVOID pLoadLibrary = (LPVOID)GetProcAddress(hKernel32, "LoadLibraryA");
    if (!pLoadLibrary) {
        VirtualFreeEx(hProcess, pDllPath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, (LPTHREAD_START_ROUTINE)pLoadLibrary,
                                        pDllPath, 0, nullptr);

    if (!hThread) {
        VirtualFreeEx(hProcess, pDllPath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProcess, pDllPath, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    return exitCode != 0;
}

// Check if a DLL is already loaded in a process
bool IsDllLoadedInProcess(DWORD processId, const char *dllPath) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (!hProcess) {
        return false;
    }

    // Get the DLL filename from the full path
    std::string dllPathStr(dllPath);
    size_t lastSlash = dllPathStr.find_last_of("\\/");
    std::string dllName = (lastSlash != std::string::npos) ? dllPathStr.substr(lastSlash + 1) : dllPathStr;

    HMODULE modules[1024];
    DWORD bytesNeeded;
    bool found = false;

    if (EnumProcessModules(hProcess, modules, sizeof(modules), &bytesNeeded)) {
        DWORD moduleCount = bytesNeeded / sizeof(HMODULE);
        for (DWORD i = 0; i < moduleCount; i++) {
            char moduleName[MAX_PATH];
            if (GetModuleBaseNameA(hProcess, modules[i], moduleName, sizeof(moduleName))) {
                if (_stricmp(moduleName, dllName.c_str()) == 0) {
                    found = true;
                    break;
                }
            }
        }
    }

    CloseHandle(hProcess);
    return found;
}