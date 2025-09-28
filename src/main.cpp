#include "process_memory.h"
#include "utils.h"
#include <iostream>
#include <psapi.h>
#include <tlhelp32.h>
#include <windows.h>

int main() {
    if (!IsRunAsAdmin()) {
        std::cout << "ERROR: Must run as Administrator!" << std::endl;
        system("pause");
        return 1;
    }

    ProcessMemory memory("eldenring.exe");
    if (memory.Initialize()) {
        std::cout << "Process memory initialized successfully!" << std::endl;
    } else {
        std::cout << "Failed to initialize process memory!" << std::endl;
    }

    std::string pattern = "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88";
    uintptr_t ptr = memory.FindPtrFromAOB(pattern);
    std::cout << "Pointer found at: 0x" << std::hex << ptr << std::endl;
    system("pause");
    return 0;
}
