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

    std::string WorldChrMan = "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88";
    std::vector<uintptr_t> HpOffsets = {0x10EF8, 0x0, 0x190, 0x0, 0x138};
    uintptr_t ptr = memory.FindPtrFromAOB(WorldChrMan);
    std::cout << "Pointer found at: 0x" << std::hex << ptr << std::endl;

    uintptr_t HpAddress = memory.ResolvePointerChain(ptr, HpOffsets);
    std::cout << "Hp found at: 0x" << std::hex << HpAddress << std::endl;

    int32_t Hp;
    while (true) {
        memory.ReadInt32(HpAddress, Hp);
        std::cout << "Hp: " << std::dec << Hp << std::endl;
        Sleep(1000);
    }
    // system("pause");
    return 0;
}
