#include <iostream>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

DWORD FindProcessByName(const std::string& processName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    
    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);
    
    if (Process32First(snapshot, &pe32)) {
        do {
            if (processName == pe32.szExeFile) {
                CloseHandle(snapshot);
                return pe32.th32ProcessID;
            }
        } while (Process32Next(snapshot, &pe32));
    }
    
    CloseHandle(snapshot);
    return 0;
}

int main() {
    std::cout << "=== Process Finder ===" << std::endl;
    
    DWORD pid = FindProcessByName("eldenring.exe");
    if (pid == 0) {
        std::cout << "eldenring.exe not found! Make sure the game is running." << std::endl;
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
        return 1;
    }
    
    std::cout << "Found eldenring.exe (PID: " << pid << ")" << std::endl;
    
    HANDLE processHandle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!processHandle) {
        std::cout << "Failed to open process. Error: " << GetLastError() << std::endl;
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
        return 1;
    }
    
    // Get base address
    HMODULE hMods[1024];
    DWORD cbNeeded;
    uintptr_t baseAddress = 0;
    
    if (EnumProcessModules(processHandle, hMods, sizeof(hMods), &cbNeeded)) {
        baseAddress = (uintptr_t)hMods[0];
        std::cout << "Base Address: 0x" << std::hex << baseAddress << std::dec << std::endl;
    } else {
        std::cout << "Failed to get base address. Error: " << GetLastError() << std::endl;
    }
    
    std::cout << "Successfully attached to eldenring.exe!" << std::endl;
    
    CloseHandle(processHandle);
    
    std::cout << "\nPress Enter to exit...";
    std::cin.get();
    return 0;
}